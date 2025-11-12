// scheduler_runtime.cpp
// C++17. Build: g++ -std=c++17 -O2 -pthread scheduler_runtime.cpp -o sched
//
// Run examples:
//   ./sched --backend=cpu
//   ./sched --backend=fpga     (stubbed; fill in PR calls)
//   ./sched --backend=auto     (tries FPGA, falls back to CPU)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// ------------------------------
// Task model
// ------------------------------
struct Task {
    using TaskId = uint64_t;

    TaskId id;
    std::string app;                // logical app name (e.g., "sobel", "gemm")
    int priority = 0;               // higher = sooner
    std::chrono::steady_clock::time_point release_time = std::chrono::steady_clock::now();
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::vector<Task::TaskId> depends_on; // completed task IDs required
    // Opaque payload—args, buffers, sizes, etc. In practice you’d carry buffers/handles.
    std::unordered_map<std::string, std::string> params;

    // Optional estimates to help scheduling decisions:
    std::chrono::nanoseconds est_runtime_ns{0};

    // Bookkeeping:
    std::atomic<bool> ready{false};
};

// Ordering: priority first (desc), then earlier release time, then ID (FIFO-ish).
struct TaskCompare {
    bool operator()(const std::shared_ptr<Task>& a, const std::shared_ptr<Task>& b) const {
        if (a->priority != b->priority) return a->priority < b->priority; // max-heap by priority
        if (a->release_time != b->release_time) return a->release_time > b->release_time;
        return a->id > b->id;
    }
};

// ------------------------------
// Thread-safe ready queue
// ------------------------------
class ReadyQueue {
public:
    void push(const std::shared_ptr<Task>& t) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            pq_.push(t);
        }
        cv_.notify_one();
    }

    std::shared_ptr<Task> pop_blocking() {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&]{ return !pq_.empty() || stop_; });
        if (stop_) return nullptr;
        auto t = pq_.top();
        pq_.pop();
        return t;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
    }

private:
    std::priority_queue<std::shared_ptr<Task>, std::vector<std::shared_ptr<Task>>, TaskCompare> pq_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_{false};
};

// ------------------------------
// Application registry (maps logical apps → bitstreams / kernel metadata)
// ------------------------------
struct AppDescriptor {
    std::string app;                // name, e.g., "sobel"
    std::string bitstream_path;     // for FPGA PR (e.g., .bit or partial .bin)
    std::string kernel_name;        // optional, for runtime control libraries
    // Add static resource hints if helpful (BRAM/clock/slots/etc.)
};

class ApplicationRegistry {
public:
    void register_app(const AppDescriptor& d) {
        std::lock_guard<std::mutex> lk(mu_);
        apps_[d.app] = d;
    }
    std::optional<AppDescriptor> lookup(const std::string& app) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = apps_.find(app);
        if (it == apps_.end()) return std::nullopt;
        return it->second;
    }
private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, AppDescriptor> apps_;
};

// ------------------------------
// Accelerator interface + backends
// ------------------------------
struct ExecutionResult {
    Task::TaskId id;
    bool ok;
    std::string message;
    std::chrono::nanoseconds runtime_ns{0};
};

class Accelerator {
public:
    virtual ~Accelerator() = default;
    virtual std::string name() const = 0;
    virtual bool is_available() = 0;

    // Called when switching app (PR/reconfig). May be a no-op on CPU.
    virtual bool ensure_app_loaded(const AppDescriptor& app) = 0;

    // Submit and run synchronously (scheduler may wrap in a worker thread).
    virtual ExecutionResult run(const Task& task, const AppDescriptor& app) = 0;
};

// Mock CPU backend: simulates runtime by sleeping est_runtime_ns (or a fallback)
class CpuMockAccelerator : public Accelerator {
public:
    explicit CpuMockAccelerator(unsigned id = 0) : id_(id) {}
    std::string name() const override { return "cpu-mock-" + std::to_string(id_); }
    bool is_available() override { return true; }
    bool ensure_app_loaded(const AppDescriptor&) override { return true; }

    ExecutionResult run(const Task& task, const AppDescriptor& app) override {
        auto t0 = std::chrono::steady_clock::now();
        auto dur = task.est_runtime_ns.count() > 0 ? task.est_runtime_ns
                                                   : std::chrono::nanoseconds(10000000);
        std::this_thread::sleep_for(dur);
        auto t1 = std::chrono::steady_clock::now();
        return {
            task.id,
            true,
            "Executed " + app.app + " on mock CPU",
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
        };
    }
private:
    unsigned id_;
};

