#pragma once
#include "accelerator.hpp"
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace schedrt {

class ApplicationRegistry {
public:
    void register_app(const AppDescriptor& d) {
        std::lock_guard<std::mutex> lk(mu_);
        apps_[d.app] = d;
    }
    std::optional<AppDescriptor> lookup(const std::string& name) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = apps_.find(name);
        if (it == apps_.end()) return std::nullopt;
        return it->second;
    }
private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, AppDescriptor> apps_;
};

} // namespace schedrt

