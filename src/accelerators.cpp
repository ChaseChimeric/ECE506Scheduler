#include "dash/contexts.hpp"
#include "schedrt/accelerator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <zlib.h>

namespace {
template <typename T>
T* context_from_task(const schedrt::Task& task, const char* key) {
    auto it = task.params.find(key);
    if (it == task.params.end() || it->second.empty()) return nullptr;
    auto value = it->second;
    auto raw = std::stoull(value);
    return reinterpret_cast<T*>(static_cast<std::uintptr_t>(raw));
}

dash::ZipContext* zip_context(const schedrt::Task& task) {
    return context_from_task<dash::ZipContext>(task, dash::kZipContextKey);
}

dash::FftContext* fft_context(const schedrt::Task& task) {
    return context_from_task<dash::FftContext>(task, dash::kFftContextKey);
}

bool run_zip_operation(dash::ZipContext& ctx) {
    if (!ctx.in.data || !ctx.out.data) {
        ctx.ok = false;
        ctx.message = "zip: buffers missing";
        return false;
    }
    auto* dst = static_cast<Bytef*>(ctx.out.data);
    auto* src = static_cast<const Bytef*>(ctx.in.data);
    uLongf dest_len = static_cast<uLongf>(ctx.out.bytes);
    int level = std::clamp(ctx.params.level, 0, 9);
    int ret = Z_OK;
    if (ctx.params.mode == dash::ZipMode::Compress) {
        ret = compress2(dst, &dest_len, src, ctx.in.bytes, level);
    } else {
        ret = uncompress(dst, &dest_len, src, ctx.in.bytes);
    }
    ctx.ok = (ret == Z_OK);
    if (ctx.out_actual) *ctx.out_actual = static_cast<size_t>(dest_len);
    if (ctx.ok) {
        ctx.message = "zip: " + std::string(ctx.params.mode == dash::ZipMode::Compress ? "compressed" : "decompressed")
            + " (" + std::to_string(ctx.in.bytes) + " -> " + std::to_string(dest_len) + ")";
        return true;
    }
    ctx.message = "zip: zlib error " + std::to_string(ret);
    return false;
}

bool run_fft_operation(dash::FftContext& ctx) {
    if (!ctx.in.data || !ctx.out.data) {
        ctx.ok = false;
        ctx.message = "fft: missing buffers";
        return false;
    }
    auto* in = static_cast<const float*>(ctx.in.data);
    auto* out = static_cast<float*>(ctx.out.data);
    size_t max_in = ctx.in.bytes / sizeof(float);
    size_t max_out = ctx.out.bytes / sizeof(float);
    size_t n = ctx.plan.n ? ctx.plan.n : std::min(max_in, max_out);
    if (n == 0 || max_in < n || max_out < n) {
        ctx.ok = false;
        ctx.message = "fft: buffer sizes insufficient";
        return false;
    }
    const double two_pi = 2.0 * M_PI;
    double sign = ctx.plan.inverse ? 1.0 : -1.0;
    for (size_t k = 0; k < n; ++k) {
        std::complex<double> sum(0.0, 0.0);
        for (size_t j = 0; j < n; ++j) {
            double angle = two_pi * static_cast<double>(k) * static_cast<double>(j) / static_cast<double>(n);
            sum += std::complex<double>(in[j], 0.0) * std::polar(1.0, angle * sign);
        }
        if (ctx.plan.inverse && n > 0) sum /= static_cast<double>(n);
        out[k] = static_cast<float>(sum.real());
    }
    ctx.ok = true;
    ctx.message = "fft: computed n=" + std::to_string(n);
    return true;
}
} // namespace

namespace schedrt {

// ---------------- CPU mock ----------------
class CpuMockAccelerator : public Accelerator {
public:
    explicit CpuMockAccelerator(unsigned id) : id_(id) {}
    std::string name() const override { return "cpu-mock-" + std::to_string(id_); }
    bool is_available() override { return true; }
    bool ensure_app_loaded(const AppDescriptor&) override { return true; }
    ExecutionResult run(const Task& task, const AppDescriptor& app) override {
        auto t0 = std::chrono::steady_clock::now();
        bool ok = true;
        std::string message = "Executed " + app.app + " on mock CPU";
        if (auto* ctx = zip_context(task)) {
            ok = run_zip_operation(*ctx);
            message = ctx->message;
        } else if (auto* ctx = fft_context(task)) {
            ok = run_fft_operation(*ctx);
            message = ctx->message;
        } else {
            auto dur = task.est_runtime_ns.count() > 0 ? task.est_runtime_ns
                                                       : std::chrono::nanoseconds(10000000);
            std::this_thread::sleep_for(dur);
        }
        auto t1 = std::chrono::steady_clock::now();
        return {task.id, ok, message,
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)};
    }
private:
    unsigned id_;
};

