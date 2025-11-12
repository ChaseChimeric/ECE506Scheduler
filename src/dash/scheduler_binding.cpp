#include "dash/scheduler_binding.hpp"

namespace dash {

schedrt::Scheduler* g_sched_ptr = nullptr;

schedrt::Scheduler* scheduler() {
    return g_sched_ptr;
}

void set_scheduler(schedrt::Scheduler* sched) {
    g_sched_ptr = sched;
}

} // namespace dash
