#include "schedrt/accelerator.hpp"

#include <chrono>
#include <cerrno>
#include <csignal>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
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
    bool trace_all = false;
    bool load_overlay = false;
    std::string overlay_label = "fft_passthrough";
    std::string overlay_bitstream = "bitstreams/fft_passthrough_partial.bin";
    bool run_loopback = false;
    std::string dma_device = "/dev/axi_dma_regs";
    std::string udmabuf = "udmabuf0";
    size_t loopback_bytes = 256 * 1024;
    unsigned dma_timeout_ms = 100;
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
              << "  --overlay=label[:bitstream]  also request a partial overlay load\n"
              << "  --mmio-probe=name:base[:span]    dump registers from /dev/mem after load\n"
              << "  --mmio-probe-offset=name:offset  add register offset to that probe\n"
              << "  --run-loopback               kick a DMA udmabuf loopback after load\n"
              << "  --udmabuf=name               override udmabuf device (default udmabuf0)\n"
              << "  --dma-device=/dev/axi_dma_regs  char device for AXI DMA registers\n"
              << "  --bytes=N                    bytes to copy during loopback (default 256KiB)\n"
              << "  --dma-timeout-ms=N           timeout per DMA channel (default 100ms)\n"
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

bool parse_size(const std::string& text, size_t* out) {
    if (text.empty()) return false;
    try {
        *out = static_cast<size_t>(std::stoull(text, nullptr, 0));
        return true;
    } catch (...) {
        return false;
    }
}

