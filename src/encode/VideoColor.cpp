#include "encode/VideoColor.h"

#include <algorithm>
#include <limits>

namespace od {

bool ConvertBgraToBt709Nv12(const uint8_t* bgra, size_t rowPitch, uint32_t width,
                          uint32_t height, std::vector<uint8_t>& nv12)
{
    if (bgra == nullptr || width == 0 || height == 0 || (width & 1u) || (height & 1u) ||
        width > std::numeric_limits<size_t>::max() / 4 || rowPitch < static_cast<size_t>(width) * 4 ||
        rowPitch > std::numeric_limits<size_t>::max() / height ||
        static_cast<size_t>(width) > std::numeric_limits<size_t>::max() / height)
        return false;

    const size_t pixels = static_cast<size_t>(width) * height;
    if (pixels > std::numeric_limits<size_t>::max() - pixels / 2)
        return false;
    nv12.resize(pixels + pixels / 2);
    uint8_t* uvPlane = nv12.data() + pixels;

    // 16-bit fixed-point coefficients from BT.709 Kr=.2126, Kb=.0722.
    // Rounding after the full sum keeps solid-color error below one code
    // value and avoids the gray drift caused by low-precision coefficients.
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* src = bgra + static_cast<size_t>(row) * rowPitch;
        uint8_t* dst = nv12.data() + static_cast<size_t>(row) * width;
        for (uint32_t col = 0; col < width; ++col, src += 4)
            dst[col] = static_cast<uint8_t>(16 + ((11966 * src[2] + 40254 * src[1] + 4064 * src[0] + 32768) >> 16));
    }

    for (uint32_t row = 0; row < height; row += 2) {
        const uint8_t* top = bgra + static_cast<size_t>(row) * rowPitch;
        const uint8_t* bottom = top + rowPitch;
        uint8_t* dst = uvPlane + static_cast<size_t>(row / 2) * width;
        for (uint32_t col = 0; col < width; col += 2, top += 8, bottom += 8) {
            const int b = top[0] + top[4] + bottom[0] + bottom[4];
            const int g = top[1] + top[5] + bottom[1] + bottom[5];
            const int r = top[2] + top[6] + bottom[2] + bottom[6];
            // Average the 2x2 chroma samples before subsampling, retaining
            // fractional RGB averages until the final rounding operation.
            dst[col] = static_cast<uint8_t>(std::clamp(128 + ((-6596 * r - 22189 * g + 28784 * b + 131072) >> 18), 16, 240));
            dst[col + 1] = static_cast<uint8_t>(std::clamp(128 + ((28784 * r - 26145 * g - 2639 * b + 131072) >> 18), 16, 240));
        }
    }
    return true;
}

} // namespace od
