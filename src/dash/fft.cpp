#include "dash/fft.hpp"
#include "dash/provider.hpp"
#include "dash/completion_bus.hpp"
#include "schedrt/scheduler.hpp"
#include "schedrt/task.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <random>

extern schedrt::Scheduler* g_sched; // defined in the demo app for simplicity

namespace {
uint64_t next_id() {
    static std::atomic<uint64_t> c{1000};
    return c.fetch_add(1, std::memory_order_relaxed);
}
}

namespace dash {
bool fft_execute(const FftPlan& plan, BufferView in, BufferView out) {
    (void)plan; (void)in; (void)out; // In a real system you'd pass buffers via Task params

    auto provs = providers_for("fft");
    if (provs.empty()) return false;

    // Pick preferred kind (first provider)
    auto kind = provs.front().kind;

    auto t = std::make_shared<schedrt::Task>();
    t->id = next_id();
    t->app = "fft";                 // must exist in ApplicationRegistry
    t->required = kind;
    t->est_runtime_ms = std::chrono::milliseconds(15);

    // Subscribe for completion and submit
    auto fut = dash::subscribe(t->id);
    g_sched->submit(t);

    // Wait synchronously for demo
    return fut.get();
}
} // namespace dash

