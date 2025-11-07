#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace schedrt {

enum class ResourceKind { CPU, ZIP, FFT };

struct Task {
    using TaskId = uint64_t;

    TaskId id{};
    std::string app;  // logical app name (e.g. "sobel", "gemm")
    int priority{0};  // higher = sooner
    std::chrono::steady_clock::time_point release_time{std::chrono::steady_clock::now()};
    std::optional<std::chrono::steady_clock::time_point> deadline{};
    std::vector<TaskId> depends_on{};
    std::unordered_map<std::string, std::string> params{};
    std::chrono::milliseconds est_runtime_ms{0};
    ResourceKind required{ResourceKind::CPU};
    std::atomic<bool> ready{false};
};

struct ExecutionResult {
    Task::TaskId id{};
    bool ok{false};
    std::string message;
    std::chrono::milliseconds runtime_ms{0};
};

} // namespace schedrt

