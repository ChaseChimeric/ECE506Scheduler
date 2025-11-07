#pragma once
#include "dash/types.hpp"

namespace dash {
bool fft_execute(const FftPlan& plan, BufferView in, BufferView out);
} // namespace dash

