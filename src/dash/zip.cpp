#include "dash/zip.hpp"
#include "dash/provider.hpp"
#include "dash/completion_bus.hpp"
#include "schedrt/scheduler.hpp"
#include "schedrt/task.hpp"
#include <atomic>
#include <chrono>
#include <memory>

extern schedrt::Scheduler* g_sched;

namespace {
uint64_t next_id() {
    static std::atomic<uint64_t> c{2000};
    return c.fetch_add(1, std::memory_order_relaxed);
}
}

namespace dash {
bool zip_execute(const ZipParams& z, BufferView in, BufferView out, size_t& out_actual) {
    (void)z; (void)in; (void)out; out_actual = 0;

    auto provs = providers_for("zip");
    if (provs.empty()) return false;

    auto kind = provs.front().kind;

    auto t = std::make_shared<schedrt::Task>();
    t->id = next_id();
    t->app = "zip";
    t->required = kind;
    t->est_runtime_ms = std::chrono::milliseconds(12);

    auto fut = dash::subscribe(t->id);
    g_sched->submit(t);
    return fut.get();
}
} // namespace dash

