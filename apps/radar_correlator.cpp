#include "apps/app_interface.hpp"
#include "dash/contexts.hpp"
#include "dash/fft.hpp"
#include "dash/provider.hpp"
#include "dash/completion_bus.hpp"
#include "schedrt/accelerator.hpp"
#include "schedrt/application_registry.hpp"
#include "schedrt/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

bool load_data(const std::filesystem::path& path, std::vector<double>& dest) {
    std::ifstream fp(path);
    if (!fp) {
        std::cerr << "unable to open '" << path << "'\n";
        return false;
    }
    dest.clear();
    double value = 0;
    while (fp >> value) {
        dest.push_back(value);
    }
    if (dest.empty()) {
        std::cerr << "input file " << path << " contains no values\n";
        return false;
    }
    return true;
}

std::filesystem::path discover_input(int argc, char** argv) {
    namespace fs = std::filesystem;
    fs::path explicit_path;
    for (int i = 0; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg.rfind("--input=", 0) == 0) {
            explicit_path = fs::path(arg.substr(sizeof("--input=") - 1));
            break;
        }
    }
    if (!explicit_path.empty() && fs::is_directory(explicit_path)) return explicit_path;

    fs::path exe_path = fs::absolute(fs::path(argv[0]));
    std::vector<fs::path> candidates = {
        exe_path.parent_path() / "input",
        fs::current_path() / "input",
        fs::path("apps/to be implemented") / "input",
    };
    for (auto& cand : candidates) {
        if (fs::is_directory(cand)) return cand;
    }
    std::cerr << "unable to locate input directory; tried:\n";
    for (const auto& cand : candidates) {
        std::cerr << "  " << cand << "\n";
    }
    throw std::runtime_error("input directory not found");
}

std::atomic<uint64_t> g_next_fft_id{5000};

struct ScheduledFFT {
    std::shared_ptr<dash::FftContext> ctx;
    std::future<bool> fut;
};

ScheduledFFT schedule_fft_task(schedrt::Scheduler& sched, float* in_buf, float* out_buf, size_t len, bool inverse) {
    auto ctx = std::make_shared<dash::FftContext>();
    ctx->plan = {static_cast<int>(len), inverse};
    ctx->in = {in_buf, len * sizeof(float)};
    ctx->out = {out_buf, len * sizeof(float)};

    auto task = std::make_shared<schedrt::Task>();
    task->id = g_next_fft_id.fetch_add(1, std::memory_order_relaxed);
    task->app = "fft";
    task->required = schedrt::ResourceKind::FFT;
    task->est_runtime_ns = std::chrono::nanoseconds(15000000);
    task->params.emplace(dash::kFftContextKey, std::to_string(reinterpret_cast<std::uintptr_t>(ctx.get())));

    auto fut = dash::subscribe(task->id);
    sched.submit(task);
    return ScheduledFFT{ctx, std::move(fut)};
}

} // namespace

using namespace schedrt;

extern "C" void app_initialize(int argc, char** argv, ApplicationRegistry& reg, Scheduler& sched) {
    (void)argc;
    (void)argv;
    reg.register_app({"fft", "", "fft_kernel"});
    sched.add_accelerator(make_cpu_mock(0));
    dash::register_provider({"fft", ResourceKind::FFT, 0, 0});
    dash::register_provider({"fft", ResourceKind::CPU, 0, 10});
}

extern "C" int app_run(int argc, char** argv, Scheduler& sched) {
    namespace fs = std::filesystem;
    fs::path asset_dir;
    try {
        asset_dir = discover_input(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    std::vector<double> time;
    if (!load_data(asset_dir / "time_input.txt", time)) return 1;

    std::vector<double> received_raw;
    if (!load_data(asset_dir / "received_input.txt", received_raw)) return 1;

    const size_t target_fft_len = 65536;
    if (time.size() < target_fft_len) time.resize(target_fft_len, 0.0);
    if (received_raw.size() < 2 * target_fft_len) received_raw.resize(2 * target_fft_len, 0.0);

    size_t n_samples = target_fft_len;
    size_t fft_len = target_fft_len;
    size_t complex_slots = 2 * fft_len;

    std::vector<float> chirp(complex_slots, 0.0f);
    std::vector<float> received(complex_slots, 0.0f);
    for (size_t i = 0; i < n_samples; ++i) {
        double phase = M_PI * 500000.0 / 0.000512 * (time[i] * time[i]);
        chirp[2 * i] = static_cast<float>(std::sin(phase));
        chirp[2 * i + 1] = static_cast<float>(std::cos(phase));
    }
    size_t available = std::min(complex_slots, received_raw.size());
    for (size_t i = 0; i < available; ++i) {
        received[i] = static_cast<float>(received_raw[i]);
    }

    std::vector<float> X1(complex_slots, 0.0f);
    std::vector<float> X2(complex_slots, 0.0f);
    std::vector<float> corr_freq(complex_slots, 0.0f);
    std::vector<float> corr_time(complex_slots, 0.0f);

    auto fft1 = schedule_fft_task(sched, chirp.data(), X1.data(), fft_len, false);
    auto fft2 = schedule_fft_task(sched, received.data(), X2.data(), fft_len, false);
    if (!fft1.fut.get() || !fft2.fut.get()) {
        std::cerr << "fft execution failed\n";
        return 1;
    }

    for (size_t i = 0; i < fft_len; ++i) {
        float a = X1[2 * i];
        float b = X1[2 * i + 1];
        float c = X2[2 * i];
        float d = X2[2 * i + 1];
        corr_freq[2 * i] = (a * c) + (b * d);
        corr_freq[2 * i + 1] = (b * c) - (a * d);
    }

    auto inverse_fft = schedule_fft_task(sched, corr_freq.data(), corr_time.data(), fft_len, true);
    if (!inverse_fft.fut.get()) {
        std::cerr << "inverse fft failed\n";
        return 1;
    }

    float max_corr = std::numeric_limits<float>::lowest();
    size_t max_index = 0;
    for (size_t i = 0; i < fft_len; ++i) {
        float real = corr_time[2 * i];
        if (real > max_corr) {
            max_corr = real;
            max_index = i;
        }
    }

    double lag = static_cast<double>(n_samples - max_index) / 1000.0;
    std::cout << "Radar correlator lag = " << lag << " (max_corr=" << max_corr << ")\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return 0;
}