// FPGA backend (stub): here’s where you hook partial reconfiguration + kernel control.
// On PYNQ you might:
//  - invoke a small service/daemon that wraps Linux fpga_manager to load a partial bitstream
//  - coordinate with your kernel driver or userspace DMA to launch kernels
//  - or, if you already have a C++ control lib for your overlays, call into it here
class FpgaAccelerator : public Accelerator {
public:
    explicit FpgaAccelerator(unsigned slot_id = 0) : slot_(slot_id) {}
    std::string name() const override { return "fpga-slot-" + std::to_string(slot_); }

    bool is_available() override {
        // TODO: probe device/driver presence, e.g., check sysfs or device node
        return false; // set true when wired up
    }

    bool ensure_app_loaded(const AppDescriptor& app) override {
        // TODO: implement PR load if different from current_
        // Pseudocode:
        // if (current_bitstream_ != app.bitstream_path) {
        //     pr_load(app.bitstream_path);
        //     current_bitstream_ = app.bitstream_path;
        // }
        return false;
    }

    ExecutionResult run(const Task& task, const AppDescriptor& app) override {
        // TODO: enqueue buffers, start kernel, wait, collect, etc.
        return { task.id, false, "FPGA backend not yet implemented", std::chrono::milliseconds(0) };
    }

private:
    unsigned slot_;
    std::string current_bitstream_;
};

// ------------------------------
// Dependency tracker
// ------------------------------
class DependencyManager {
public:
    void mark_complete(Task::TaskId id) {
        std::lock_guard<std::mutex> lk(mu_);
        completed_.insert(id);
    }
    bool deps_satisfied(const Task& t) const {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& d : t.depends_on) {
            if (completed_.find(d) == completed_.end()) return false;
        }
        return true;
    }
private:
    mutable std::mutex mu_;
    std::set<Task::TaskId> completed_;
};

// ------------------------------
// Scheduler
// ------------------------------
enum class BackendMode { AUTO, FPGA, CPU };

class Scheduler {
public:
    Scheduler(ApplicationRegistry& reg, BackendMode mode, unsigned cpu_workers = std::thread::hardware_concurrency())
        : reg_(reg), mode_(mode), cpu_workers_(cpu_workers ? cpu_workers : 1) {}

    ~Scheduler() {
        stop();
    }

    void add_accelerator(std::unique_ptr<Accelerator> acc) {
        std::lock_guard<std::mutex> lk(mu_);
        accelerators_.push_back(std::move(acc));
    }

    // Enqueue a task; if deps satisfied, it goes to ready queue; else holds until they are.
    void submit(const std::shared_ptr<Task>& t) {
        if (deps_.deps_satisfied(*t)) {
            t->ready.store(true, std::memory_order_relaxed);
            ready_.push(t);
        } else {
            std::lock_guard<std::mutex> lk(mu_waiting_);
            waiting_.push_back(t);
        }
    }

