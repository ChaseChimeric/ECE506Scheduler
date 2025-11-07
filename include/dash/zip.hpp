#pragma once
#include "dash/types.hpp"

namespace dash {
bool zip_execute(const ZipParams& z, BufferView in, BufferView out, size_t& out_actual);
} // namespace dash

