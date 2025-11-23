#include "dash/contexts.hpp"
#include "schedrt/accelerator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
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

class UdmabufRegion {
public:
    ~UdmabufRegion() {
        if (virt_) munmap(virt_, size_);
        if (fd_ >= 0) close(fd_);
    }

    bool init(const std::string& dev_name, size_t min_size_bytes) {
        std::string sysfs_base = "/sys/class/u-dma-buf/" + dev_name;
        uint64_t size_value = 0;
        if (!read_value(sysfs_base + "/size", &size_value)) return false;
        if (size_value < min_size_bytes) {
            std::cerr << "[udmabuf] device " << dev_name << " too small (" << size_value << " bytes)\n";
            return false;
        }
        uint64_t phys_value = 0;
        if (!read_value(sysfs_base + "/phys_addr", &phys_value)) return false;
        std::string dev_path = "/dev/" + dev_name;
        fd_ = ::open(dev_path.c_str(), O_RDWR | O_SYNC);
        if (fd_ < 0) {
            std::cerr << "[udmabuf] open(" << dev_path << ") failed: " << strerror(errno) << "\n";
            return false;
        }
        void* map = mmap(nullptr, size_value, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (map == MAP_FAILED) {
            std::cerr << "[udmabuf] mmap failed: " << strerror(errno) << "\n";
            close(fd_);
            fd_ = -1;
            return false;
        }
        virt_ = map;
        size_ = static_cast<size_t>(size_value);
        phys_ = phys_value;
        return true;
    }

    void* virt() const { return virt_; }
    size_t size() const { return size_; }
    uint64_t phys() const { return phys_; }

private:
    static bool read_value(const std::string& path, uint64_t* out) {
        std::ifstream ifs(path);
        if (!ifs) return false;
        std::string text;
        ifs >> text;
        try {
            *out = std::stoull(text, nullptr, 0);
        } catch (...) {
            return false;
        }
        return true;
    }

    int fd_{-1};
    void* virt_{nullptr};
    size_t size_{0};
    uint64_t phys_{0};
};

class AxiDmaController {
public:
    AxiDmaController(uintptr_t base_phys, size_t span_bytes)
        : base_phys_(base_phys), span_(span_bytes) {}

    ~AxiDmaController() {
        if (regs_) munmap(const_cast<uint32_t*>(regs_), span_);
        if (mem_fd_ >= 0) close(mem_fd_);
    }

    bool init() {
        mem_fd_ = ::open("/dev/mem", O_RDWR | O_SYNC);
        if (mem_fd_ < 0) {
            std::cerr << "[axi-dma] unable to open /dev/mem: " << strerror(errno) << "\n";
            return false;
        }
        void* mapped = mmap(nullptr, span_, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd_, base_phys_);
        if (mapped == MAP_FAILED) {
            std::cerr << "[axi-dma] mmap failed: " << strerror(errno) << "\n";
            close(mem_fd_);
            mem_fd_ = -1;
            return false;
        }
        regs_ = static_cast<volatile uint32_t*>(mapped);
        reset_channel(true);
        reset_channel(false);
        ready_ = true;
        return true;
    }

    bool ready() const { return ready_; }

