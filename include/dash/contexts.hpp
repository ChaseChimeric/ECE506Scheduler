#pragma once
#include "dash/types.hpp"
#include <cstddef>
#include <string>

namespace dash {

struct ZipContext {
    ZipParams params;
    BufferView in{};
    BufferView out{};
    size_t* out_actual = nullptr;
    bool ok = false;
    std::string message;
};

struct FftContext {
    FftPlan plan{};
    BufferView in{};
    BufferView out{};
    bool ok = false;
    std::string message;
};

inline constexpr const char kZipContextKey[] = "dash.zip_ctx";
inline constexpr const char kFftContextKey[] = "dash.fft_ctx";

} // namespace dash
