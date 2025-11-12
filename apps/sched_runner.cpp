#include "apps/app_interface.hpp"
#include "dash/scheduler_binding.hpp"
#include "schedrt/scheduler.hpp"
#include "schedrt/application_registry.hpp"

#include <dlfcn.h>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace schedrt;

namespace {

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " --app-lib=PATH [--backend=auto|cpu|fpga] [--cpu-workers=N] "
              << "[--preload-threshold=N] -- [app args...]\n";
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

int main(int argc, char** argv) {
    std::string app_lib;
    BackendMode backend = BackendMode::AUTO;
    unsigned cpu_workers = std::thread::hardware_concurrency();
    if (cpu_workers == 0) cpu_workers = 4;
    unsigned preload_threshold = 3;

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

    ApplicationRegistry reg;
    Scheduler sched(reg, backend, cpu_workers, preload_threshold);
    dash::set_scheduler(&sched);

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
