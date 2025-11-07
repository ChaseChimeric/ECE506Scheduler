#include "dash/provider.hpp"
#include <algorithm>
#include <mutex>

namespace dash {
static std::mutex g_mu;
static std::vector<Provider> g_providers;

void register_provider(const Provider& p) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_providers.push_back(p);
    std::sort(g_providers.begin(), g_providers.end(),
              [](auto& a, auto& b){
                  if (a.op != b.op) return a.op < b.op;
                  if (a.priority != b.priority) return a.priority < b.priority;
                  if (a.kind != b.kind) return a.kind < b.kind;
                  return a.instance_id < b.instance_id;
              });
}

std::vector<Provider> providers_for(const std::string& op) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<Provider> out;
    for (auto& p : g_providers) if (p.op == op) out.push_back(p);
    return out;
}
} // namespace dash

