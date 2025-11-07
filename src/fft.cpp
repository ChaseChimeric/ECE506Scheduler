// src/dash/fft.cpp
#include "dash/fft.hpp"
#include "dash/provider.hpp"
#include "schedrt/scheduler.hpp"
#include "schedrt/task.hpp"

extern schedrt::Scheduler* g_sched; // injected/wired during app boot

namespace dash {

struct FftCtx { FftPlan plan; BufferView in, out; bool ok=false; std::string msg; };

bool fft_execute(const FftPlan& plan, BufferView in, BufferView out) {
    // Pick a provider (prefer HW FFT, then CPU FFT fallback)
    auto provs = providers_for("fft");        // already sorted by priority
    if (provs.empty()) return false;

    // Fill a Task with opaque pointer to context
    auto ctx = std::make_shared<FftCtx>(FftCtx{plan, in, out});
    auto t = std::make_shared<schedrt::Task>();
    t->id = /* generate */;
    t->app = "fft";                            // used to lookup descriptor
    t->est_runtime_ms = std::chrono::milliseconds(5); // rough guess

    // Decide target kind from provider[0]; scheduler uses the kind to route.
    t->required = provs.front().kind;

    // Attach opaque pointer so the device worker can call provider.run(ctx.get()).
    t->params["__ctx_ptr"] = std::to_string(reinterpret_cast<uintptr_t>(ctx.get()));

    g_sched->submit(t);

    // For a simple synchronous API, wait by polling or use a promise/future you set in the worker.
    // (Omitted for brevity.)
    // return ctx->ok;
    return true;
}

} // namespace dash

