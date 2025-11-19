#include "apps/app_interface.hpp"
#include "dash/fft.hpp"
#include "dash/zip.hpp"

#include <chrono>
#include <iostream>
#include <thread>

using namespace schedrt;

extern "C" void app_initialize(int, char**, ApplicationRegistry&, Scheduler&) {
    // No overlay registration here; scheduler config supplies FPGA apps.
}

extern "C" int app_run(int, char**, Scheduler&) {
    dash::ZipParams zp{3, dash::ZipMode::Compress};
    char inbuf[1024], outbuf[2048];
    size_t out_actual = 0;
    auto ok_zip = dash::zip_execute(zp, {inbuf, sizeof(inbuf)}, {outbuf, sizeof(outbuf)}, out_actual);
    std::cout << "zip_execute -> " << (ok_zip ? "OK" : "FAIL") << "\n";

    dash::FftPlan plan{1024, false};
    float fin[1024], fout[1024];
    auto ok_fft = dash::fft_execute(plan, {fin, sizeof(fin)}, {fout, sizeof(fout)});
    std::cout << "fft_execute -> " << (ok_fft ? "OK" : "FAIL") << "\n";

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return 0;
}
