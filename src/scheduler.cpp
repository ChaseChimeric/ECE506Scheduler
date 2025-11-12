
#include "schedrt/scheduler.hpp"
#include "dash/completion_bus.hpp"
#include <algorithm>
#include <iostream>
#include <set>
#include <unordered_map>
#include <vector>

namespace schedrt {

class DependencyManager {
public:
    void mark_complete(Task::TaskId id) {
        std::lock_guard<std::mutex> lk(mu_);
        completed_.insert(id);
    }
    bool deps_satisfied(const Task& t) const {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto d : t.depends_on) if (!completed_.count(d)) return false;
        return true;
    }
private:
    mutable std::mutex mu_;
    std::set<Task::TaskId> completed_;
};

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
        cv_.wait(lk, [&]{ return stop_ || !pq_.empty(); });
        if (stop_) return nullptr;
        auto t = pq_.top(); pq_.pop(); return t;
    }
    void stop() { { std::lock_guard<std::mutex> lk(mu_); stop_ = true; } cv_.notify_all(); }
private:
    std::priority_queue<std::shared_ptr<Task>, std::vector<std::shared_ptr<Task>>, TaskCompare> pq_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_{false};
};

class Scheduler::Impl {
public:
    Impl(ApplicationRegistry& reg, BackendMode mode, unsigned cpu_workers, unsigned overlay_preload_threshold)
        : reg_(reg),
          mode_(mode),
          cpu_workers_(cpu_workers ? cpu_workers : std::thread::hardware_concurrency()),
          overlay_preload_threshold_(overlay_preload_threshold) {}

    ~Impl() { stop(); }

    void add_accelerator(std::unique_ptr<Accelerator> acc) {
        std::lock_guard<std::mutex> lk(mu_acc_);
        accelerators_.push_back(std::move(acc));
    }

    void submit(const std::shared_ptr<Task>& t) {
        if (deps_.deps_satisfied(*t)) {
            t->ready.store(true);
            ready_.push(t);
            record_ready(t, +1);
        } else {
            std::lock_guard<std::mutex> lk(mu_wait_);
            waiting_.push_back(t);
        }
    }

