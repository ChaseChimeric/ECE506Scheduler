#include "apps/app_interface.hpp"
#include "dash/provider.hpp"
#include "dash/scheduler_binding.hpp"
#include "schedrt/accelerator.hpp"
#include "schedrt/reporting.hpp"
#include "schedrt/scheduler.hpp"
#include "schedrt/application_registry.hpp"

#include <dlfcn.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace schedrt;

namespace {

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " --app-lib=PATH [--backend=auto|cpu|fpga] [--cpu-workers=N] "
              << "[--preload-threshold=N] -- [app args...]\n";
    std::cout << "  --csv-report          emit task lines as CSV (id,ok,msg,time_ns)\n";
    std::cout << "  --fpga-debug          enable verbose logging inside the FPGA accelerators\n";
}

BackendMode parse_backend(const std::string& value) {
    if (value == "cpu") return BackendMode::CPU;
    if (value == "fpga") return BackendMode::FPGA;
    return BackendMode::AUTO;
}

unsigned parse_unsigned(const std::string& value, unsigned default_value) {
    unsigned result = 0;
    for (char c : value) {
        if (c < '0' || c > '9') return default_value;
        result = result * 10 + static_cast<unsigned>(c - '0');
    }
    return result > 0 ? result : default_value;
}

} // namespace

struct OverlaySpec {
    std::string app;
    unsigned count = 1;
    std::string bitstream;
};

static ResourceKind resource_for_app(const std::string& app) {
    if (app == "zip") return ResourceKind::ZIP;
    if (app == "fft") return ResourceKind::FFT;
    if (app == "fir") return ResourceKind::FIR;
    return ResourceKind::CPU;
}