// --------------- FPGA PR slot ----------------
FpgaSlotAccelerator::FpgaSlotAccelerator(unsigned slot, FpgaSlotOptions opts)
    : slot_(slot), opts_(std::move(opts)) {}

std::string FpgaSlotAccelerator::name() const {
    return "fpga-slot-" + std::to_string(slot_);
}

bool FpgaSlotAccelerator::is_available() {
    if (opts_.mock_mode) return true;
    std::ifstream ifs(opts_.manager_path);
    return ifs.good() || configured_;
}

bool FpgaSlotAccelerator::ensure_app_loaded(const AppDescriptor& app) {
    std::lock_guard<std::mutex> lk(mu_);
    if (configured_ && current_app_ == app.app) return true;
    if (!load_bitstream(app.bitstream_path)) {
        log("Failed to load " + app.app);
        return false;
    }
    current_app_ = app.app;
    current_kind_ = app.kind;
    configured_ = true;
    log("Loaded " + app.app + " (kind=" + std::to_string(static_cast<int>(app.kind)) + ")");
    return true;
}

bool FpgaSlotAccelerator::prepare_static() {
    std::lock_guard<std::mutex> lk(mu_);
    if (static_loaded_ || opts_.static_bitstream.empty()) return true;
    if (!load_bitstream(opts_.static_bitstream)) {
        log("Failed to load static shell " + opts_.static_bitstream);
        return false;
    }
    static_loaded_ = true;
    log("Static shell loaded: " + opts_.static_bitstream);
    return true;
}

ExecutionResult FpgaSlotAccelerator::run(const Task& task, const AppDescriptor& app) {
    std::lock_guard<std::mutex> run_lk(run_mu_);
    if (!ensure_app_loaded(app)) {
        return {task.id, false, "Failed to ensure " + app.app + " on " + name(), std::chrono::nanoseconds(0)};
    }
    auto t0 = std::chrono::steady_clock::now();
    if (opts_.mock_mode) {
        auto dur = task.est_runtime_ns.count() > 0 ? task.est_runtime_ns
                                                   : std::chrono::nanoseconds(15000000);
        std::this_thread::sleep_for(dur);
    }
    auto t1 = std::chrono::steady_clock::now();
    std::chrono::nanoseconds elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0);
    return {task.id, true, "Executed " + app.app + " on " + name(), elapsed};
}

std::string FpgaSlotAccelerator::current_app() const {
    std::lock_guard<std::mutex> lk(mu_);
    return current_app_;
}

ResourceKind FpgaSlotAccelerator::current_kind() const {
    std::lock_guard<std::mutex> lk(mu_);
    return current_kind_;
}

unsigned FpgaSlotAccelerator::slot_id() const {
    return slot_;
}

bool FpgaSlotAccelerator::load_bitstream(const std::string& path) {
    if (path.empty()) {
        log("No bitstream path provided; skipping load");
        return true;
    }
    if (opts_.mock_mode) {
        log("Mock loading " + path);
        return true;
    }
    std::ofstream ofs(opts_.manager_path);
    if (!ofs) {
        log("Unable to open FPGA manager at " + opts_.manager_path);
        return false;
    }
    ofs << path << "\n";
    if (!ofs.good()) {
        log("Write failed for bitstream " + path);
        return false;
    }
    log("Requested reconfiguration " + path);
    return true;
}

void FpgaSlotAccelerator::log(const std::string& msg) const {
    std::cout << "[" << name() << "] " << msg << "\n";
}

std::unique_ptr<Accelerator> make_cpu_mock(unsigned id) {
    return std::make_unique<CpuMockAccelerator>(id);
}
std::unique_ptr<Accelerator> make_fpga_slot(unsigned slot, FpgaSlotOptions opts) {
    return std::make_unique<FpgaSlotAccelerator>(slot, std::move(opts));
}
std::unique_ptr<Accelerator> make_zip_overlay(unsigned id) { 
    return make_cpu_mock(id); 
}
std::unique_ptr<Accelerator> make_fft_overlay(unsigned id) { 
    return make_cpu_mock(id+10); 
}

} // namespace schedrt