    bool transfer(uint64_t src_phys, uint64_t dst_phys, size_t bytes) {
        if (!ready_ || bytes == 0) return ready_;
        constexpr uint32_t kDmaCrRunStop = 0x1;
        constexpr uint32_t kDmaCrIoC = 0x10;
        constexpr uint32_t kDmaCrErr = 0x40;
        constexpr uint32_t kStatusErrMask = (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) |
                                            (1u << 12) | (1u << 13) | (1u << 14);

        auto clear_status = [&](bool s2mm) {
            write_reg(s2mm ? S2MM_DMASR : MM2S_DMASR, 0xFFFFFFFF);
        };
        clear_status(true);
        clear_status(false);

        write_reg(S2MM_DMACR, kDmaCrRunStop | kDmaCrIoC | kDmaCrErr);
        write_reg(S2MM_DA, static_cast<uint32_t>(dst_phys & 0xFFFFFFFF));
        write_reg(S2MM_DA_MSB, static_cast<uint32_t>((dst_phys >> 32) & 0xFFFFFFFF));
        write_reg(S2MM_LENGTH, static_cast<uint32_t>(bytes));

        write_reg(MM2S_DMACR, kDmaCrRunStop | kDmaCrIoC | kDmaCrErr);
        write_reg(MM2S_SA, static_cast<uint32_t>(src_phys & 0xFFFFFFFF));
        write_reg(MM2S_SA_MSB, static_cast<uint32_t>((src_phys >> 32) & 0xFFFFFFFF));
        write_reg(MM2S_LENGTH, static_cast<uint32_t>(bytes));

        if (!wait_for_idle(true)) {
            std::cerr << "[axi-dma] mm2s timeout status=0x" << std::hex << read_reg(MM2S_DMASR)
                      << std::dec << "\n";
            return false;
        }
        if (!wait_for_idle(false)) {
            std::cerr << "[axi-dma] s2mm timeout status=0x" << std::hex << read_reg(S2MM_DMASR)
                      << std::dec << "\n";
            return false;
        }

        auto status_mm2s = read_reg(MM2S_DMASR);
        auto status_s2mm = read_reg(S2MM_DMASR);
        if ((status_mm2s & kStatusErrMask) || (status_s2mm & kStatusErrMask)) {
            std::cerr << "[axi-dma] error status mm2s=0x" << std::hex << status_mm2s
                      << " s2mm=0x" << status_s2mm << std::dec << "\n";
            return false;
        }
        return true;
    }

private:
    void reset_channel(bool s2mm) {
        constexpr uint32_t kResetBit = 0x4;
        write_reg(s2mm ? S2MM_DMACR : MM2S_DMACR, kResetBit);
        for (int i = 0; i < 1000; ++i) {
            if ((read_reg(s2mm ? S2MM_DMACR : MM2S_DMACR) & kResetBit) == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        write_reg(s2mm ? S2MM_DMASR : MM2S_DMASR, 0xFFFFFFFF);
    }

    bool wait_for_idle(bool mm2s) {
        constexpr uint32_t kIdleBit = 0x2;
        constexpr uint32_t kErrMask = (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) |
                                      (1u << 12) | (1u << 13) | (1u << 14);
        off_t status_reg = mm2s ? MM2S_DMASR : S2MM_DMASR;
        for (int i = 0; i < 4000; ++i) {
            auto status = read_reg(status_reg);
            if (status & kErrMask) return false;
            if (status & kIdleBit) return true;
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        return false;
    }

    uint32_t read_reg(off_t offset) const {
        return regs_[offset / sizeof(uint32_t)];
    }

    void write_reg(off_t offset, uint32_t value) {
        regs_[offset / sizeof(uint32_t)] = value;
    }

    int mem_fd_{-1};
    volatile uint32_t* regs_{nullptr};
    uintptr_t base_phys_{0};
    size_t span_{0};
    bool ready_{false};

    static constexpr off_t MM2S_DMACR = 0x00;
    static constexpr off_t MM2S_DMASR = 0x04;
    static constexpr off_t MM2S_SA = 0x18;
    static constexpr off_t MM2S_SA_MSB = 0x1C;
    static constexpr off_t MM2S_LENGTH = 0x28;
    static constexpr off_t S2MM_DMACR = 0x30;
    static constexpr off_t S2MM_DMASR = 0x34;
    static constexpr off_t S2MM_DA = 0x48;
    static constexpr off_t S2MM_DA_MSB = 0x4C;
    static constexpr off_t S2MM_LENGTH = 0x58;
};

class FftHwRunner {
public:
    FftHwRunner() = default;

    bool initialize() {
        std::string udmabuf_name = "udmabuf0";
        if (const char* env = std::getenv("SCHEDRT_UDMABUF")) udmabuf_name = env;
        if (!buffer_.init(udmabuf_name, 1 << 19)) {
            std::cerr << "[fft-hw] missing udmabuf (" << udmabuf_name << ")\n";
            return false;
        }

        uintptr_t dma_base = 0x40400000;
        if (const char* env = std::getenv("SCHEDRT_DMA_BASE")) {
            try {
                dma_base = static_cast<uintptr_t>(std::stoull(env, nullptr, 0));
            } catch (...) {
                std::cerr << "[fft-hw] invalid SCHEDRT_DMA_BASE value\n";
            }
        }
        dma_ = std::make_unique<AxiDmaController>(dma_base, 0x10000);
        if (!dma_->init()) {
            std::cerr << "[fft-hw] unable to initialize AXI DMA\n";
            return false;
        }
        input_offset_ = 0;
        output_offset_ = buffer_.size() / 2;
        ready_ = true;
        return true;
    }

    bool available() const { return ready_; }

    bool execute(dash::FftContext& ctx) {
        if (!ready_) return false;
        if (!ctx.in.data || !ctx.out.data) return false;
        std::lock_guard<std::mutex> lk(mu_);

        size_t sample_count = ctx.plan.n;
        if (sample_count == 0) {
            size_t complex_floats = ctx.in.bytes / (2 * sizeof(float));
            sample_count = complex_floats;
        }
        if (sample_count == 0) return false;
        size_t bytes = sample_count * sizeof(int16_t) * 2;
        size_t half_buf = buffer_.size() / 2;
        if (bytes > half_buf) {
            std::cerr << "[fft-hw] requested transfer exceeds buffer size\n";
            return false;
        }

        auto* input = static_cast<const float*>(ctx.in.data);
        auto* output = static_cast<float*>(ctx.out.data);
        if (ctx.out.bytes < sample_count * 2 * sizeof(float)) {
            std::cerr << "[fft-hw] output buffer too small\n";
            return false;
        }

        auto* hw_in = reinterpret_cast<int16_t*>(static_cast<uint8_t*>(buffer_.virt()) + input_offset_);
        auto* hw_out = reinterpret_cast<int16_t*>(static_cast<uint8_t*>(buffer_.virt()) + output_offset_);

        for (size_t i = 0; i < sample_count * 2; ++i) {
            float value = input[i];
            if (value > 0.999969f) value = 0.999969f;
            if (value < -1.0f) value = -1.0f;
            hw_in[i] = static_cast<int16_t>(std::lrint(value * 32767.0f));
        }

        if (!dma_->transfer(buffer_.phys() + input_offset_, buffer_.phys() + output_offset_, bytes)) {
            ctx.ok = false;
            ctx.message = "fft: hw DMA failure";
            return false;
        }

        for (size_t i = 0; i < sample_count * 2; ++i) {
            output[i] = static_cast<float>(hw_out[i]) / 32768.0f;
        }
        ctx.ok = true;
        ctx.message = "fft: hw n=" + std::to_string(sample_count);
        return true;
    }

private:
    UdmabufRegion buffer_;
    std::unique_ptr<AxiDmaController> dma_;
    size_t input_offset_{0};
    size_t output_offset_{0};
    bool ready_{false};
    std::mutex mu_;
};

std::shared_ptr<FftHwRunner> acquire_fft_runner() {
    static std::mutex runner_mu;
    static std::shared_ptr<FftHwRunner> runner;
    std::lock_guard<std::mutex> lk(runner_mu);
    if (!runner) {
        auto tmp = std::make_shared<FftHwRunner>();
        if (tmp->initialize()) {
            runner = tmp;
        }
    }
    return runner;
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
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0),
                name()};
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
    log_debug("ensure_app_loaded app=" + app.app + " kind=" + std::to_string(static_cast<int>(app.kind))
              + " bitstream=" + app.bitstream_path);
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
    log_debug("prepare_static shell=" + opts_.static_bitstream);
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
    log_debug("run task id=" + std::to_string(task.id) + " app=" + task.app);
    if (!ensure_app_loaded(app)) {
        return {task.id, false, "Failed to ensure " + app.app + " on " + name(), std::chrono::nanoseconds(0), name()};
    }
    auto t0 = std::chrono::steady_clock::now();
    bool ok = true;
    std::string message = "Executed " + app.app + " on " + name();
if (!opts_.mock_mode && task.app == "fft") {
        auto* ctx = fft_context(task);
        bool ran_hw = false;
        if (ctx) {
            log_debug("fft context available for task=" + std::to_string(task.id));
            auto runner = acquire_fft_runner();
            if (runner && runner->available()) {
                ran_hw = runner->execute(*ctx);
                ok = ran_hw && ctx->ok;
                message = ctx->message;
            }
            if (!ran_hw) {
                log_debug("fft task fallback to CPU path (id=" + std::to_string(task.id) + ")");
                ok = run_fft_operation(*ctx);
                message = ctx->message + " (cpu fallback)";
            }
        } else {
            log_debug("fft task missing execution context (id=" + std::to_string(task.id) + ")");
            ok = false;
            message = "fft: missing execution context";
        }
    } else {
        auto dur = task.est_runtime_ns.count() > 0 ? task.est_runtime_ns
                                                   : std::chrono::nanoseconds(15000000);
        std::this_thread::sleep_for(dur);
    }
    auto t1 = std::chrono::steady_clock::now();
    std::chrono::nanoseconds elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0);
    return {task.id, ok, message, elapsed, name()};
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
    log_debug("load_bitstream start path=" + path);
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

void FpgaSlotAccelerator::log_debug(const std::string& msg) const {
    if (!opts_.debug_logging) return;
    std::cout << "[" << name() << "] [debug] " << msg << "\n";
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