int main(int argc, char** argv) {
    std::string app_lib;
    BackendMode backend = BackendMode::AUTO;
    unsigned cpu_workers = std::thread::hardware_concurrency();
    if (cpu_workers == 0) cpu_workers = 4;
    unsigned preload_threshold = 3;
    bool csv_report = false;
    std::string bitstream_dir = "bitstreams";
    std::string static_bitstream = "bitstreams/static_wrapper.bit";
    std::string fpga_manager = "/sys/class/fpga_manager/fpga0/firmware";
    bool fpga_real = false;
    bool fpga_debug = false;
    std::vector<OverlaySpec> overlays;

    int app_arg_start = argc;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--") {
            app_arg_start = i + 1;
            break;
        }
        if (arg.rfind("--app-lib=", 0) == 0) {
            app_lib = arg.substr(sizeof("--app-lib=") - 1);
            continue;
        }
        if (arg.rfind("--backend=", 0) == 0) {
            backend = parse_backend(arg.substr(sizeof("--backend=") - 1));
            continue;
        }
        if (arg.rfind("--cpu-workers=", 0) == 0) {
            cpu_workers = parse_unsigned(arg.substr(sizeof("--cpu-workers=") - 1), cpu_workers);
            continue;
        }
        if (arg.rfind("--preload-threshold=", 0) == 0) {
            preload_threshold = parse_unsigned(arg.substr(sizeof("--preload-threshold=") - 1), preload_threshold);
            continue;
        }
        if (arg.rfind("--bitstream-dir=", 0) == 0) {
            bitstream_dir = arg.substr(sizeof("--bitstream-dir=") - 1);
            continue;
        }
        if (arg.rfind("--static-bitstream=", 0) == 0) {
            static_bitstream = arg.substr(sizeof("--static-bitstream=") - 1);
            continue;
        }
        if (arg.rfind("--fpga-manager=", 0) == 0) {
            fpga_manager = arg.substr(sizeof("--fpga-manager=") - 1);
            continue;
        }
        if (arg == "--fpga-real") {
            fpga_real = true;
            continue;
        }
        if (arg == "--fpga-mock") {
            fpga_real = false;
            continue;
        }
        if (arg == "--fpga-debug") {
            fpga_debug = true;
            continue;
        }
        if (arg.rfind("--overlay=", 0) == 0) {
            std::string spec = arg.substr(sizeof("--overlay=") - 1);
            std::vector<std::string> parts;
            size_t start = 0;
            while (start < spec.size()) {
                auto pos = spec.find(':', start);
                if (pos == std::string::npos) pos = spec.size();
                parts.push_back(spec.substr(start, pos - start));
                start = pos + 1;
            }
            if (!parts.empty()) {
                OverlaySpec overlay;
                overlay.app = parts[0];
                if (parts.size() > 1) overlay.count = parse_unsigned(parts[1], overlay.count);
                if (parts.size() > 2) overlay.bitstream = parts[2];
                overlays.push_back(overlay);
            }
            continue;
        }
        if (arg == "--csv-report") {
            csv_report = true;
            continue;
        }
        std::cerr << "Unknown option: " << arg << "\n";
        print_usage(argv[0]);
        return 1;
    }

    if (app_lib.empty()) {
        std::cerr << "Missing --app-lib=PATH\n";
        print_usage(argv[0]);
        return 1;
    }

    int app_argc = argc - app_arg_start;
    char** app_argv = argv + app_arg_start;

    if (overlays.empty()) {
        overlays.push_back({"zip", 2});
        overlays.push_back({"fft", 1});
        overlays.push_back({"fir", 1});
    }

    struct OverlayEntry {
        std::string app;
        ResourceKind kind;
        unsigned count;
        std::string bitstream;
    };
    std::vector<OverlayEntry> registered;
    ApplicationRegistry reg;
    std::filesystem::path base(bitstream_dir);
    for (const auto& overlay : overlays) {
        ResourceKind kind = resource_for_app(overlay.app);
        std::filesystem::path bit = overlay.bitstream.empty()
            ? base / (overlay.app + "_partial.bit")
            : base / overlay.bitstream;
        AppDescriptor desc{};
        desc.app = overlay.app;
        desc.kernel_name = overlay.app + "_kernel";
        desc.kind = kind;
        desc.bitstream_path = bit.string();
        reg.register_app(desc);
        registered.push_back({overlay.app, kind, overlay.count, desc.bitstream_path});
    }

    Scheduler sched(reg, backend, cpu_workers, preload_threshold);
    dash::set_scheduler(&sched);

    unsigned next_slot_id = 0;
    unsigned provider_instance = 0;
    std::unordered_set<std::string> cpu_registered;
    for (const auto& desc : registered) {
        for (unsigned i = 0; i < desc.count; ++i) {
            FpgaSlotOptions opts{fpga_manager, !fpga_real};
            opts.static_bitstream = static_bitstream;
            opts.debug_logging = fpga_debug;
            sched.add_accelerator(make_fpga_slot(next_slot_id++, opts));
            dash::register_provider({desc.app, desc.kind, provider_instance++, 0});
        }
        if (cpu_registered.insert(desc.app).second) {
            dash::register_provider({desc.app, ResourceKind::CPU, provider_instance++, 10});
        }
    }
    if (cpu_registered.insert("zip").second) {
        dash::register_provider({"zip", ResourceKind::CPU, provider_instance++, 10});
    }
    if (cpu_registered.insert("fft").second) {
        dash::register_provider({"fft", ResourceKind::CPU, provider_instance++, 10});
    }
    if (cpu_registered.insert("fir").second) {
        dash::register_provider({"fir", ResourceKind::CPU, provider_instance++, 10});
    }

    sched.add_accelerator(make_cpu_mock(0));
    schedrt::reporting::set_csv(csv_report);

    void* handle = dlopen(app_lib.c_str(), RTLD_NOW);
    if (!handle) {
        std::cerr << "dlopen failed: " << dlerror() << "\n";
        return 1;
    }

    using init_fn = void (*)(int, char**, ApplicationRegistry&, Scheduler&);
    using run_fn = int (*)(int, char**, Scheduler&);

    auto init = reinterpret_cast<init_fn>(dlsym(handle, "app_initialize"));
    auto run = reinterpret_cast<run_fn>(dlsym(handle, "app_run"));
    if (!init || !run) {
        std::cerr << "failed to resolve app_initialize/app_run\n";
        dlclose(handle);
        return 1;
    }

    init(app_argc, app_argv, reg, sched);
    sched.start();
    int app_ret = run(app_argc, app_argv, sched);
    sched.stop();

    dlclose(handle);
    return app_ret;
}
