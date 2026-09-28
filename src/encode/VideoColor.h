#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace od {

// SDR BT.709, video range: Y 16..235 and Cb/Cr 16..240. The encoder's
// media types and emitted SPS must declare this same conversion.
// Width/height must be even. Invalid geometry leaves output unchanged.
bool ConvertBgraToBt709Nv12(const uint8_t* bgra, size_t rowPitch, uint32_t width,
                          uint32_t height, std::vector<uint8_t>& nv12);

} // namespace od
