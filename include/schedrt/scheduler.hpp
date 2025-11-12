#pragma once
#include "accelerator.hpp"
#include "application_registry.hpp"
#include "task.hpp"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace schedrt {

enum class BackendMode { AUTO, FPGA, CPU };

struct TaskCompare {
    bool operator()(const std::shared_ptr<Task>& a, const std::shared_ptr<Task>& b) const {
        if (a->priority != b->priority) return a->priority < b->priority; // max-heap
        if (a->release_time != b->release_time) return a->release_time > b->release_time;
        return a->id > b->id;
    }
};

class Scheduler {
public:
    Scheduler(ApplicationRegistry& reg, BackendMode mode, unsigned cpu_workers = 0,
              unsigned overlay_preload_threshold = 2);
    ~Scheduler();

    void add_accelerator(std::unique_ptr<Accelerator> acc);
    void submit(const std::shared_ptr<Task>& t);
    void start();
    void stop();

private:
    // PIMPL-ish internal helpers kept in .cpp
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace schedrt
