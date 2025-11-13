#include "apps/app_interface.hpp"
#include "dash/fft.hpp"
#include "dash/provider.hpp"
#include "dash/zip.hpp"
#include "schedrt/accelerator.hpp"
#include "schedrt/application_registry.hpp"
#include "schedrt/scheduler.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

struct OverlaySpec {
    std::string app;
    unsigned count = 1;
};

struct DashOptions {
    std::vector<OverlaySpec> overlays;
    unsigned cpu_workers = 4;
    unsigned preload_threshold = 3;
    std::string fpga_manager_path = "/sys/class/fpga_manager/fpga0/firmware";
    std::string bitstream_dir = "bitstreams";
    bool fpga_mock = true;
};

unsigned parse_unsigned(const std::string& text, unsigned fallback) {
    if (text.empty()) return fallback;
    unsigned value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return fallback;
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    return value > 0 ? value : fallback;
}

DashOptions parse_options(int argc, char** argv) {
    DashOptions opts;
    for (int i = 0; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--fpga-real") {
            opts.fpga_mock = false;
            continue;
        }
        if (arg == "--fpga-mock") {
            opts.fpga_mock = true;
            continue;
        }
        if (arg.rfind("--fpga-manager=", 0) == 0) {
            opts.fpga_manager_path = arg.substr(sizeof("--fpga-manager=") - 1);
            continue;
        }
        if (arg.rfind("--bitstream-dir=", 0) == 0) {
            opts.bitstream_dir = arg.substr(sizeof("--bitstream-dir=") - 1);
            continue;
        }
        if (arg.rfind("--overlay=", 0) == 0) {
            std::string spec = arg.substr(sizeof("--overlay=") - 1);
            auto colon = spec.find(':');
            OverlaySpec overlay;
            overlay.app = colon == std::string::npos ? spec : spec.substr(0, colon);
            if (colon != std::string::npos && colon + 1 < spec.size()) {
                overlay.count = parse_unsigned(spec.substr(colon + 1), overlay.count);
            }
            if (!overlay.app.empty()) opts.overlays.push_back(overlay);
            continue;
        }
        if (arg.rfind("--cpu-workers=", 0) == 0) {
            opts.cpu_workers = parse_unsigned(arg.substr(sizeof("--cpu-workers=") - 1), opts.cpu_workers);
            continue;
        }
        if (arg.rfind("--preload-threshold=", 0) == 0) {
            opts.preload_threshold =
                parse_unsigned(arg.substr(sizeof("--preload-threshold=") - 1), opts.preload_threshold);
            continue;
        }
    }
    if (opts.overlays.empty()) {
        opts.overlays.push_back({"zip", 2});
        opts.overlays.push_back({"fft", 1});
    }
    return opts;
}

DashOptions g_opts;

} // namespace

using namespace schedrt;

extern "C" void app_initialize(int argc, char** argv, ApplicationRegistry& reg, Scheduler& sched) {
    g_opts = parse_options(argc, argv);
    auto make_desc = [&](const std::string& app, ResourceKind kind) {
        schedrt::AppDescriptor desc{};
        desc.app = app;
        desc.kernel_name = app + "_kernel";
        desc.kind = kind;
        std::filesystem::path base(g_opts.bitstream_dir);
        desc.bitstream_path = (base / (app + "_partial.bit")).string();
        return desc;
    };
    reg.register_app(make_desc("zip", ResourceKind::ZIP));
    reg.register_app(make_desc("fft", ResourceKind::FFT));

    unsigned next_slot_id = 0;
    unsigned provider_instance = 0;
    std::unordered_set<std::string> cpu_registered;

    for (const auto& overlay : g_opts.overlays) {
        auto descOpt = reg.lookup(overlay.app);
        if (!descOpt) {
            std::cerr << "Warning: unknown overlay '" << overlay.app << "'; skipping\n";
            continue;
        }
        if (overlay.count > 0) {
            for (unsigned i = 0; i < overlay.count; ++i) {
                FpgaSlotOptions slot_opts{g_opts.fpga_manager_path, g_opts.fpga_mock};
                sched.add_accelerator(make_fpga_slot(next_slot_id++, slot_opts));
                dash::register_provider({overlay.app, descOpt->kind, provider_instance++, 0});
            }
        }
        if (cpu_registered.insert(overlay.app).second) {
            dash::register_provider({overlay.app, ResourceKind::CPU, provider_instance++, 10});
        }
    }
    for (const auto* op : {"zip", "fft"}) {
        if (cpu_registered.insert(op).second) {
            dash::register_provider({op, ResourceKind::CPU, provider_instance++, 10});
        }
    }

    sched.add_accelerator(make_cpu_mock(0));
}

extern "C" int app_run(int argc, char** argv, Scheduler& sched) {
    (void)argc;
    (void)argv;

    {
        dash::ZipParams zp{3, dash::ZipMode::Compress};
        char inbuf[1024], outbuf[2048];
        size_t out_actual = 0;
        bool ok = dash::zip_execute(zp, {inbuf, sizeof(inbuf)}, {outbuf, sizeof(outbuf)}, out_actual);
        std::cout << "zip_execute -> " << (ok ? "OK" : "FAIL") << "\n";
    }
    {
        dash::FftPlan plan{1024, false};
        float inbuf[1024], outbuf[1024];
        bool ok = dash::fft_execute(plan, {inbuf, sizeof(inbuf)}, {outbuf, sizeof(outbuf)});
        std::cout << "fft_execute -> " << (ok ? "OK" : "FAIL") << "\n";
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return 0;
}
