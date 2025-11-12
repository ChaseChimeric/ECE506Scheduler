#include "schedrt/scheduler.hpp"
#include "schedrt/application_registry.hpp"
#include "schedrt/accelerator.hpp"
#include "schedrt/task.hpp"
#include <chrono>
#include <iostream>
#include <memory>
#include <string>

using namespace schedrt;

static BackendMode parse_backend(int argc, char** argv) {
    std::string v = "auto";
    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if (a.rfind("--backend=", 0) == 0) v = a.substr(10);
    }
    if (v == "cpu")  return BackendMode::CPU;
    if (v == "fpga") return BackendMode::FPGA;
    return BackendMode::AUTO;
}

int main(int argc, char** argv) {
    ApplicationRegistry reg;
    reg.register_app({"sobel", "bitstreams/sobel_partial.bit", "sobel_kernel"});
    reg.register_app({"gemm",  "bitstreams/gemm_partial.bit",  "gemm_kernel"});

    auto mode = parse_backend(argc, argv);
    Scheduler sched(reg, mode, /*cpu_workers=*/4);

    // Add accelerators (prefer FPGA, fallback CPU)
    sched.add_accelerator(make_fpga_slot(0));  // stubbed, unavailable by default
    sched.add_accelerator(make_cpu_mock(0));
    sched.add_accelerator(make_cpu_mock(1));

    sched.start();

    auto now = std::chrono::steady_clock::now();

    auto t1 = std::make_shared<Task>();
    t1->id = 1; t1->app = "sobel"; t1->priority = 5; t1->release_time = now;
    t1->est_runtime_ns = std::chrono::nanoseconds(120000000);

    auto t2 = std::make_shared<Task>();
    t2->id = 2; t2->app = "gemm"; t2->priority = 3; t2->depends_on = {1};
    t2->est_runtime_ns = std::chrono::nanoseconds(250000000);

    auto t3 = std::make_shared<Task>();
    t3->id = 3; t3->app = "sobel"; t3->priority = 4;
    t3->est_runtime_ns = std::chrono::nanoseconds(80000000);

    sched.submit(t1);
    sched.submit(t2);
    sched.submit(t3);

    std::this_thread::sleep_for(std::chrono::seconds(2));
    sched.stop();
    return 0;
}
