#include "dash/contexts.hpp"
#include "dash/contexts.hpp"
#include "dash/provider.hpp"
#include "dash/scheduler_binding.hpp"
#include "dash/zip.hpp"
#include "dash/completion_bus.hpp"
#include "schedrt/scheduler.hpp"
#include "schedrt/task.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace {
uint64_t next_id() {
    static std::atomic<uint64_t> c{2000};
    return c.fetch_add(1, std::memory_order_relaxed);
}
}

namespace dash {
bool zip_execute(const ZipParams& z, BufferView in, BufferView out, size_t& out_actual) {
    auto provs = providers_for("zip");
    if (provs.empty()) return false;

    auto kind = provs.front().kind;
    auto ctx = std::make_shared<ZipContext>();
    ctx->params = z;
    ctx->in = in;
    ctx->out = out;
    ctx->out_actual = &out_actual;

    auto t = std::make_shared<schedrt::Task>();
    t->id = next_id();
    t->app = "zip";
    t->required = kind;
    t->est_runtime_ns = std::chrono::nanoseconds(12000000);
    t->params.emplace(kZipContextKey, std::to_string(reinterpret_cast<std::uintptr_t>(ctx.get())));

    auto fut = dash::subscribe(t->id);
    auto* sched = dash::scheduler();
    if (!sched) return false;
    sched->submit(t);
    auto ok = fut.get();
    return ok;
}
} // namespace dash
