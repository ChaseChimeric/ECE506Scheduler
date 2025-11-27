#include "fpga_loader_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {

struct Options {
    bool show_help = false;
    bool dry_run = false;
    std::string static_bit = "bitstreams/top_reconfig_wrapper.bin";
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

    fpga::FpgaManagerClient manager(opts.manager_node,
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

    fpga::DecoupleController decoupler(opts.dry_run);
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