    void start() {
        if (running_.exchange(true)) return;

        bool fpga_ok = false;
        {
            std::lock_guard<std::mutex> lk(mu_acc_);
            for (auto& a : accelerators_) {
                if (a->name().find("fpga") != std::string::npos && a->is_available()) {
                    fpga_ok = true; break;
                }
            }
        }
        use_cpu_ = (mode_ == BackendMode::CPU) || (mode_ == BackendMode::AUTO && !fpga_ok);

        // workers
        for (unsigned i = 0; i < cpu_workers_; ++i)
            workers_.emplace_back([this]{ worker_loop(); });

        // dependency watcher
        dep_thread_ = std::thread([this]{ dep_loop(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        ready_.stop();
        if (dep_thread_.joinable()) dep_thread_.join();
        for (auto& w : workers_) if (w.joinable()) w.join();
        workers_.clear();
    }

private:
    void record_ready(const std::shared_ptr<Task>& task, int delta);
    Accelerator* select_accelerator(const std::shared_ptr<Task>& task, const AppDescriptor& app);
    void maybe_preload(const std::string& app);

    void dep_loop() {
        using namespace std::chrono_literals;
        while (running_) {
            {
                std::lock_guard<std::mutex> lk(mu_wait_);
                auto it = waiting_.begin();
                while (it != waiting_.end()) {
                        if (deps_.deps_satisfied(**it)) {
                            (*it)->ready.store(true);
                            ready_.push(*it);
                            record_ready(*it, +1);
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
            record_ready(task, -1);

            auto appOpt = reg_.lookup(task->app);
           if (!appOpt) { report({task->id, false, "Unknown app: " + task->app, std::chrono::milliseconds(0)}); continue; } 
            auto app = *appOpt;

            Accelerator* chosen = select_accelerator(task, app);
            if (!chosen) {
                report({task->id, false, "No accelerator available", std::chrono::milliseconds(0)});
                continue;
            }

            auto r = chosen->run(*task, app);
            report(r);
            if (r.ok) deps_.mark_complete(task->id);
        }
    }

    void report(const ExecutionResult& r) {
        std::lock_guard<std::mutex> lk(io_);
        std::cout << "[RESULT] Task " << r.id << " ok=" << (r.ok ? "true" : "false")
                  << " msg=\"" << r.message << "\" time_ns=" << r.runtime_ns.count() << "\n";
        dash::fulfill(r.id, r.ok);
    }

private:
    ApplicationRegistry& reg_;
    BackendMode mode_;
    bool use_cpu_{true};
    unsigned cpu_workers_;
    unsigned overlay_preload_threshold_;
    std::unordered_map<std::string, int> ready_app_counts_;
    std::mutex ready_counts_mu_;

    std::atomic<bool> running_{false};
    ReadyQueue ready_;
    DependencyManager deps_;

    std::mutex mu_acc_;
    std::vector<std::unique_ptr<Accelerator>> accelerators_;

    std::mutex mu_wait_;
    std::vector<std::shared_ptr<Task>> waiting_;

    std::vector<std::thread> workers_;
    std::thread dep_thread_;

    std::mutex io_;
};

void Scheduler::Impl::record_ready(const std::shared_ptr<Task>& task, int delta) {
    if (delta == 0) return;
    std::string high_demand_app;
    {
        std::lock_guard<std::mutex> lk(ready_counts_mu_);
        auto it = ready_app_counts_.find(task->app);
        int count = it != ready_app_counts_.end() ? it->second : 0;
        count = std::max(0, count + delta);
        if (count == 0) {
            ready_app_counts_.erase(task->app);
        } else {
            ready_app_counts_[task->app] = count;
            if (delta > 0 && overlay_preload_threshold_ > 0 && count >= static_cast<int>(overlay_preload_threshold_)) {
                high_demand_app = task->app;
            }
        }
    }
    if (!high_demand_app.empty()) maybe_preload(high_demand_app);
}

Accelerator* Scheduler::Impl::select_accelerator(const std::shared_ptr<Task>& task, const AppDescriptor& app) {
    std::vector<Accelerator*> cpu_candidates;
    std::vector<Accelerator*> reconfigurable;
    {
        std::lock_guard<std::mutex> lk(mu_acc_);
        for (auto& acc : accelerators_) {
            if (!acc->is_available()) continue;
            if (acc->is_reconfigurable()) {
                reconfigurable.push_back(acc.get());
            } else {
                cpu_candidates.push_back(acc.get());
            }
        }
    }

    if (!use_cpu_ && task->required != ResourceKind::CPU) {
        for (auto* acc : reconfigurable) {
            auto* slot = dynamic_cast<FpgaSlotAccelerator*>(acc);
            if (!slot) continue;
            if (slot->current_app() == task->app || slot->ensure_app_loaded(app)) {
                return acc;
            }
        }
    }

    if (!cpu_candidates.empty()) return cpu_candidates.front();
    if (!use_cpu_ && !reconfigurable.empty()) return reconfigurable.front();
    return nullptr;
}

void Scheduler::Impl::maybe_preload(const std::string& app) {
    if (use_cpu_ || overlay_preload_threshold_ == 0) return;
    auto descOpt = reg_.lookup(app);
    if (!descOpt) return;

    std::vector<FpgaSlotAccelerator*> slots;
    {
        std::lock_guard<std::mutex> lk(mu_acc_);
        for (auto& acc : accelerators_) {
            if (!acc->is_available()) continue;
            if (auto* slot = dynamic_cast<FpgaSlotAccelerator*>(acc.get())) {
                if (slot->current_app() == app) return;
                slots.push_back(slot);
            }
        }
    }

    for (auto* slot : slots) {
        if (slot->ensure_app_loaded(*descOpt)) return;
    }
}


// -------------- thin wrappers --------------
Scheduler::Scheduler(ApplicationRegistry& reg, BackendMode mode, unsigned cpu_workers,
                     unsigned overlay_preload_threshold)
    : impl_(std::make_unique<Impl>(reg, mode, cpu_workers, overlay_preload_threshold)) {}

Scheduler::~Scheduler() = default;

void Scheduler::add_accelerator(std::unique_ptr<Accelerator> acc) { impl_->add_accelerator(std::move(acc)); }
void Scheduler::submit(const std::shared_ptr<Task>& t) { impl_->submit(t); }
void Scheduler::start() { impl_->start(); }
void Scheduler::stop() { impl_->stop(); }

} // namespace schedrt
