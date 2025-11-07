#include "dash/completion_bus.hpp"
#include <mutex>
#include <unordered_map>

namespace dash {
static std::mutex g_mu;
static std::unordered_map<uint64_t, std::promise<bool>> g_prom;

std::future<bool> subscribe(uint64_t task_id) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto& pr = g_prom[task_id];
    return pr.get_future();
}

void fulfill(uint64_t task_id, bool ok) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_prom.find(task_id);
    if (it != g_prom.end()) {
        it->second.set_value(ok);
        g_prom.erase(it);
    }
}
} // namespace dash

