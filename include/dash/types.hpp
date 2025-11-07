#pragma once
#include <cstddef>
#include <cstdint>

namespace dash {

struct BufferView {
    void*  data;
    size_t bytes;
};

// ZIP types
enum class ZipMode { Compress, Decompress };
struct ZipParams {
    int     level = 3;
    ZipMode mode  = ZipMode::Compress;
};

// FFT types
struct FftPlan {
    int  n = 0;
    bool inverse = false;
};

} // namespace dash

