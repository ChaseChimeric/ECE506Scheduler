#pragma once
#include "task.hpp"
#include <memory>
#include <string>

namespace schedrt {

struct AppDescriptor {
    std::string app;
    std::string bitstream_path;
    std::string kernel_name;
};

class Accelerator {
public:
    virtual ~Accelerator() = default;
    virtual std::string name() const = 0;
    virtual bool is_available() = 0;
    virtual bool ensure_app_loaded(const AppDescriptor& app) = 0;
    virtual ExecutionResult run(const Task& task, const AppDescriptor& app) = 0;
};

/// Factory helpers (implemented in accelerators.cpp)
std::unique_ptr<Accelerator> make_cpu_mock(unsigned id = 0);
std::unique_ptr<Accelerator> make_fpga_slot(unsigned slot = 0);

std::unique_ptr<Accelerator> make_zip_overlay(unsigned id);
std::unique_ptr<Accelerator> make_fft_overlay(unsigned id);
} // namespace schedrt

