#pragma once
#include <cstdint>
#include <future>

namespace dash {
// Register a promise for a task id; returns future to wait on
std::future<bool> subscribe(uint64_t task_id);
// Fulfill completion (called by scheduler on task end)
void fulfill(uint64_t task_id, bool ok);
} // namespace dash

