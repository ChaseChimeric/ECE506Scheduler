#include "schedrt/accelerator.hpp"

#include <cerrno>
#include <csignal>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

namespace {

struct Options {
    std::string static_bitstream = "bitstreams/static_wrapper.bin";
    std::string fpga_manager = "/sys/class/fpga_manager/fpga0/firmware";
    bool fpga_real = false;
    bool fpga_debug = false;
    int pr_gpio = -1;
    bool pr_gpio_active_low = false;
    unsigned pr_gpio_delay_ms = 5;
    unsigned repetitions = 1;
    std::vector<uint32_t> mmio_offsets_default{0x0, 0x4, 0x8, 0xC};
};

struct MmioProbe {
    std::string label;
    uintptr_t base = 0;
    size_t span = 0x1000;
    std::vector<uint32_t> offsets;
};

class SigbusGuard {
public:
    explicit SigbusGuard(std::string desc) : desc_(std::move(desc)) {}

    template <typename Fn>
    bool run(Fn&& fn) {
        install();
        current_ = this;
        if (sigsetjmp(env_, 1) != 0) {
            current_ = nullptr;
            return false;
        }
        bool ok = fn();
        current_ = nullptr;
        return ok;
    }

private:
    static void install() {
        std::call_once(flag_, [] {
            struct sigaction act{};
            act.sa_sigaction = &SigbusGuard::dispatch;
            sigemptyset(&act.sa_mask);
            act.sa_flags = SA_SIGINFO;
            sigaction(SIGBUS, &act, &prev_);
            have_prev_ = true;
        });
    }

    static void dispatch(int sig, siginfo_t* info, void* ctx) {
        if (current_) {
            std::cerr << "[static-probe] SIGBUS during " << current_->desc_;
            if (info && info->si_addr) {
                std::cerr << " (bad addr=0x"
                          << std::hex << reinterpret_cast<std::uintptr_t>(info->si_addr)
                          << std::dec << ")";
            }
            std::cerr << "\n";
            siglongjmp(current_->env_, 1);
            return;
        }
        if (have_prev_) {
            if (prev_.sa_flags & SA_SIGINFO) {
                if (prev_.sa_sigaction) {
                    prev_.sa_sigaction(sig, info, ctx);
                    return;
                }
            } else if (prev_.sa_handler == SIG_IGN) {
                return;
            } else if (prev_.sa_handler == SIG_DFL) {
                signal(SIGBUS, SIG_DFL);
                raise(SIGBUS);
                return;
            } else if (prev_.sa_handler) {
                prev_.sa_handler(sig);
                return;
            }
        }
        signal(SIGBUS, SIG_DFL);
        raise(SIGBUS);
    }

    static inline std::once_flag flag_;
    static inline struct sigaction prev_{};
    static inline bool have_prev_{false};
    static inline SigbusGuard* current_{nullptr};

    sigjmp_buf env_{};
    std::string desc_;
};

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "  --static-bitstream=PATH      bitstream (.bin) to load as the static shell\n"
              << "  --fpga-manager=PATH          sysfs path to the fpga_manager firmware entry\n"
              << "  --fpga-real / --fpga-mock    actually write to fpga_manager (default mock)\n"
              << "  --fpga-debug                 enable verbose accelerator logging\n"
              << "  --fpga-pr-gpio=N             PR decouple GPIO to toggle during load\n"
              << "  --fpga-pr-gpio-active-low    treat PR GPIO as active-low\n"
              << "  --fpga-pr-gpio-delay-ms=N    delay between GPIO toggles (default 5)\n"
              << "  --mmio-probe=name:base[:span]    dump registers from /dev/mem after load\n"
              << "  --mmio-probe-offset=name:offset  add register offset to that probe\n"
              << "  --repeat=N                   number of times to reload the static shell\n"
              << "  --help                       show this message\n";
}

