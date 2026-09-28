#include "encode/AnnexB.h"

#include <algorithm>
#include <limits>

namespace od {

namespace {

struct Marker {
    size_t codeBegin;
    size_t payloadBegin;
};

// One byte per bit keeps the SPS splice auditable. SPS units are tiny and are
// processed only when a parameter set is emitted, never once per video pixel.
struct Bits {
    std::vector<uint8_t> values;
    size_t position = 0;

    bool Read(unsigned count, uint32_t& value)
    {
        if (count > 32 || position > values.size() || count > values.size() - position)
            return false;
        value = 0;
        for (unsigned i = 0; i < count; ++i)
            value = (value << 1) | values[position++];
        return true;
    }
    bool Skip(unsigned count)
    {
        uint32_t unused = 0;
        return Read(count, unused);
    }
    bool Ue(uint32_t& value)
    {
        uint32_t bit = 0;
        unsigned zeros = 0;
        while (Read(1, bit) && bit == 0) {
            if (++zeros > 31)
                return false;
        }
        if (bit == 0)
            return false;
        uint32_t suffix = 0;
        if (!Read(zeros, suffix))
            return false;
        value = ((uint32_t{1} << zeros) - 1) + suffix;
        return true;
    }
    bool SkipUe()
    {
        uint32_t unused = 0;
        return Ue(unused);
    }
};

struct SpsLayout {
    Bits bits;
    size_t vuiFlag = 0;
    size_t signalBegin = 0;
    size_t signalEnd = 0;
    SpsColorInfo color;
};

bool SkipScalingList(Bits& bits, unsigned size)
{
    int lastScale = 8;
    int nextScale = 8;
    for (unsigned i = 0; i < size; ++i) {
        if (nextScale != 0) {
            uint32_t code = 0;
            if (!bits.Ue(code))
                return false;
            const int64_t delta = (code & 1) ? (static_cast<int64_t>(code) + 1) / 2 :
                                              -static_cast<int64_t>(code) / 2;
            nextScale = static_cast<int>((lastScale + delta) & 255);
        }
        if (nextScale != 0)
            lastScale = nextScale;
    }
    return true;
}

bool SkipHrd(Bits& bits)
{
    uint32_t count = 0;
    if (!bits.Ue(count) || count > 31 || !bits.Skip(8))
        return false;
    for (uint32_t i = 0; i <= count; ++i)
        if (!bits.SkipUe() || !bits.SkipUe() || !bits.Skip(1))
            return false;
    return bits.Skip(20);
}

std::optional<SpsLayout> ParseSps(const NalUnit& nal)
{
    if (nal.type != kNalTypeSps || nal.payload.size() < 5 || nal.payload.size() > 65536 ||
        (nal.payload[0] & 0x9f) != kNalTypeSps)
        return std::nullopt;
    SpsLayout layout;
    unsigned zeroCount = 0;
    for (size_t i = 1; i < nal.payload.size(); ++i) {
        const uint8_t byte = nal.payload[i];
        if (zeroCount == 2 && byte == 3) {
            if (i + 1 >= nal.payload.size() || nal.payload[i + 1] > 3)
                return std::nullopt;
            zeroCount = 0;
            continue;
        }
        for (int shift = 7; shift >= 0; --shift)
            layout.bits.values.push_back((byte >> shift) & 1);
        zeroCount = byte == 0 ? std::min(zeroCount + 1, 2u) : 0;
    }
    // rbsp_stop_one_bit followed by zero alignment/trailing_zero_8bits.
    auto& bits = layout.bits;
    while (!bits.values.empty() && bits.values.back() == 0)
        bits.values.pop_back();
    if (bits.values.empty())
        return std::nullopt;
    bits.values.pop_back();

    uint32_t profile = 0, value = 0;
    if (!bits.Read(8, profile) || !bits.Skip(16) || !bits.Ue(value) || value > 31)
        return std::nullopt;
    const bool highProfile = profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
                             profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
                             profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135;
    if (highProfile) {
        uint32_t chroma = 0;
        if (!bits.Ue(chroma) || chroma > 3 || (chroma == 3 && !bits.Skip(1)) ||
            !bits.SkipUe() || !bits.SkipUe() || !bits.Skip(1) || !bits.Read(1, value))
            return std::nullopt;
        if (value) {
            for (unsigned i = 0; i < (chroma == 3 ? 12u : 8u); ++i) {
                if (!bits.Read(1, value) || (value && !SkipScalingList(bits, i < 6 ? 16 : 64)))
                    return std::nullopt;
            }
        }
    }
    uint32_t orderType = 0;
    if (!bits.SkipUe() || !bits.Ue(orderType) || orderType > 2)
        return std::nullopt;
    if (orderType == 0) {
        if (!bits.SkipUe())
            return std::nullopt;
    } else if (orderType == 1) {
        uint32_t count = 0;
        if (!bits.Skip(1) || !bits.SkipUe() || !bits.SkipUe() || !bits.Ue(count) || count > 255)
            return std::nullopt;
        for (uint32_t i = 0; i < count; ++i)
            if (!bits.SkipUe())
                return std::nullopt;
    }
    if (!bits.SkipUe() || !bits.Skip(1) || !bits.SkipUe() || !bits.SkipUe() || !bits.Read(1, value) ||
        (!value && !bits.Skip(1)) || !bits.Skip(1) || !bits.Read(1, value))
        return std::nullopt;
    if (value)
        for (int i = 0; i < 4; ++i)
            if (!bits.SkipUe())
                return std::nullopt;

    layout.vuiFlag = bits.position;
    if (!bits.Read(1, value))
        return std::nullopt;
    layout.color.vuiPresent = value != 0;
    if (value) {
        if (!bits.Read(1, value))
            return std::nullopt;
        if (value && (!bits.Read(8, value) || (value == 255 && !bits.Skip(32))))
            return std::nullopt;
        if (!bits.Read(1, value) || (value && !bits.Skip(1)))
            return std::nullopt;
        layout.signalBegin = bits.position;
        if (!bits.Read(1, value))
            return std::nullopt;
        layout.color.videoSignalPresent = value != 0;
        if (value) {
            if (!bits.Skip(3) || !bits.Read(1, value))
                return std::nullopt;
            layout.color.fullRange = value != 0;
            if (!bits.Read(1, value))
                return std::nullopt;
            layout.color.colorDescriptionPresent = value != 0;
            if (value) {
                if (!bits.Read(8, value)) return std::nullopt;
                layout.color.primaries = static_cast<uint8_t>(value);
                if (!bits.Read(8, value)) return std::nullopt;
                layout.color.transfer = static_cast<uint8_t>(value);
                if (!bits.Read(8, value)) return std::nullopt;
                layout.color.matrix = static_cast<uint8_t>(value);
            }
        }
        layout.signalEnd = bits.position;
        if (!bits.Read(1, value) || (value && (!bits.SkipUe() || !bits.SkipUe())) || !bits.Read(1, value) ||
            (value && (!bits.Skip(32) || !bits.Skip(32) || !bits.Skip(1))))
            return std::nullopt;
        uint32_t nalHrd = 0, vclHrd = 0;
        if (!bits.Read(1, nalHrd) || (nalHrd && !SkipHrd(bits)) ||
            !bits.Read(1, vclHrd) || (vclHrd && !SkipHrd(bits)) ||
            ((nalHrd || vclHrd) && !bits.Skip(1)) || !bits.Skip(1) || !bits.Read(1, value))
            return std::nullopt;
        if (value) {
            if (!bits.Skip(1))
                return std::nullopt;
            for (int i = 0; i < 6; ++i)
                if (!bits.SkipUe())
                    return std::nullopt;
        }
    }
    if (bits.position != bits.values.size())
        return std::nullopt;
    return layout;
}

void AppendBits(std::vector<uint8_t>& bits, uint32_t value, unsigned count)
{
    for (int shift = static_cast<int>(count) - 1; shift >= 0; --shift)
        bits.push_back((value >> shift) & 1);
}

void AppendBt709Signal(std::vector<uint8_t>& bits)
{
    AppendBits(bits, 1, 1); // video_signal_type_present_flag
    AppendBits(bits, 5, 3); // unspecified video format
    AppendBits(bits, 0, 1); // video_full_range_flag: studio/video range
    AppendBits(bits, 1, 1); // colour_description_present_flag
    AppendBits(bits, 1, 8); // colour_primaries: BT.709
    AppendBits(bits, 1, 8); // transfer_characteristics: BT.709
    AppendBits(bits, 1, 8); // matrix_coefficients: BT.709
}

std::vector<Marker> FindMarkers(const uint8_t* data, size_t size)
{
    std::vector<Marker> markers;
    size_t i = 0;
    while (i + 1 < size) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
                markers.push_back({i, i + 4});
                i += 4;
                continue;
            }
            if (i + 2 < size && data[i + 2] == 1) {
                markers.push_back({i, i + 3});
                i += 3;
                continue;
            }
        }
        ++i;
    }
    return markers;
}

} // namespace

