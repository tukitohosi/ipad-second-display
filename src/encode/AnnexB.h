#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <vector>

// Annex-B byte-format handling per wire spec §4. Kept independent of Media
// Foundation so it can be reasoned about (and tested) on its own: the encoder
// output is scanned into NAL units regardless of whether the MFT emitted
// 3-byte or 4-byte start codes, then reassembled with 4-byte start codes only
// (spec §4.1 — the iPad parser does not recognize 3-byte codes).
namespace od {

constexpr int kNalTypeSps = 7;
constexpr int kNalTypePps = 8;
constexpr int kNalTypeIdrSlice = 5;

struct NalUnit {
    int type = 0;                  // (first payload byte) & 0x1F
    std::vector<uint8_t> payload;  // NAL bytes, start code excluded
};

// Scans a buffer that may mix 3-byte (00 00 01) and 4-byte (00 00 00 01)
// start codes into NAL units. Bytes outside any start-code-delimited region
// (e.g. leading garbage before the first start code) are ignored.
std::vector<NalUnit> ScanStartCodes(const uint8_t* data, size_t size);

// Concatenates NALs into one Annex-B access unit, each prefixed with a
// normalized 4-byte start code (00 00 00 01) — this is the exact byte layout
// sent as one framed video message (spec §4 rules 1-2).
std::vector<uint8_t> BuildAccessUnit(const std::vector<NalUnit>& nals);

struct SpsColorInfo {
    bool vuiPresent = false;
    bool videoSignalPresent = false;
    bool colorDescriptionPresent = false;
    bool fullRange = false;
    uint8_t primaries = 0;
    uint8_t transfer = 0;
    uint8_t matrix = 0;

    bool IsBt709Limited() const
    {
        return videoSignalPresent && colorDescriptionPresent && !fullRange &&
               primaries == 1 && transfer == 1 && matrix == 1;
    }
};

// Reads the actual emitted SPS, not just the MFT's requested media type.
// Invalid/truncated bitstreams return nullopt without modifying their input.
std::optional<SpsColorInfo> InspectSpsColor(const NalUnit& sps);
enum class SpsColorResult { Unchanged, Updated, Invalid };
// Corrects only video_signal_type in VUI (inserting VUI when absent). Coded
// picture data, geometry, timing and all other SPS fields are preserved.
SpsColorResult EnsureSpsBt709Limited(NalUnit& sps);

// Tracks the most recently seen SPS/PPS and prepends them to any access unit
// that contains an IDR slice but doesn't already have its own SPS+PPS
// immediately in front of it (spec §4 rule 3). Without this, an encoder that
// doesn't repeat parameter sets on every IDR would produce a black screen on
// the receiver.
class SpsPpsCache {
public:
    std::vector<NalUnit> EnsureParameterSets(const std::vector<NalUnit>& nals);

private:
    std::optional<NalUnit> sps_;
    std::optional<NalUnit> pps_;
};

} // namespace od