std::optional<unsigned> parse_unsigned(const std::string& text) {
    if (text.empty()) return std::nullopt;
    try {
        unsigned long val = std::stoul(text, nullptr, 0);
        if (val == 0) return std::nullopt;
        return static_cast<unsigned>(val);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int> parse_int(const std::string& text) {
    if (text.empty()) return std::nullopt;
    try {
        return static_cast<int>(std::stol(text, nullptr, 0));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::filesystem::path> resolve_bitstream_host_path(const std::string& request) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path direct(request);
    if (fs::exists(direct, ec)) return fs::weakly_canonical(direct, ec);
    if (!direct.is_absolute()) {
        fs::path fallback = fs::path("/lib/firmware") / direct;
        if (fs::exists(fallback, ec)) return fs::weakly_canonical(fallback, ec);
    }
    return std::nullopt;
}

std::vector<std::string> split_colon(const std::string& spec) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= spec.size()) {
        size_t pos = spec.find(':', start);
        if (pos == std::string::npos) pos = spec.size();
        out.emplace_back(spec.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

std::optional<MmioProbe> parse_mmio_probe(const std::string& text) {
    auto parts = split_colon(text);
    if (parts.size() < 2) return std::nullopt;
    MmioProbe probe;
    probe.label = parts[0];
    try {
        probe.base = static_cast<uintptr_t>(std::stoull(parts[1], nullptr, 0));
    } catch (...) {
        return std::nullopt;
    }
    if (parts.size() > 2 && !parts[2].empty()) {
        try {
            probe.span = static_cast<size_t>(std::stoull(parts[2], nullptr, 0));
        } catch (...) {
            return std::nullopt;
        }
    }
    if (probe.span == 0) probe.span = 0x1000;
    return probe;
}

MmioProbe* find_probe(std::vector<MmioProbe>& probes, const std::string& label) {
    for (auto& probe : probes) {
        if (probe.label == label) return &probe;
    }
    return nullptr;
}

bool run_mmio_probe(const MmioProbe& probe) {
    std::ostringstream desc;
    desc << "mmio probe '" << probe.label << "' base=0x" << std::hex << probe.base;
    SigbusGuard guard(desc.str());
    return guard.run([&]() -> bool {
        int fd = ::open("/dev/mem", O_RDONLY | O_SYNC);
        if (fd < 0) {
            std::cerr << "[static-probe] mmio-probe(" << probe.label
                      << ") failed to open /dev/mem: " << strerror(errno) << "\n";
            return false;
        }
        void* map = mmap(nullptr, probe.span, PROT_READ, MAP_SHARED, fd, probe.base);
        if (map == MAP_FAILED) {
            std::cerr << "[static-probe] mmio-probe(" << probe.label << ") mmap failed: "
                      << strerror(errno) << "\n";
            close(fd);
            return false;
        }
        auto* regs = static_cast<volatile uint32_t*>(map);
        std::cout << "[static-probe] MMIO probe '" << probe.label << "' base=0x"
                  << std::hex << probe.base << " span=0x" << probe.span << std::dec << "\n";
        for (uint32_t offset : probe.offsets) {
            if (offset >= probe.span) {
                std::cout << "    offset 0x" << std::hex << offset
                          << " outside span 0x" << probe.span << std::dec << "\n";
                continue;
            }
            uint32_t value = regs[offset / sizeof(uint32_t)];
            std::cout << "    [0x" << std::hex << offset << "] = 0x"
                      << value << std::dec << "\n";
        }
        munmap(map, probe.span);
        close(fd);
        return true;
    });
}

bool run_mmio_probes(const std::vector<MmioProbe>& probes) {
    bool ok = true;
    for (const auto& probe : probes) {
        if (!run_mmio_probe(probe)) ok = false;
    }
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    Options opts;
    std::vector<MmioProbe> mmio_probes;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--fpga-real") {
            opts.fpga_real = true;
            continue;
        }
        if (arg == "--fpga-mock") {
            opts.fpga_real = false;
            continue;
        }
        if (arg == "--fpga-debug") {
            opts.fpga_debug = true;
            continue;
        }
        if (arg == "--fpga-pr-gpio-active-low") {
            opts.pr_gpio_active_low = true;
            continue;
        }
        if (arg.rfind("--static-bitstream=", 0) == 0) {
            opts.static_bitstream = arg.substr(sizeof("--static-bitstream=") - 1);
            continue;
        }
        if (arg.rfind("--fpga-manager=", 0) == 0) {
            opts.fpga_manager = arg.substr(sizeof("--fpga-manager=") - 1);
            continue;
        }
        if (arg.rfind("--fpga-pr-gpio=", 0) == 0) {
            auto val = parse_int(arg.substr(sizeof("--fpga-pr-gpio=") - 1));
            if (!val) {
                std::cerr << "Invalid value for --fpga-pr-gpio\n";
                return 1;
            }
            opts.pr_gpio = *val;
            continue;
        }
        if (arg.rfind("--fpga-pr-gpio-delay-ms=", 0) == 0) {
            auto val = parse_unsigned(arg.substr(sizeof("--fpga-pr-gpio-delay-ms=") - 1));
            if (!val) {
                std::cerr << "Invalid value for --fpga-pr-gpio-delay-ms\n";
                return 1;
            }
            opts.pr_gpio_delay_ms = *val;
            continue;
        }
        if (arg.rfind("--repeat=", 0) == 0) {
            auto val = parse_unsigned(arg.substr(sizeof("--repeat=") - 1));
            if (!val) {
                std::cerr << "Invalid value for --repeat\n";
                return 1;
            }
            opts.repetitions = *val;
            continue;
        }
        if (arg.rfind("--mmio-probe=", 0) == 0) {
            auto probe = parse_mmio_probe(arg.substr(sizeof("--mmio-probe=") - 1));
            if (!probe) {
                std::cerr << "Invalid --mmio-probe spec: " << arg << "\n";
                return 1;
            }
            if (probe->label.empty()) {
                std::cerr << "MMIO probe label cannot be empty\n";
                return 1;
            }
            if (probe->offsets.empty()) {
                probe->offsets = opts.mmio_offsets_default;
            }
            mmio_probes.push_back(*probe);
            continue;
        }
        if (arg.rfind("--mmio-probe-offset=", 0) == 0) {
            auto parts = split_colon(arg.substr(sizeof("--mmio-probe-offset=") - 1));
            if (parts.size() != 2) {
                std::cerr << "Invalid --mmio-probe-offset spec: " << arg << "\n";
                return 1;
            }
            auto* probe = find_probe(mmio_probes, parts[0]);
            if (!probe) {
                std::cerr << "Unknown probe label '" << parts[0] << "' for " << arg << "\n";
                return 1;
            }
            try {
                uint32_t offset = static_cast<uint32_t>(std::stoul(parts[1], nullptr, 0));
                probe->offsets.push_back(offset);
            } catch (...) {
                std::cerr << "Invalid offset in " << arg << "\n";
                return 1;
            }
            continue;
        }
        std::cerr << "Unknown option: " << arg << "\n";
        print_usage(argv[0]);
        return 1;
    }

    if (!opts.fpga_real) {
        std::cerr << "[static-probe] Refusing to load static shell without --fpga-real\n";
        return 1;
    }

    auto host_path = resolve_bitstream_host_path(opts.static_bitstream);
    if (opts.static_bitstream.empty() || !host_path) {
        std::cerr << "[static-probe] Static bitstream not found: " << opts.static_bitstream;
        if (!std::filesystem::path(opts.static_bitstream).is_absolute()) {
            std::cerr << " (also checked /lib/firmware/" << opts.static_bitstream << ")";
        }
        std::cerr << "\n";
        return 1;
    } else if (opts.fpga_debug) {
        std::cout << "[static-probe] Using host-visible bitstream at " << host_path->string() << "\n";
    }

    schedrt::FpgaSlotOptions slot_opts;
    slot_opts.manager_path = opts.fpga_manager;
    slot_opts.mock_mode = !opts.fpga_real;
    slot_opts.static_bitstream = opts.static_bitstream;
    slot_opts.debug_logging = opts.fpga_debug;
    slot_opts.pr_gpio_number = opts.pr_gpio;
    slot_opts.pr_gpio_active_low = opts.pr_gpio_active_low;
    slot_opts.pr_gpio_delay_ms = opts.pr_gpio_delay_ms;

    for (unsigned iter = 0; iter < opts.repetitions; ++iter) {
        std::cout << "[static-probe] Attempt " << (iter + 1) << " of "
                  << opts.repetitions << ": loading "
                  << slot_opts.static_bitstream << "\n";
        schedrt::FpgaSlotAccelerator slot(iter, slot_opts);
        if (!slot.prepare_static()) {
            std::cerr << "[static-probe] Static shell load failed on attempt "
                      << (iter + 1) << "\n";
            return 1;
        }
    }

    if (!mmio_probes.empty()) {
        if (!run_mmio_probes(mmio_probes)) {
            return 1;
        }
    }

    std::cout << "[static-probe] Static shell load requests completed successfully.\n"
              << "Check 'dmesg' for the corresponding fpga_manager status.\n";
    return 0;
}
