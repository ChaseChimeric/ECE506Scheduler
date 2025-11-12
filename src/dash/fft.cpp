#include "dash/contexts.hpp"
#include "dash/contexts.hpp"
#include "dash/fft.hpp"
#include "dash/provider.hpp"
#include "dash/scheduler_binding.hpp"
#include "dash/completion_bus.hpp"
#include "schedrt/scheduler.hpp"
#include "schedrt/task.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace {
uint64_t next_id() {
    static std::atomic<uint64_t> c{1000};
    return c.fetch_add(1, std::memory_order_relaxed);
}
}

namespace dash {
bool fft_execute(const FftPlan& plan, BufferView in, BufferView out) {
    auto provs = providers_for("fft");
    if (provs.empty()) return false;

    auto kind = provs.front().kind;
    auto ctx = std::make_shared<FftContext>();
    ctx->plan = plan;
    ctx->in = in;
    ctx->out = out;

    auto t = std::make_shared<schedrt::Task>();
    t->id = next_id();
    t->app = "fft";
    t->required = kind;
    t->est_runtime_ns = std::chrono::nanoseconds(15000000);
    t->params.emplace(kFftContextKey, std::to_string(reinterpret_cast<std::uintptr_t>(ctx.get())));

    auto fut = dash::subscribe(t->id);
    auto* sched = dash::scheduler();
    if (!sched) return false;
    sched->submit(t);
    return fut.get();
}
} // namespace dash
