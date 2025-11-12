#pragma once
#include "schedrt/application_registry.hpp"
#include "schedrt/scheduler.hpp"

#ifdef __cplusplus
extern "C" {
#endif

void app_initialize(int argc, char** argv, schedrt::ApplicationRegistry& reg, schedrt::Scheduler& sched);
int app_run(int argc, char** argv, schedrt::Scheduler& sched);

#ifdef __cplusplus
}
#endif
