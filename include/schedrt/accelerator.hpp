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
    virtual bool prepare_static() { return true; }
};

struct FpgaSlotOptions {
    std::string manager_path = "/sys/class/fpga_manager/fpga0/firmware";
    bool mock_mode = true;
    std::string static_bitstream;
    bool debug_logging = false;
    int pr_gpio_number = -1;
    bool pr_gpio_active_low = false;
    unsigned pr_gpio_delay_ms = 5;
};

class FpgaSlotAccelerator : public Accelerator {
public:
    explicit FpgaSlotAccelerator(unsigned slot, FpgaSlotOptions opts = {});
    std::string name() const override;
    bool is_available() override;
    bool ensure_app_loaded(const AppDescriptor& app) override;
    ExecutionResult run(const Task& task, const AppDescriptor& app) override;
    bool prepare_static() override;
    bool is_reconfigurable() const override { return true; }
    std::string current_app() const;
    ResourceKind current_kind() const;
    unsigned slot_id() const;
private:
    bool load_bitstream(const std::string& path);
    bool ensure_pr_gpio_ready();
    bool set_decouple_gpio(bool asserted);
    bool has_pr_gpio() const { return opts_.pr_gpio_number >= 0; }
    void log(const std::string& msg) const;
    void log_debug(const std::string& msg) const;

    unsigned slot_;
    FpgaSlotOptions opts_;
    mutable std::mutex mu_;
    std::mutex run_mu_;
    std::string current_app_;
    ResourceKind current_kind_{ResourceKind::CPU};
    bool configured_{false};
    bool static_loaded_{false};
    bool pr_gpio_ready_{false};
    std::string pr_gpio_value_path_;
};

/// Factory helpers (implemented in accelerators.cpp)
std::unique_ptr<Accelerator> make_cpu_mock(unsigned id = 0);
std::unique_ptr<Accelerator> make_fpga_slot(unsigned slot = 0, FpgaSlotOptions opts = {});

std::unique_ptr<Accelerator> make_zip_overlay(unsigned id);
std::unique_ptr<Accelerator> make_fft_overlay(unsigned id);
} // namespace schedrt
