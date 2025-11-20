#include "apps/app_interface.hpp"
#include "dash/fft.hpp"
#include "dash/provider.hpp"
#include "schedrt/accelerator.hpp"
#include "schedrt/application_registry.hpp"
#include "schedrt/scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Complex {
    float re;
    float im;
};

bool load_raw(const std::filesystem::path& path, std::vector<Complex>& dest, size_t count) {
    std::ifstream fp(path);
    if (!fp) {
        std::cerr << "unable to open '" << path << "'\n";
        return false;
    }
    dest.clear();
    dest.resize(count);
    for (size_t i = 0; i < count; ++i) {
        double re = 0, im = 0;
        if (!(fp >> re >> im)) {
            std::cerr << "unexpected EOF in " << path << "\n";
            return false;
        }
        dest[i].re = static_cast<float>(re);
        dest[i].im = static_cast<float>(im);
    }
    return true;
}

void fftshift(std::vector<Complex>& data, size_t len) {
    size_t half = len / 2;
    if (len % 2 == 0) {
        for (size_t i = 0; i < half; ++i) {
            std::swap(data[i], data[i + half]);
        }
    } else {
        std::rotate(data.begin(), data.begin() + half + 1, data.end());
    }
}

bool run_fft(const std::vector<float>& input, std::vector<float>& output, size_t len, bool inverse) {
    dash::FftPlan plan;
    plan.n = static_cast<int>(len);
    plan.inverse = inverse;
    return dash::fft_execute(plan,
                             {const_cast<float*>(input.data()), input.size() * sizeof(float)},
                             {output.data(), output.size() * sizeof(float)});
}

size_t discover_arg(int argc, char** argv, const char* prefix, const std::string& default_value, std::string& out) {
    for (int i = 0; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg.rfind(prefix, 0) == 0) {
            out = arg.substr(strlen(prefix));
            return i;
        }
    }
    out = default_value;
    return std::string::npos;
}

std::filesystem::path find_input_dir(int argc, char** argv) {
    std::string maybe;
    auto idx = discover_arg(argc, argv, "--input=", "", maybe);
    if (!maybe.empty() && std::filesystem::is_directory(maybe)) return maybe;
    std::filesystem::path exe = std::filesystem::absolute(std::filesystem::path(argv[0]));
    std::filesystem::path candidates[] = {
        exe.parent_path() / "SAR" / "input",
        exe.parent_path() / "input",
        std::filesystem::current_path() / "apps" / "SAR" / "input",
    };
    for (auto& cand : candidates) {
        if (std::filesystem::is_directory(cand)) return cand;
    }
    throw std::runtime_error("input directory not found");
}

} // namespace

using namespace schedrt;

extern "C" void app_initialize(int argc, char** argv, ApplicationRegistry& reg, Scheduler& sched) {
    (void)argc;
    (void)argv;
    if (!reg.lookup("fft")) {
        reg.register_app({"fft", "", "fft_kernel"});
    }
    sched.add_accelerator(make_cpu_mock(0));
    dash::register_provider({"fft", ResourceKind::FFT, 0, 0});
    dash::register_provider({"fft", ResourceKind::CPU, 0, 10});
}

extern "C" int app_run(int argc, char** argv, Scheduler& sched) {
    namespace fs = std::filesystem;
    fs::path input_dir;
    try {
        input_dir = find_input_dir(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    const size_t Nslow = 256;
    const size_t Nfast = 512;
    const size_t complex_len = Nslow * Nfast;
    const double c = 3e8;
    const double v = 150;
    const double Xmin = 0;
    const double Xmax = 50;
    const double Yc = 10000;
    const double Y0 = 500;
    const double Tr = 2.5e-6;
    const double Kr = 2e13;
    const double h = 5000;
    const double lambda = 0.0566;

    const double R0 = std::sqrt(Yc * Yc + h * h);
    const double Ka = 2 * v * v / lambda / R0;

    std::vector<Complex> s0;
    if (!load_raw(input_dir / "rawdata_rda.txt", s0, complex_len)) return 1;

    std::vector<float> g(Nfast * 2, 0.0f);
    for (size_t i = 0; i < Nfast; ++i) {
        double tr = (i < Nfast) ? i * (2 * std::sqrt((Yc + Y0) * (Yc + Y0) + h * h) / c + Tr -
                                   2 * std::sqrt((Yc - Y0) * (Yc - Y0) + h * h) / c) / (Nfast - 1) : 0;
        if (tr > -Tr / 2 && tr < Tr / 2) {
            g[2 * i] = static_cast<float>(std::cos(M_PI * Kr * tr * tr));
            g[2 * i + 1] = static_cast<float>(-std::sin(M_PI * Kr * tr * tr));
        }
    }

    std::vector<float> s0_flat(complex_len * 2);
    for (size_t i = 0; i < complex_len; ++i) {
        s0_flat[2 * i] = s0[i].re;
        s0_flat[2 * i + 1] = s0[i].im;
    }

    std::vector<float> temp(complex_len * 2, 0.0f);
    std::vector<float> fft_tmp(complex_len * 2, 0.0f);
    std::vector<float> corr(complex_len * 2, 0.0f);

    size_t block = Nfast;
    for (size_t slow = 0; slow < Nslow; ++slow) {
        size_t offset = slow * Nfast * 2;
        std::copy_n(s0_flat.begin() + offset, block * 2, temp.begin() + offset);
        if (!run_fft(temp, fft_tmp, Nfast, false)) return 1;
        std::vector<Complex> chunk(Nfast);
        for (size_t i = 0; i < Nfast; ++i) {
            chunk[i].re = fft_tmp[offset + 2 * i];
            chunk[i].im = fft_tmp[offset + 2 * i + 1];
        }
        fftshift(chunk, Nfast);
        for (size_t i = 0; i < Nfast; ++i) {
            float re = chunk[i].re * g[2 * i] - chunk[i].im * g[2 * i + 1];
            float im = chunk[i].im * g[2 * i] + chunk[i].re * g[2 * i + 1];
            fft_tmp[offset + 2 * i] = re;
            fft_tmp[offset + 2 * i + 1] = im;
        }
        if (!run_fft(fft_tmp, temp, Nfast, true)) return 1;
        for (size_t i = 0; i < Nfast; ++i) {
            corr[offset + 2 * i] = temp[offset + 2 * i];
            corr[offset + 2 * i + 1] = temp[offset + 2 * i + 1];
        }
    }

    // Output magnitude
    std::ofstream out((input_dir / "SAR_output.txt").string());
    if (out) {
        for (size_t slow = 0; slow < Nslow; ++slow) {
            for (size_t fast = 0; fast < Nfast; ++fast) {
                size_t idx = slow * Nfast * 2 + fast * 2;
                float mag = std::sqrt(corr[idx] * corr[idx] + corr[idx + 1] * corr[idx + 1]);
                out << mag << " ";
            }
            out << "\n";
        }
    }

    std::cout << "[SAR] Execution complete; output written to " << (input_dir / "SAR_output.txt") << "\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return 0;
}
