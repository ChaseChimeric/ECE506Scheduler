#pragma once
#include "task.hpp"
#include <memory>
#include <mutex>
#include <string>

namespace schedrt {

struct AppDescriptor {
    std::string app;
    std::string bitstream_path;
    std::string kernel_name;
    ResourceKind kind{ResourceKind::CPU};
};

class Accelerator {
public:
    virtual ~Accelerator() = default;
    virtual std::string name() const = 0;
    virtual bool is_available() = 0;
    virtual bool ensure_app_loaded(const AppDescriptor& app) = 0;
    virtual ExecutionResult run(const Task& task, const AppDescriptor& app) = 0;
    virtual bool is_reconfigurable() const { return false; }
};

struct FpgaSlotOptions {
    std::string manager_path = "/sys/class/fpga_manager/fpga0/firmware";
    bool mock_mode = true;
};

class FpgaSlotAccelerator : public Accelerator {
public:
    explicit FpgaSlotAccelerator(unsigned slot, FpgaSlotOptions opts = {});
    std::string name() const override;
    bool is_available() override;
    bool ensure_app_loaded(const AppDescriptor& app) override;
    ExecutionResult run(const Task& task, const AppDescriptor& app) override;
    bool is_reconfigurable() const override { return true; }
    std::string current_app() const;
    ResourceKind current_kind() const;
    unsigned slot_id() const;
private:
    bool load_bitstream(const std::string& path);
    void log(const std::string& msg) const;

    unsigned slot_;
    FpgaSlotOptions opts_;
    mutable std::mutex mu_;
    std::mutex run_mu_;
    std::string current_app_;
    ResourceKind current_kind_{ResourceKind::CPU};
    bool configured_{false};
};

/// Factory helpers (implemented in accelerators.cpp)
std::unique_ptr<Accelerator> make_cpu_mock(unsigned id = 0);
std::unique_ptr<Accelerator> make_fpga_slot(unsigned slot = 0, FpgaSlotOptions opts = {});

std::unique_ptr<Accelerator> make_zip_overlay(unsigned id);
std::unique_ptr<Accelerator> make_fft_overlay(unsigned id);
} // namespace schedrt
