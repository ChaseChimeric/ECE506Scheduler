#pragma once
#include "schedrt/task.hpp"
#include <string>
#include <vector>

namespace dash {

struct Provider {
    std::string op;                   // "zip" or "fft"
    schedrt::ResourceKind kind;       // which queue/device family
    unsigned instance_id;             // informative; scheduling is by kind
    int priority;                     // 0 = most preferred (HW), higher = fallback (CPU)
};

// Register and query (thread-safe, implemented in provider.cpp)
void register_provider(const Provider& p);
std::vector<Provider> providers_for(const std::string& op);

} // namespace dash

