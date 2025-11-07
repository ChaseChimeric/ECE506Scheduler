#include "schedrt/accelerator.hpp"
#include <chrono>
#include <thread>

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
        auto dur = task.est_runtime_ms.count() > 0 ? task.est_runtime_ms
                                                   : std::chrono::milliseconds(10);
        std::this_thread::sleep_for(dur);
        auto t1 = std::chrono::steady_clock::now();
        return {task.id, true, "Executed " + app.app + " on mock CPU",
                std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)};
    }
private:
    unsigned id_;
};

// --------------- FPGA stub ----------------
class FpgaAccelerator : public Accelerator {
public:
    explicit FpgaAccelerator(unsigned slot) : slot_(slot) {}
    std::string name() const override { return "fpga-slot-" + std::to_string(slot_); }
    bool is_available() override {
        // TODO: detect PYNQ slot; return true when wired up
        return false;
    }
    bool ensure_app_loaded(const AppDescriptor& /*app*/) override {
        // TODO: perform PR if needed
        return false;
    }
    ExecutionResult run(const Task& task, const AppDescriptor& app) override {
        return {task.id, false, "FPGA backend not implemented for " + app.app, std::chrono::milliseconds(0)};
    }
private:
    unsigned slot_;
};

std::unique_ptr<Accelerator> make_cpu_mock(unsigned id) {
    return std::make_unique<CpuMockAccelerator>(id);
}
std::unique_ptr<Accelerator> make_fpga_slot(unsigned slot) {
    return std::make_unique<FpgaAccelerator>(slot);
}

} // namespace schedrt

