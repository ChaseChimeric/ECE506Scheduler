#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct Options {
    bool show_help = false;
    bool dry_run = false;
    std::string static_bit = "bitstreams/top_reconfig_wrapper.bit";
    std::string partial_bit;
    std::string manager_node = "/sys/class/fpga_manager/fpga0/firmware";
    std::string firmware_dir = "/lib/firmware";
    uint64_t gpio_base = 0x41200000u;
    size_t gpio_span = 0x1000;
    std::chrono::milliseconds timeout{5000};
};

bool parse_u64(const std::string& text, uint64_t& value) {
    try {
        size_t idx = 0;
        value = std::stoull(text, &idx, 0);
        return idx == text.size();
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_options(int argc, char** argv, Options& opts) {
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            opts.show_help = true;
            return true;
        }
        if (arg == "--dry-run") {
            opts.dry_run = true;
            continue;
        }
        auto pos = arg.find('=');
        if (pos == std::string::npos) {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
        std::string key = arg.substr(0, pos);
        std::string value = arg.substr(pos + 1);
        if (key == "--static") {
            opts.static_bit = value;
        } else if (key == "--partial") {
            opts.partial_bit = value;
        } else if (key == "--manager") {
            opts.manager_node = value;
        } else if (key == "--firmware-dir") {
            opts.firmware_dir = value;
        } else if (key == "--gpio-base") {
            if (!parse_u64(value, opts.gpio_base)) {
                std::cerr << "Failed to parse --gpio-base value\n";
                return false;
            }
        } else if (key == "--gpio-span") {
            uint64_t span = 0;
            if (!parse_u64(value, span)) {
                std::cerr << "Failed to parse --gpio-span value\n";
                return false;
            }
            opts.gpio_span = static_cast<size_t>(span);
        } else if (key == "--wait-ms") {
            uint64_t ms = 0;
            if (!parse_u64(value, ms)) {
                std::cerr << "Failed to parse --wait-ms value\n";
                return false;
            }
            opts.timeout = std::chrono::milliseconds(ms);
        } else {
            std::cerr << "Unknown option: " << key << "\n";
            return false;
        }
    }
    return true;
}

void print_usage() {
    std::cout << "Usage: fpga_loader [--static=PATH] [--partial=PATH] [options]\n"
                 "\n"
                 "Options:\n"
                 "  --static=PATH        Static bitstream to load first\n"
                 "  --partial=PATH       Optional partial bitstream to load after static\n"
                 "  --manager=PATH       fpga_manager firmware node (default /sys/.../firmware)\n"
                 "  --firmware-dir=DIR   Directory fpga_manager searches for bitstreams (/lib/firmware)\n"
                 "  --gpio-base=ADDR     Physical base address of AXI GPIO controlling decouple\n"
                 "  --gpio-span=BYTES    Span to map from gpio-base (default 0x1000)\n"
                 "  --wait-ms=MS         Timeout waiting for fpga_manager state transitions (5000ms)\n"
                 "  --dry-run            Log actions without touching hardware (for host testing)\n"
                 "  -h, --help           Show this message\n";
}

class DecoupleController {
public:
    explicit DecoupleController(bool dry_run) : dry_run_(dry_run) {}
    ~DecoupleController() { close(); }

    bool open(uint64_t phys_addr, size_t span) {
        if (opened_) return true;
        if (dry_run_) {
            opened_ = true;
            return true;
        }
        page_size_ = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
        if (page_size_ == 0) page_size_ = 4096;
        off_t page_base = static_cast<off_t>(phys_addr & ~static_cast<uint64_t>(page_size_ - 1));
        size_t page_offset = static_cast<size_t>(phys_addr - static_cast<uint64_t>(page_base));
        map_len_ = ((page_offset + span + page_size_ - 1) / page_size_) * page_size_;
        fd_ = ::open("/dev/mem", O_RDWR | O_SYNC);
        if (fd_ < 0) {
            std::cerr << "[fpga_loader] Failed to open /dev/mem: " << std::strerror(errno) << "\n";
            return false;
        }
        map_base_ = ::mmap(nullptr, map_len_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, page_base);
        if (map_base_ == MAP_FAILED) {
            std::cerr << "[fpga_loader] mmap failed: " << std::strerror(errno) << "\n";
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        regs_ = reinterpret_cast<volatile uint32_t*>(static_cast<uint8_t*>(map_base_) + page_offset);
        // Channel 1 data @ 0x0, tri-state @ 0x4.
        regs_[1] = 0x0; // drive as outputs
        opened_ = true;
        return true;
    }

    bool set(bool asserted) {
        if (!opened_) return false;
        current_value_ = asserted;
        if (dry_run_) return true;
        regs_[0] = asserted ? 0x1 : 0x0;
        (void)regs_[0]; // readback to ensure write posts
        return true;
    }

    void close() {
        if (dry_run_) {
            opened_ = false;
            return;
        }
        if (map_base_ && map_base_ != MAP_FAILED) {
            ::munmap(map_base_, map_len_);
            map_base_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        regs_ = nullptr;
        opened_ = false;
    }

private:
    bool dry_run_;
    bool opened_ = false;
    bool current_value_ = false;
    int fd_ = -1;
    size_t page_size_ = 0;
    void* map_base_ = nullptr;
    size_t map_len_ = 0;
    volatile uint32_t* regs_ = nullptr;
};

class FpgaManagerClient {
public:
    FpgaManagerClient(std::string firmware_node,
                      std::string flags_node,
                      std::string state_node,
                      std::string staging_dir,
                      bool dry_run)
        : firmware_node_(std::move(firmware_node)),
          flags_node_(std::move(flags_node)),
          state_node_(std::move(state_node)),
          staging_dir_(std::move(staging_dir)),
          dry_run_(dry_run) {}

    bool load_bitstream(const std::string& source, bool partial,
                        std::chrono::milliseconds timeout) const {
        std::string firmware_name;
        if (!stage_file(source, firmware_name)) return false;
        bool flags_set = false;
        if (partial && !flags_node_.empty()) {
            if (!set_flags(1)) return false;
            flags_set = true;
        }
        bool ok = request_firmware(firmware_name);
        if (flags_set) {
            if (!set_flags(0)) return false;
        }
        if (!ok) return false;
        return wait_for_completion(timeout);
    }

private:
    bool stage_file(const std::string& source, std::string& firmware_name) const {
        namespace fs = std::filesystem;
        fs::path src(source);
        if (!fs::exists(src)) {
            std::cerr << "[fpga_loader] Missing bitstream: " << source << "\n";
            return false;
        }
        firmware_name = src.filename().string();
        if (dry_run_) return true;

        fs::path target_dir = staging_dir_.empty() ? src.parent_path() : fs::path(staging_dir_);
        if (target_dir.empty()) target_dir = fs::current_path();
        fs::path dest = target_dir / firmware_name;

        std::error_code ec;
        if (fs::equivalent(src, dest, ec)) {
            return true; // already staged
        }
        ec.clear();
        fs::create_directories(target_dir, ec);
        if (ec) {
            std::cerr << "[fpga_loader] Failed to create firmware dir '" << target_dir
                      << "': " << ec.message() << "\n";
            return false;
        }
        ec.clear();
        fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "[fpga_loader] Failed to copy " << src << " -> " << dest
                      << ": " << ec.message() << "\n";
            return false;
        }
        std::cout << "[fpga_loader] Staged " << src << " -> " << dest << "\n";
        return true;
    }

    bool set_flags(uint32_t value) const {
        if (dry_run_ || flags_node_.empty()) return true;
        std::ofstream ofs(flags_node_);
        if (!ofs) {
            std::cerr << "[fpga_loader] Unable to open " << flags_node_ << ": "
                      << std::strerror(errno) << "\n";
            return false;
        }
        ofs << value << std::endl;
        if (!ofs.good()) {
            std::cerr << "[fpga_loader] Failed to write flags " << value << "\n";
            return false;
        }
        return true;
    }

    bool request_firmware(const std::string& firmware_name) const {
        std::cout << "[fpga_loader] Programming " << firmware_name << "\n";
        if (dry_run_) return true;
        std::ofstream ofs(firmware_node_);
        if (!ofs) {
            std::cerr << "[fpga_loader] Unable to open " << firmware_node_ << ": "
                      << std::strerror(errno) << "\n";
            return false;
        }
        ofs << firmware_name << std::endl;
        if (!ofs.good()) {
            std::cerr << "[fpga_loader] Failed to request firmware " << firmware_name << "\n";
            return false;
        }
        return true;
    }

    std::string read_state() const {
        if (dry_run_ || state_node_.empty()) return "dry-run";
        std::ifstream ifs(state_node_);
        if (!ifs) return {};
        std::string line;
        std::getline(ifs, line);
        return line;
    }

    bool wait_for_completion(std::chrono::milliseconds timeout) const {
        if (dry_run_ || state_node_.empty()) return true;
        auto start = std::chrono::steady_clock::now();
        std::string last_state;
        while (std::chrono::steady_clock::now() - start < timeout) {
            std::string state = read_state();
            if (state.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (state != last_state) {
                std::cout << "[fpga_loader] fpga_manager state -> " << state << "\n";
                last_state = state;
            }
            if (state.find("error") != std::string::npos) {
                std::cerr << "[fpga_loader] fpga_manager reported error\n";
                return false;
            }
            if (state.find("operating") != std::string::npos ||
                state.find("unknown") != std::string::npos ||
                state.find("user") != std::string::npos) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cerr << "[fpga_loader] Timeout waiting for fpga_manager completion\n";
        return false;
    }

    std::string firmware_node_;
    std::string flags_node_;
    std::string state_node_;
    std::string staging_dir_;
    bool dry_run_;
};

} // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!parse_options(argc, argv, opts)) {
        print_usage();
        return 1;
    }
    if (opts.show_help) {
        print_usage();
        return 0;
    }
    if (opts.static_bit.empty()) {
        std::cerr << "Static bitstream path is required (use --static=...)\n";
        return 1;
    }

    namespace fs = std::filesystem;
    fs::path firmware_node(opts.manager_node);
    fs::path manager_dir = firmware_node.parent_path();
    fs::path flags_node = manager_dir / "flags";
    fs::path state_node = manager_dir / "state";

    FpgaManagerClient manager(opts.manager_node,
                              flags_node.string(),
                              state_node.string(),
                              opts.firmware_dir,
                              opts.dry_run);

    std::cout << "[fpga_loader] Loading static shell: " << opts.static_bit << "\n";
    if (!manager.load_bitstream(opts.static_bit, /*partial=*/false, opts.timeout)) {
        return 1;
    }
    if (opts.partial_bit.empty()) {
        std::cout << "[fpga_loader] Static bitstream loaded. No partial requested.\n";
        return 0;
    }

    DecoupleController decoupler(opts.dry_run);
    if (!decoupler.open(opts.gpio_base, opts.gpio_span)) {
        std::cerr << "[fpga_loader] Failed to map AXI GPIO at 0x" << std::hex << opts.gpio_base
                  << std::dec << "\n";
        return 1;
    }

    std::cout << "[fpga_loader] Asserting DFX decouple via AXI GPIO\n";
    decoupler.set(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cout << "[fpga_loader] Loading partial: " << opts.partial_bit << "\n";
    bool ok = manager.load_bitstream(opts.partial_bit, /*partial=*/true, opts.timeout);

    std::cout << "[fpga_loader] Releasing DFX decouple\n";
    decoupler.set(false);

    return ok ? 0 : 1;
}
