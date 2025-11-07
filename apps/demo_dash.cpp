#include "schedrt/scheduler.hpp"
#include "schedrt/application_registry.hpp"
#include "schedrt/accelerator.hpp"
#include "dash/provider.hpp"
#include "dash/fft.hpp"
#include "dash/zip.hpp"
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

// trivial global for demo; wire with DI in real code
schedrt::Scheduler* g_sched = nullptr;

using namespace schedrt;

int main() {
    // 1) App registry: map ops to kinds in preference order (HW then CPU)
    ApplicationRegistry reg;
   {
    schedrt::AppDescriptor a{};
    a.app = "zip";
    a.bitstream_path = "";
    a.kernel_name = "zip_kernel";
    reg.register_app(a);
}
{
    schedrt::AppDescriptor a{};
    a.app = "fft";
    a.bitstream_path = "";
    a.kernel_name = "fft_kernel";
    reg.register_app(a);
}
 
    

    // 2) Build scheduler as before
    static Scheduler sched(reg, BackendMode::AUTO, /*cpu_workers_hint*/4);
    g_sched = &sched;

    // 3) Device inventory: up to 2 of each hardware overlay + CPU fallbacks
    //    (your accelerators.cpp already defines these factories)
    sched.add_accelerator(make_zip_overlay(0));
    sched.add_accelerator(make_zip_overlay(1));
    sched.add_accelerator(make_fft_overlay(0));
    sched.add_accelerator(make_fft_overlay(1));
    // CPU fallbacks that implement ZIP/FFT in software:
    // (we discussed adding these; if you haven't added them yet, you can skip and rely on CPU-only.)
    // sched.add_accelerator(make_zip_cpu(0));
    // sched.add_accelerator(make_fft_cpu(0));

    // Plain CPU-only for generic CPU tasks (optional)
    sched.add_accelerator(make_cpu_mock(0));

    // 4) Register providers (DASH-style discovery), with preference: HW first, CPU last
    dash::register_provider({"zip", ResourceKind::ZIP, 0, 0});
    dash::register_provider({"zip", ResourceKind::ZIP, 1, 0});
    dash::register_provider({"zip", ResourceKind::CPU, 0, 10}); // fallback

    dash::register_provider({"fft", ResourceKind::FFT, 0, 0});
    dash::register_provider({"fft", ResourceKind::FFT, 1, 0});
    dash::register_provider({"fft", ResourceKind::CPU, 0, 10}); // fallback

    sched.start();

    // 5) Use the DASH API (no device details mentioned here)
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
    sched.stop();
    return 0;
}