bool read_uint64_file(const std::string& path, uint64_t* out) {
    std::ifstream ifs(path);
    if (!ifs) return false;
    std::string text;
    ifs >> text;
    try {
        *out = std::stoull(text, nullptr, 0);
    } catch (...) {
        return false;
    }
    return true;
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
                  << std::hex << probe.base << " span=0x" << probe.span << std::dec << std::endl;
        for (uint32_t offset : probe.offsets) {
            if (offset >= probe.span) {
                std::cout << "    offset 0x" << std::hex << offset
                          << " outside span 0x" << probe.span << std::dec << std::endl;
                continue;
            }
            uint32_t value = regs[offset / sizeof(uint32_t)];
            std::cout << "    [0x" << std::hex << offset << "] = 0x"
                      << value << std::dec << std::endl;
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

class UdmabufRegion {
public:
    bool init(const std::string& name) {
        std::string base = "/sys/class/u-dma-buf/" + name;
        if (!read_uint64_file(base + "/size", &size_)) {
            std::cerr << "[static-probe] failed to read udmabuf size for " << name << "\n";
            return false;
        }
        if (!read_uint64_file(base + "/phys_addr", &phys_)) {
            std::cerr << "[static-probe] failed to read udmabuf phys addr for " << name << "\n";
            return false;
        }
        std::string dev_path = "/dev/" + name;
        fd_ = ::open(dev_path.c_str(), O_RDWR | O_SYNC);
        if (fd_ < 0) {
            std::perror(("[static-probe] open " + dev_path).c_str());
            return false;
        }
        void* map = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (map == MAP_FAILED) {
            std::perror("[static-probe] udmabuf mmap");
            close(fd_);
            fd_ = -1;
            return false;
        }
        virt_ = static_cast<uint8_t*>(map);
        return true;
    }

    ~UdmabufRegion() {
        if (virt_) munmap(virt_, size_);
        if (fd_ >= 0) close(fd_);
    }

    uint8_t* virt() const { return virt_; }
    size_t size() const { return static_cast<size_t>(size_); }
    uint64_t phys() const { return phys_; }

private:
    int fd_{-1};
    uint8_t* virt_{nullptr};
    uint64_t size_{0};
    uint64_t phys_{0};
};

class DmaDevice {
public:
    explicit DmaDevice(std::string path) : path_(std::move(path)) {}
    bool open_rw() {
        fd_ = ::open(path_.c_str(), O_RDWR);
        if (fd_ < 0) {
            std::perror(("[static-probe] open " + path_).c_str());
            return false;
        }
        return true;
    }
    ~DmaDevice() {
        if (fd_ >= 0) close(fd_);
    }

    void write(off_t offset, uint32_t value) const {
        if (pwrite(fd_, &value, sizeof(value), offset) != sizeof(value)) {
            std::perror("[static-probe] dma pwrite");
        }
    }
    uint32_t read(off_t offset) const {
        uint32_t value = 0;
        if (pread(fd_, &value, sizeof(value), offset) != sizeof(value)) {
            std::perror("[static-probe] dma pread");
        }
        return value;
    }

private:
    std::string path_;
    int fd_{-1};
};

constexpr off_t MM2S_DMACR = 0x00;
constexpr off_t MM2S_DMASR = 0x04;
constexpr off_t MM2S_SA = 0x18;
constexpr off_t MM2S_SA_MSB = 0x1C;
constexpr off_t MM2S_LENGTH = 0x28;
constexpr off_t S2MM_DMACR = 0x30;
constexpr off_t S2MM_DMASR = 0x34;
constexpr off_t S2MM_DA = 0x48;
constexpr off_t S2MM_DA_MSB = 0x4C;
constexpr off_t S2MM_LENGTH = 0x58;
constexpr uint32_t DMA_CR_RUNSTOP = 0x1;
constexpr uint32_t DMA_CR_IOC_IrqEn = 0x10;
constexpr uint32_t DMA_CR_ERR_IrqEn = 0x40;
constexpr uint32_t DMA_SR_IDLE = 0x2;
constexpr uint32_t DMA_SR_ERR_MASK = (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) |
                                     (1u << 12) | (1u << 13) | (1u << 14);

bool wait_for_idle(const DmaDevice& dev, off_t status_reg, unsigned timeout_ms, const char* tag) {
    const unsigned polls = timeout_ms * 4;
    for (unsigned i = 0; i < polls; ++i) {
        auto status = dev.read(status_reg);
        if (status & DMA_SR_ERR_MASK) {
            std::cerr << "[static-probe] " << tag << " error status=0x"
                      << std::hex << status << std::dec << "\n";
            return false;
        }
        if (status & DMA_SR_IDLE) return true;
        std::this_thread::sleep_for(std::chrono::microseconds(250));
    }
    std::cerr << "[static-probe] " << tag << " timeout status=0x"
              << std::hex << dev.read(status_reg) << std::dec << "\n";
    return false;
}

bool run_dma_loopback(const Options& opts) {
    UdmabufRegion buf;
    if (!buf.init(opts.udmabuf)) return false;
    size_t half = buf.size() / 2;
    if (half == 0) {
        std::cerr << "[static-probe] udmabuf too small\n";
        return false;
    }
    size_t bytes = opts.loopback_bytes ? opts.loopback_bytes : half;
    if (bytes > half) {
        std::cerr << "[static-probe] requested bytes exceed half the buffer (" << half << ")\n";
        return false;
    }

    uint8_t* in = buf.virt();
    uint8_t* out = buf.virt() + half;
    for (size_t i = 0; i < bytes; ++i) in[i] = static_cast<uint8_t>(i & 0xFF);
    std::memset(out, 0, bytes);

    DmaDevice dev(opts.dma_device);
    if (!dev.open_rw()) return false;

    auto clear_status = [&](bool s2mm) {
        dev.write(s2mm ? S2MM_DMASR : MM2S_DMASR, 0xFFFFFFFF);
    };
    clear_status(true);
    clear_status(false);

    dev.write(S2MM_DMACR, DMA_CR_RUNSTOP | DMA_CR_IOC_IrqEn | DMA_CR_ERR_IrqEn);
    dev.write(S2MM_DA, static_cast<uint32_t>(buf.phys() + half));
    dev.write(S2MM_DA_MSB, static_cast<uint32_t>((buf.phys() + half) >> 32));
    dev.write(S2MM_LENGTH, static_cast<uint32_t>(bytes));

    dev.write(MM2S_DMACR, DMA_CR_RUNSTOP | DMA_CR_IOC_IrqEn | DMA_CR_ERR_IrqEn);
    dev.write(MM2S_SA, static_cast<uint32_t>(buf.phys()));
    dev.write(MM2S_SA_MSB, static_cast<uint32_t>(buf.phys() >> 32));
    dev.write(MM2S_LENGTH, static_cast<uint32_t>(bytes));

    bool mm2s_ok = wait_for_idle(dev, MM2S_DMASR, opts.dma_timeout_ms, "mm2s");
    bool s2mm_ok = wait_for_idle(dev, S2MM_DMASR, opts.dma_timeout_ms, "s2mm");
    uint32_t mm2s_sr = dev.read(MM2S_DMASR);
    uint32_t s2mm_sr = dev.read(S2MM_DMASR);
    std::cout << "[static-probe] DMA mm2s_sr=0x" << std::hex << mm2s_sr
              << " s2mm_sr=0x" << s2mm_sr << std::dec << std::endl;

    if (!mm2s_ok || !s2mm_ok) {
        std::cerr << "[static-probe] DMA transfer did not complete\n";
        return false;
    }

    size_t mismatches = 0;
    for (size_t i = 0; i < bytes; ++i) {
        if (in[i] != out[i]) {
            if (mismatches < 8) {
                std::cerr << "[static-probe] mismatch @" << i
                          << " in=0x" << std::hex << static_cast<int>(in[i])
                          << " out=0x" << static_cast<int>(out[i]) << std::dec << "\n";
            }
            ++mismatches;
        }
    }
    if (mismatches > 0) {
        std::cerr << "[static-probe] loopback detected " << mismatches << " mismatches\n";
        return false;
    }
    std::cout << "[static-probe] DMA loopback SUCCESS (" << bytes << " bytes)" << std::endl;
    return true;
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
        if (arg == "--trace-all") {
            opts.trace_all = true;
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
        if (arg.rfind("--overlay=", 0) == 0) {
            auto parts = split_colon(arg.substr(sizeof("--overlay=") - 1));
            if (parts.empty() || parts[0].empty()) {
                std::cerr << "Invalid --overlay spec: " << arg << "\n";
                return 1;
            }
            opts.load_overlay = true;
            opts.overlay_label = parts[0];
            if (parts.size() > 1 && !parts[1].empty()) {
                opts.overlay_bitstream = parts[1];
            }
            continue;
        }
        if (arg == "--run-loopback") {
            opts.run_loopback = true;
            continue;
        }
        if (arg.rfind("--udmabuf=", 0) == 0) {
            opts.udmabuf = arg.substr(sizeof("--udmabuf=") - 1);
            continue;
        }
        if (arg.rfind("--dma-device=", 0) == 0) {
            opts.dma_device = arg.substr(sizeof("--dma-device=") - 1);
            continue;
        }
        if (arg.rfind("--bytes=", 0) == 0) {
            size_t val = 0;
            if (!parse_size(arg.substr(sizeof("--bytes=") - 1), &val)) {
                std::cerr << "Invalid value for --bytes\n";
                return 1;
            }
            opts.loopback_bytes = val;
            continue;
        }
        if (arg.rfind("--dma-timeout-ms=", 0) == 0) {
            auto val = parse_unsigned(arg.substr(sizeof("--dma-timeout-ms=") - 1));
            if (!val) {
                std::cerr << "Invalid value for --dma-timeout-ms\n";
                return 1;
            }
            opts.dma_timeout_ms = *val;
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

    if (opts.trace_all) {
        std::cout.setf(std::ios::unitbuf);
        setenv("SCHEDRT_TRACE", "1", 1);
        setenv("SCHEDRT_DMA_DEBUG", "1", 1);
        std::cout << "[static-probe] trace-all enabled (fpga + DMA verbose logging)" << std::endl;
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
        std::cout << "[static-probe] Using host-visible bitstream at " << host_path->string() << std::endl;
    }

    std::optional<std::filesystem::path> overlay_host;
    if (opts.load_overlay) {
        overlay_host = resolve_bitstream_host_path(opts.overlay_bitstream);
        if (!overlay_host) {
            std::cerr << "[static-probe] Overlay bitstream not found: " << opts.overlay_bitstream;
            if (!std::filesystem::path(opts.overlay_bitstream).is_absolute()) {
                std::cerr << " (also checked /lib/firmware/" << opts.overlay_bitstream << ")";
            }
            std::cerr << "\n";
            return 1;
        } else if (opts.fpga_debug) {
            std::cout << "[static-probe] Using overlay bitstream at " << overlay_host->string() << std::endl;
        }
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
                  << slot_opts.static_bitstream << std::endl;
        schedrt::FpgaSlotAccelerator slot(iter, slot_opts);
        if (!slot.prepare_static()) {
            std::cerr << "[static-probe] Static shell load failed on attempt "
                      << (iter + 1) << "\n";
            return 1;
        }
        if (opts.load_overlay) {
            schedrt::AppDescriptor desc;
            desc.app = opts.overlay_label;
            desc.kernel_name = opts.overlay_label + "_kernel";
            desc.bitstream_path = opts.overlay_bitstream;
            desc.kind = schedrt::ResourceKind::FFT;
            if (!slot.ensure_app_loaded(desc)) {
                std::cerr << "[static-probe] Failed to load overlay "
                          << desc.app << " on attempt " << (iter + 1) << "\n";
                return 1;
            }
        }
    }

    if (!mmio_probes.empty()) {
        if (!run_mmio_probes(mmio_probes)) {
            return 1;
        }
    }

    if (opts.run_loopback) {
        if (!run_dma_loopback(opts)) {
            return 1;
        }
    }

    std::cout << "[static-probe] Static shell load requests completed successfully." << std::endl
              << "Check 'dmesg' for the corresponding fpga_manager status." << std::endl;
    return 0;
}