    void start() {
        if (running_.exchange(true)) return;

        // Resolve backend choice
        bool fpga_ok = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& a : accelerators_) {
                if (a->name().find("fpga") != std::string::npos && a->is_available()) {
                    fpga_ok = true;
                }
            }
        }

        use_cpu_ = (mode_ == BackendMode::CPU) ||
                   (mode_ == BackendMode::AUTO && !fpga_ok);

        // Worker threads
        for (unsigned i = 0; i < cpu_workers_; ++i) {
            workers_.emplace_back([this]{ worker_loop(); });
        }

        // Watcher to promote waiting → ready when deps complete
        dep_thread_ = std::thread([this]{ dependency_watcher(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        ready_.stop();
        if (dep_thread_.joinable()) dep_thread_.join();
        for (auto& t : workers_) if (t.joinable()) t.join();
    }

private:
    void dependency_watcher() {
        using namespace std::chrono_literals;
        while (running_) {
            {
                std::lock_guard<std::mutex> lk(mu_waiting_);
                auto it = waiting_.begin();
                while (it != waiting_.end()) {
                    auto& ts = *it;
                    if (deps_.deps_satisfied(*ts)) {
                        ts->ready.store(true, std::memory_order_relaxed);
                        ready_.push(ts);
                        it = waiting_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            std::this_thread::sleep_for(1ms);
        }
    }

    void worker_loop() {
        while (running_) {
            auto task = ready_.pop_blocking();
            if (!task) break;

            auto appOpt = reg_.lookup(task->app);
            if (!appOpt) {
                report_result({task->id, false, "Unknown app: " + task->app, std::chrono::milliseconds(0)});
                continue;
            }
            auto app = *appOpt;

            // Pick an accelerator
            std::unique_ptr<Accelerator>* chosen = nullptr;

            {
                std::lock_guard<std::mutex> lk(mu_);
                // Very simple policy: prefer FPGA if we’re using it; otherwise CPU mock
                if (!use_cpu_) {
                    for (auto& a : accelerators_) {
                        if (a->name().find("fpga") != std::string::npos && a->is_available()) {
                            chosen = &a;
                            break;
                        }
                    }
                }
                if (!chosen) {
                    for (auto& a : accelerators_) {
                        if (a->name().find("cpu-mock") != std::string::npos && a->is_available()) {
                            chosen = &a;
                            break;
                        }
                    }
                }
            }

            if (!chosen) {
                report_result({task->id, false, "No accelerator available", std::chrono::milliseconds(0)});
                continue;
            }

            // Ensure correct bitstream/app is loaded (no-op on CPU)
            if (!(*chosen)->ensure_app_loaded(app)) {
                if (!use_cpu_) {
                    // Optional: fall back to CPU if FPGA load fails
                    // Try to find CPU mock
                    std::unique_ptr<Accelerator>* cpu = nullptr;
                    std::lock_guard<std::mutex> lk(mu_);
                    for (auto& a : accelerators_) {
                        if (a->name().find("cpu-mock") != std::string::npos && a->is_available()) {
                            cpu = &a; break;
                        }
                    }
            if (cpu) {
                auto r = (*cpu)->run(*task, app);
                report_result(r);
                deps_.mark_complete(task->id);
                continue;
            }
        }
        report_result({task->id, false, "Failed to load app on accelerator", std::chrono::nanoseconds(0)});
        continue;
    }

            // Execute
            auto r = (*chosen)->run(*task, app);
                report_result(r);
                if (r.ok) deps_.mark_complete(task->id);
            }
        }

    void report_result(const ExecutionResult& r) {
        // Hook your metrics/logging bus here
        std::lock_guard<std::mutex> lk(io_mu_);
        std::cout << "[RESULT] Task " << r.id << " ok=" << (r.ok ? "true" : "false")
                  << " msg=\"" << r.message << "\" time_ns=" << r.runtime_ns.count() << "\n";
    }

private:
    ApplicationRegistry& reg_;
    BackendMode mode_;
    bool use_cpu_{true};

    ReadyQueue ready_;
    DependencyManager deps_;

    std::mutex mu_; // accelerators guard
    std::vector<std::unique_ptr<Accelerator>> accelerators_;

    std::atomic<bool> running_{false};
    std::vector<std::thread> workers_;
    std::thread dep_thread_;

    std::mutex mu_waiting_;
    std::vector<std::shared_ptr<Task>> waiting_;

    unsigned cpu_workers_;
    std::mutex io_mu_; // printing
};

// ------------------------------
// Utility: parse backend flag
// ------------------------------
BackendMode parse_backend(int argc, char** argv) {
    std::string v = "auto";
    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if (a.rfind("--backend=", 0) == 0) v = a.substr(10);
    }
    if (v == "cpu")  return BackendMode::CPU;
    if (v == "fpga") return BackendMode::FPGA;
    return BackendMode::AUTO;
}

// ------------------------------
// Demo main
// ------------------------------
int main(int argc, char** argv) {
    ApplicationRegistry reg;
    reg.register_app({"sobel", "bitstreams/sobel_partial.bit", "sobel_kernel"});
    reg.register_app({"gemm",  "bitstreams/gemm_partial.bit",  "gemm_kernel"});

    auto mode = parse_backend(argc, argv);
    Scheduler sched(reg, mode, /*cpu_workers=*/4);

    // Add accelerators (order: FPGA first for AUTO preference)
    sched.add_accelerator(std::make_unique<FpgaAccelerator>(0)); // stubbed
    // Add a couple CPU mock "slots" to simulate multi-accelerator throughput
    sched.add_accelerator(std::make_unique<CpuMockAccelerator>(0));
    sched.add_accelerator(std::make_unique<CpuMockAccelerator>(1));

    sched.start();

    // Create a small DAG:
    // t1: sobel
    // t2: gemm depends on t1
    // t3: sobel independent
    auto now = std::chrono::steady_clock::now();

    auto t1 = std::make_shared<Task>();
    t1->id = 1; t1->app = "sobel"; t1->priority = 5;
    t1->release_time = now;
    t1->est_runtime_ns = std::chrono::nanoseconds(120000000);

    auto t2 = std::make_shared<Task>();
    t2->id = 2; t2->app = "gemm"; t2->priority = 3;
    t2->depends_on = {1};
    t2->est_runtime_ns = std::chrono::nanoseconds(250000000);

    auto t3 = std::make_shared<Task>();
    t3->id = 3; t3->app = "sobel"; t3->priority = 4;
    t3->est_runtime_ns = std::chrono::nanoseconds(80000000);

    sched.submit(t1);
    sched.submit(t2);
    sched.submit(t3);

    // Let it run for a moment
    std::this_thread::sleep_for(std::chrono::seconds(2));
    sched.stop();
    return 0;
}
