#include "dash/contexts.hpp"
#include "schedrt/accelerator.hpp"
#include "schedrt/task.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>
#include <cctype>

using schedrt::AppDescriptor;
using schedrt::FpgaSlotAccelerator;
using schedrt::FpgaSlotOptions;
using schedrt::ResourceKind;
using schedrt::Task;

namespace {

struct OverlaySpec {
    std::string app;
    unsigned count = 1;
    std::string bitstream_path;
};

enum class FftPattern { Impulse, Sine, Ramp, Random };

struct Config {
    std::string fpga_manager = "/sys/class/fpga_manager/fpga0/firmware";
    std::string static_bitstream = "bitstreams/static_wrapper.bit";
    std::string bitstream_dir = "bitstreams";
    bool fpga_real = false;
    bool fpga_debug = false;
    bool run_fft = false;
    unsigned fft_iterations = 1;
    size_t fft_length = 1024;
    bool fft_inverse = false;
    FftPattern fft_pattern = FftPattern::Impulse;
    bool fft_dump = false;
    std::optional<std::string> udmabuf_name;
    std::optional<std::string> dma_base;
    bool dma_debug = false;
};

struct SlotInstance {
    unsigned slot_id = 0;
    AppDescriptor desc;
    std::unique_ptr<FpgaSlotAccelerator> slot;
};

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "  --overlay=name[:count][:bitstream]   select overlays to load (default fft:1)\n"
              << "  --bitstream-dir=DIR                  where <app>_partial.bit is resolved\n"
              << "  --static-bitstream=PATH              static shell bitstream\n"
              << "  --fpga-manager=PATH                  sysfs path for fpga manager firmware entry\n"
              << "  --fpga-real / --fpga-mock            actually touch the manager (default mock)\n"
              << "  --fpga-debug                         enable verbose accelerator logging\n"
              << "  --udmabuf=name                       override udmabuf device (default udmabuf0)\n"
              << "  --dma-base=0xADDR                    override AXI DMA base address\n"
              << "  --dma-debug                          enable verbose DMA logs\n"
              << "  --run-fft                            execute FFT overlay diagnostic after load\n"
              << "  --fft-length=N                       complex samples per iteration (default 1024)\n"
              << "  --fft-iters=N                        iterations to run when --run-fft is set\n"
              << "  --fft-pattern=impulse|sine|ramp|random\n"
              << "  --fft-inverse                        request inverse FFT mode\n"
              << "  --fft-dump                           dump first few FFT outputs per iteration\n"
              << "  --help                               show this message\n";
}

ResourceKind resource_for_app(const std::string& app) {
    if (app == "zip") return ResourceKind::ZIP;
    if (app == "fft") return ResourceKind::FFT;
    if (app == "fir") return ResourceKind::FIR;
    return ResourceKind::CPU;
}

unsigned parse_unsigned(const std::string& text, unsigned fallback) {
    if (text.empty()) return fallback;
    try {
        unsigned long val = std::stoul(text, nullptr, 0);
        if (val == 0) return fallback;
        return static_cast<unsigned>(val);
    } catch (...) {
        return fallback;
    }
}

size_t parse_size(const std::string& text, size_t fallback) {
    if (text.empty()) return fallback;
    try {
        unsigned long long val = std::stoull(text, nullptr, 0);
        if (val == 0) return fallback;
        return static_cast<size_t>(val);
    } catch (...) {
        return fallback;
    }
}

