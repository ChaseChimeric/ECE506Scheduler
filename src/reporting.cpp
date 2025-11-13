#include "schedrt/reporting.hpp"

namespace schedrt {
namespace reporting {

static std::atomic<bool> g_csv{false};

void set_csv(bool value) {
    g_csv.store(value, std::memory_order_relaxed);
}

bool csv_enabled() {
    return g_csv.load(std::memory_order_relaxed);
}

} // namespace reporting
} // namespace schedrt