std::vector<NalUnit> ScanStartCodes(const uint8_t* data, size_t size)
{
    std::vector<NalUnit> result;
    if (data == nullptr || size == 0)
        return result;

    auto markers = FindMarkers(data, size);
    for (size_t idx = 0; idx < markers.size(); ++idx) {
        size_t payloadBegin = markers[idx].payloadBegin;
        size_t payloadEnd = (idx + 1 < markers.size()) ? markers[idx + 1].codeBegin : size;
        if (payloadBegin >= payloadEnd)
            continue;

        NalUnit nal;
        nal.type = data[payloadBegin] & 0x1F;
        nal.payload.assign(data + payloadBegin, data + payloadEnd);
        result.push_back(std::move(nal));
    }
    return result;
}

std::vector<uint8_t> BuildAccessUnit(const std::vector<NalUnit>& nals)
{
    std::vector<uint8_t> out;
    for (const auto& nal : nals) {
        out.push_back(0x00);
        out.push_back(0x00);
        out.push_back(0x00);
        out.push_back(0x01);
        out.insert(out.end(), nal.payload.begin(), nal.payload.end());
    }
    return out;
}

std::optional<SpsColorInfo> InspectSpsColor(const NalUnit& sps)
{
    auto layout = ParseSps(sps);
    return layout ? std::optional<SpsColorInfo>(layout->color) : std::nullopt;
}