std::vector<std::string> split_colon(const std::string& spec) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= spec.size()) {
        auto pos = spec.find(':', start);
        if (pos == std::string::npos) pos = spec.size();
        out.emplace_back(spec.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

std::filesystem::path default_overlay_bitstream(const Config& cfg, const std::string& app) {
    std::filesystem::path base(cfg.bitstream_dir);
    base /= app + "_partial.bit";
    return base;
}

std::optional<OverlaySpec> parse_overlay(const std::string& text, const Config& cfg) {
    if (text.empty()) return std::nullopt;
    auto parts = split_colon(text);
    OverlaySpec spec;
    spec.app = parts[0];
    if (parts.size() > 1 && !parts[1].empty()) {
        spec.count = parse_unsigned(parts[1], spec.count);
    }
    if (parts.size() > 2 && !parts[2].empty()) {
        spec.bitstream_path = parts[2];
    }
    if (spec.bitstream_path.empty()) {
        spec.bitstream_path = default_overlay_bitstream(cfg, spec.app).string();
    }
    return spec;
}

FftPattern parse_fft_pattern(const std::string& text, FftPattern fallback) {
    std::string lower;
    lower.reserve(text.size());
    std::transform(text.begin(), text.end(), std::back_inserter(lower), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower == "impulse") return FftPattern::Impulse;
    if (lower == "sine" || lower == "sin") return FftPattern::Sine;
    if (lower == "ramp") return FftPattern::Ramp;
    if (lower == "random" || lower == "noise") return FftPattern::Random;
    return fallback;
}

void configure_fft_env(const Config& cfg) {
    if (cfg.udmabuf_name) {
        setenv("SCHEDRT_UDMABUF", cfg.udmabuf_name->c_str(), 1);
    }
    if (cfg.dma_base) {
        setenv("SCHEDRT_DMA_BASE", cfg.dma_base->c_str(), 1);
    }
    if (cfg.dma_debug) {
        setenv("SCHEDRT_DMA_DEBUG", "1", 1);
    }
}

bool ensure_path_exists(const std::string& label, const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        std::cerr << "[tester] " << label << " missing: " << path << "\n";
        return false;
    }
    return true;
}

bool load_overlays(const std::vector<OverlaySpec>& overlays,
                   const Config& cfg,
                   std::vector<SlotInstance>& slots) {
    if (!ensure_path_exists("static bitstream", cfg.static_bitstream)) return false;
    FpgaSlotOptions opts;
    opts.manager_path = cfg.fpga_manager;
    opts.mock_mode = !cfg.fpga_real;
    opts.static_bitstream = cfg.static_bitstream;
    opts.debug_logging = cfg.fpga_debug;

    unsigned next_slot = 0;
    for (const auto& overlay : overlays) {
        if (!ensure_path_exists(overlay.app + " bitstream", overlay.bitstream_path)) {
            return false;
        }
        for (unsigned i = 0; i < overlay.count; ++i) {
            AppDescriptor desc;
            desc.app = overlay.app;
            desc.kernel_name = overlay.app + "_kernel";
            desc.bitstream_path = overlay.bitstream_path;
            desc.kind = resource_for_app(desc.app);

            auto slot = std::make_unique<FpgaSlotAccelerator>(next_slot++, opts);
            std::cout << "[tester] Preparing " << slot->name()
                      << " (app=" << desc.app << ")\n";
            if (!slot->prepare_static()) {
                std::cerr << "[tester] Failed to load static shell for " << slot->name() << "\n";
                return false;
            }
            if (!slot->ensure_app_loaded(desc)) {
                std::cerr << "[tester] Failed to load overlay " << desc.app
                          << " on " << slot->name() << "\n";
                return false;
            }

            slots.push_back(SlotInstance{
                .slot_id = slot->slot_id(),
                .desc = desc,
                .slot = std::move(slot),
            });
        }
    }
    return true;
}

void fill_fft_input(std::vector<float>& data,
                    FftPattern pattern,
                    size_t complex_len,
                    unsigned iter,
                    std::mt19937& rng) {
    std::fill(data.begin(), data.end(), 0.0f);
    switch (pattern) {
    case FftPattern::Impulse:
        data[0] = 1.0f;
        break;
    case FftPattern::Sine: {
        double freq = std::max<size_t>(1, (iter % std::max<size_t>(complex_len, 1)));
        constexpr double two_pi = 6.28318530717958647692;
        for (size_t i = 0; i < complex_len; ++i) {
            double angle = two_pi * freq * static_cast<double>(i) / static_cast<double>(complex_len);
            data[2 * i] = static_cast<float>(std::sin(angle));
        }
        break;
    }
    case FftPattern::Ramp:
        for (size_t i = 0; i < complex_len; ++i) {
            data[2 * i] = static_cast<float>((i % 1024) / 512.0 - 1.0);
        }
        break;
    case FftPattern::Random: {
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        for (size_t i = 0; i < complex_len * 2; ++i) {
            data[i] = dist(rng);
        }
        break;
    }
    }
}

void dump_fft_samples(const std::vector<float>& out, size_t complex_len) {
    size_t to_show = std::min<size_t>(complex_len, 8);
    std::cout << "    samples:";
    for (size_t i = 0; i < to_show; ++i) {
        float re = out[2 * i];
        float im = out[2 * i + 1];
        std::cout << " [" << i << "]=" << std::fixed << std::setprecision(4)
                  << re << "," << im;
    }
    std::cout << std::defaultfloat << "\n";
}

bool run_fft_diagnostic(SlotInstance& slot,
                        const Config& cfg) {
    if (cfg.fft_length == 0) {
        std::cerr << "[tester] fft-length must be > 0\n";
        return false;
    }
    std::cout << "[tester] Running FFT diagnostics on " << slot.slot->name()
              << " (" << cfg.fft_iterations << " iteration"
              << (cfg.fft_iterations == 1 ? "" : "s") << ")\n";
    std::vector<float> input(cfg.fft_length * 2);
    std::vector<float> output(cfg.fft_length * 2);
    std::mt19937 rng(0xC0FFEE);

    for (unsigned iter = 0; iter < cfg.fft_iterations; ++iter) {
        fill_fft_input(input, cfg.fft_pattern, cfg.fft_length, iter, rng);
        std::fill(output.begin(), output.end(), 0.0f);

        dash::FftContext ctx;
        ctx.plan.n = static_cast<int>(cfg.fft_length);
        ctx.plan.inverse = cfg.fft_inverse;
        ctx.in = {input.data(), input.size() * sizeof(float)};
        ctx.out = {output.data(), output.size() * sizeof(float)};

        Task task;
        task.id = 5000 + iter;
        task.app = slot.desc.app;
        task.required = ResourceKind::FFT;
        task.est_runtime_ns = std::chrono::nanoseconds(15000000);
        task.params.emplace(dash::kFftContextKey,
            std::to_string(reinterpret_cast<std::uintptr_t>(&ctx)));

        auto result = slot.slot->run(task, slot.desc);
        bool ok = result.ok && ctx.ok;
        std::cout << "  iter " << iter << ": "
                  << (ok ? "OK " : "FAIL ")
                  << result.message << " ("
                  << result.runtime_ns.count() << " ns)\n";
        if (cfg.fft_dump) {
            dump_fft_samples(output, cfg.fft_length);
        }
        if (!ok) return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Config cfg;
    std::vector<OverlaySpec> overlays;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg.rfind("--overlay=", 0) == 0) {
            auto spec = parse_overlay(arg.substr(sizeof("--overlay=") - 1), cfg);
            if (!spec) {
                std::cerr << "[tester] Failed to parse " << arg << "\n";
                return 1;
            }
            overlays.push_back(*spec);
            continue;
        }
        if (arg.rfind("--bitstream-dir=", 0) == 0) {
            cfg.bitstream_dir = arg.substr(sizeof("--bitstream-dir=") - 1);
            continue;
        }
        if (arg.rfind("--static-bitstream=", 0) == 0) {
            cfg.static_bitstream = arg.substr(sizeof("--static-bitstream=") - 1);
            continue;
        }
        if (arg.rfind("--fpga-manager=", 0) == 0) {
            cfg.fpga_manager = arg.substr(sizeof("--fpga-manager=") - 1);
            continue;
        }
        if (arg == "--fpga-real") {
            cfg.fpga_real = true;
            continue;
        }
        if (arg == "--fpga-mock") {
            cfg.fpga_real = false;
            continue;
        }
        if (arg == "--fpga-debug") {
            cfg.fpga_debug = true;
            continue;
        }
        if (arg.rfind("--udmabuf=", 0) == 0) {
            cfg.udmabuf_name = arg.substr(sizeof("--udmabuf=") - 1);
            continue;
        }
        if (arg.rfind("--dma-base=", 0) == 0) {
            cfg.dma_base = arg.substr(sizeof("--dma-base=") - 1);
            continue;
        }
        if (arg == "--dma-debug") {
            cfg.dma_debug = true;
            continue;
        }
        if (arg == "--run-fft") {
            cfg.run_fft = true;
            continue;
        }
        if (arg.rfind("--fft-length=", 0) == 0) {
            cfg.fft_length = parse_size(arg.substr(sizeof("--fft-length=") - 1), cfg.fft_length);
            continue;
        }
        if (arg.rfind("--fft-iters=", 0) == 0) {
            cfg.fft_iterations = parse_unsigned(arg.substr(sizeof("--fft-iters=") - 1), cfg.fft_iterations);
            continue;
        }
        if (arg.rfind("--fft-pattern=", 0) == 0) {
            cfg.fft_pattern = parse_fft_pattern(arg.substr(sizeof("--fft-pattern=") - 1), cfg.fft_pattern);
            continue;
        }
        if (arg == "--fft-inverse") {
            cfg.fft_inverse = true;
            continue;
        }
        if (arg == "--fft-dump") {
            cfg.fft_dump = true;
            continue;
        }
        std::cerr << "[tester] Unknown option: " << arg << "\n";
        print_usage(argv[0]);
        return 1;
    }

    if (overlays.empty()) {
        overlays.push_back(*parse_overlay("fft:1", cfg));
    }

    configure_fft_env(cfg);

    std::vector<SlotInstance> slots;
    if (!load_overlays(overlays, cfg, slots)) {
        return 1;
    }

    if (cfg.run_fft) {
        bool ran_fft = false;
        for (auto& slot : slots) {
            if (slot.desc.app != "fft") continue;
            if (!run_fft_diagnostic(slot, cfg)) {
                return 1;
            }
            ran_fft = true;
        }
        if (!ran_fft) {
            std::cerr << "[tester] No FFT overlays were configured; "
                      << "--run-fft has nothing to exercise\n";
        }
    } else {
        std::cout << "[tester] Skipping overlay execution (--run-fft not provided)\n";
    }

    return 0;
}
