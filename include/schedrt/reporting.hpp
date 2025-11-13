#pragma once
#include <atomic>

namespace schedrt {
namespace reporting {

void set_csv(bool value);
bool csv_enabled();

} // namespace reporting
} // namespace schedrt