SpsColorResult EnsureSpsBt709Limited(NalUnit& sps)
{
    auto layout = ParseSps(sps);
    if (!layout)
        return SpsColorResult::Invalid;
    if (layout->color.IsBt709Limited())
        return SpsColorResult::Unchanged;

    const auto& original = layout->bits.values;
    std::vector<uint8_t> bits;
    if (layout->color.vuiPresent) {
        bits.insert(bits.end(), original.begin(), original.begin() + layout->signalBegin);
        AppendBt709Signal(bits);
        bits.insert(bits.end(), original.begin() + layout->signalEnd, original.end());
    } else {
        bits.insert(bits.end(), original.begin(), original.begin() + layout->vuiFlag);
        AppendBits(bits, 1, 1); // vui_parameters_present_flag
        AppendBits(bits, 0, 2); // no aspect ratio or overscan override
        AppendBt709Signal(bits);
        AppendBits(bits, 0, 6); // chroma loc, timing, both HRDs, pic struct, restrictions
    }
    AppendBits(bits, 1, 1); // rbsp_stop_one_bit
    while (bits.size() % 8 != 0)
        bits.push_back(0);

    std::vector<uint8_t> payload{sps.payload.front()};
    unsigned zeros = 0;
    for (size_t i = 0; i < bits.size(); i += 8) {
        uint8_t byte = 0;
        for (size_t j = 0; j < 8; ++j)
            byte = static_cast<uint8_t>((byte << 1) | bits[i + j]);
        if (zeros == 2 && byte <= 3) {
            payload.push_back(3); // emulation_prevention_three_byte
            zeros = 0;
        }
        payload.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    NalUnit updated{kNalTypeSps, std::move(payload)};
    const auto checked = InspectSpsColor(updated);
    if (!checked || !checked->IsBt709Limited())
        return SpsColorResult::Invalid;
    sps = std::move(updated);
    return SpsColorResult::Updated;
}

std::vector<NalUnit> SpsPpsCache::EnsureParameterSets(const std::vector<NalUnit>& nals)
{
    for (const auto& nal : nals) {
        if (nal.type == kNalTypeSps)
            sps_ = nal;
        else if (nal.type == kNalTypePps)
            pps_ = nal;
    }

    int idrIndex = -1;
    for (size_t i = 0; i < nals.size(); ++i) {
        if (nals[i].type == kNalTypeIdrSlice) {
            idrIndex = static_cast<int>(i);
            break;
        }
    }
    if (idrIndex < 0)
        return nals; // not a keyframe access unit, nothing to ensure

    bool hasSpsBefore = false;
    bool hasPpsBefore = false;
    for (int i = 0; i < idrIndex; ++i) {
        if (nals[i].type == kNalTypeSps) hasSpsBefore = true;
        if (nals[i].type == kNalTypePps) hasPpsBefore = true;
    }
    if (hasSpsBefore && hasPpsBefore)
        return nals;

    if (!sps_ || !pps_)
        return nals; // nothing cached yet to fix up with (first-ever frame must supply its own)

    std::vector<NalUnit> result;
    result.reserve(nals.size() + 2);
    result.push_back(*sps_);
    result.push_back(*pps_);
    result.insert(result.end(), nals.begin(), nals.end());
    return result;
}

} // namespace od
