#pragma once
#include "schedrt/scheduler.hpp"

namespace dash {

schedrt::Scheduler* scheduler();
void set_scheduler(schedrt::Scheduler* sched);

} // namespace dash
