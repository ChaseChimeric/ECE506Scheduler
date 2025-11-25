#include "schedrt/accelerator.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>

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

} // namespace

int main(int argc, char** argv) {
    Options opts;
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

    std::cout << "[static-probe] Static shell load requests completed successfully.\n"
              << "Check 'dmesg' for the corresponding fpga_manager status.\n";
    return 0;
}
