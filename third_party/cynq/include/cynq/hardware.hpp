/*
 * Simplified hardware parameter definitions borrowed from Cynq.
 * We only expose the minimum that the allocator path needs.
 */
#pragma once

#include <memory>
#include <xrt/xrt_device.h>

namespace cynq {
struct HardwareParameters {
    virtual ~HardwareParameters() = default;
};

struct UltraScaleParameters : public HardwareParameters {
    xrt::device device_;
};
}  // namespace cynq
