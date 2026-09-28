#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "encode/AnnexB.h"
#include "encode/VideoColor.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

struct BitWriter {
    std::vector<uint8_t> bits;
    void Put(uint32_t value, unsigned count)
    {
        for (int i = static_cast<int>(count) - 1; i >= 0; --i)
            bits.push_back((value >> i) & 1);
    }
    void Ue(uint32_t value)
    {
        uint32_t code = value + 1;
        unsigned length = 0;
        for (uint32_t n = code; n != 0; n >>= 1)
            ++length;
        Put(0, length - 1);
        Put(code, length);
    }
    od::NalUnit Finish()
    {
        Put(1, 1);
        while (bits.size() % 8) Put(0, 1);
        od::NalUnit nal{od::kNalTypeSps, {0x67}};
        unsigned zeros = 0;
        for (size_t i = 0; i < bits.size(); i += 8) {
            uint8_t byte = 0;
            for (size_t j = 0; j < 8; ++j)
                byte = static_cast<uint8_t>((byte << 1) | bits[i + j]);
            if (zeros == 2 && byte <= 3) {
                nal.payload.push_back(3);
                zeros = 0;
            }
            nal.payload.push_back(byte);
            zeros = byte == 0 ? zeros + 1 : 0;
        }
        return nal;
    }
};

// signal: 0 absent, 1 range only, 2 BT.601/full, 3 BT.709/limited.
od::NalUnit TestSps(bool vui, int signal, bool high, bool extras)
{
    BitWriter w;
    w.Put(high ? 100 : 66, 8); w.Put(0, 8); w.Put(40, 8); w.Ue(0);
    if (high) {
        w.Ue(1); w.Ue(0); w.Ue(0); w.Put(0, 1); w.Put(1, 1);
        for (unsigned i = 0; i < 8; ++i) {
            w.Put(i == 0, 1);
            if (i == 0)
                for (int j = 0; j < 16; ++j) w.Ue(0);
        }
    }
    w.Ue(0); w.Ue(0); w.Ue(0); w.Ue(1); w.Put(0, 1);
    w.Ue(39); w.Ue(23); w.Put(1, 1); w.Put(1, 1); w.Put(0, 1);
    w.Put(vui, 1);
    if (vui) {
        w.Put(extras, 1);
        if (extras) { w.Put(255, 8); w.Put(1, 16); w.Put(1, 16); }
        w.Put(extras, 1); if (extras) w.Put(1, 1);
        w.Put(signal != 0, 1);
        if (signal != 0) {
            w.Put(5, 3); w.Put(signal != 3, 1); w.Put(signal != 1, 1);
            if (signal != 1) {
                for (int i = 0; i < 3; ++i) w.Put(signal == 3 ? 1 : 6, 8);
            }
        }
        w.Put(extras, 1); if (extras) { w.Ue(2); w.Ue(3); }
        w.Put(extras, 1); if (extras) { w.Put(1, 32); w.Put(120, 32); w.Put(1, 1); }
        w.Put(extras, 1); // nal_hrd
        if (extras) {
            w.Ue(0); w.Put(0, 8); w.Ue(300); w.Ue(600); w.Put(1, 1); w.Put(0, 20);
        }
        w.Put(0, 1); // vcl_hrd
        if (extras) w.Put(1, 1); // low_delay_hrd
        w.Put(0, 1); // pic_struct
        w.Put(extras, 1);
        if (extras) { w.Put(1, 1); for (int i = 0; i < 6; ++i) w.Ue(i); }
    }
    return w.Finish();
}

void TestColor()
{
    // Padded row stride verifies that padding is not interpreted as pixels.
    std::array<uint8_t, 32> bgra{};
    auto convert = [&](int r, int g, int b) {
        bgra.fill(0xa5);
        for (size_t offset : {0u, 4u, 16u, 20u}) {
            bgra[offset] = static_cast<uint8_t>(b);
            bgra[offset + 1] = static_cast<uint8_t>(g);
            bgra[offset + 2] = static_cast<uint8_t>(r);
            bgra[offset + 3] = 255;
        }
        std::vector<uint8_t> nv12;
        assert(od::ConvertBgraToBt709Nv12(bgra.data(), 16, 2, 2, nv12));
        assert(nv12.size() == 6 && nv12[0] == nv12[1] && nv12[0] == nv12[2] && nv12[0] == nv12[3]);
        return std::array<int, 3>{nv12[0], nv12[4], nv12[5]};
    };
    assert((convert(0, 0, 0) == std::array<int, 3>{16, 128, 128}));
    assert((convert(255, 255, 255) == std::array<int, 3>{235, 128, 128}));
    assert((convert(255, 0, 0) == std::array<int, 3>{63, 102, 240}));
    assert((convert(0, 255, 0) == std::array<int, 3>{173, 42, 26}));
    assert((convert(0, 0, 255) == std::array<int, 3>{32, 240, 118}));
    for (int gray = 0; gray < 256; ++gray) {
        auto yuv = convert(gray, gray, gray);
        assert(yuv[1] == 128 && yuv[2] == 128);
        assert(std::abs(yuv[0] - (16.0 + gray * 219.0 / 255.0)) <= 0.51);
    }
    for (int r = 0; r <= 255; r += 17)
        for (int g = 0; g <= 255; g += 17)
            for (int b = 0; b <= 255; b += 17) {
                const auto yuv = convert(r, g, b);
                const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                assert(std::abs(yuv[0] - (16 + luma * 219 / 255)) < 0.52);
                assert(std::abs(yuv[1] - (128 + (b - luma) / 1.8556 * 224 / 255)) < 0.52);
                assert(std::abs(yuv[2] - (128 + (r - luma) / 1.5748 * 224 / 255)) < 0.52);
            }
    std::vector<uint8_t> sentinel{42};
    assert(!od::ConvertBgraToBt709Nv12(nullptr, 8, 2, 2, sentinel));
    assert(!od::ConvertBgraToBt709Nv12(bgra.data(), 8, 3, 2, sentinel));
    assert(!od::ConvertBgraToBt709Nv12(bgra.data(), 4, 2, 2, sentinel));
    assert(sentinel == std::vector<uint8_t>{42});
    const std::array<uint8_t, 16> mixed{
        0, 0, 255, 0, 0, 255, 0, 255, 255, 0, 0, 17, 255, 255, 255, 128};
    std::vector<uint8_t> subsampled;
    assert(od::ConvertBgraToBt709Nv12(mixed.data(), 8, 2, 2, subsampled));
    assert((subsampled == std::vector<uint8_t>{63, 173, 32, 235, 128, 128}));
}

void TestSpsColor()
{
    for (bool high : {false, true}) {
        auto absent = TestSps(false, 0, high, false);
        assert(od::InspectSpsColor(absent).has_value());
        assert(!od::InspectSpsColor(absent)->vuiPresent);
        assert(od::EnsureSpsBt709Limited(absent) == od::SpsColorResult::Updated);
        assert(absent.payload == TestSps(true, 3, high, false).payload);
        for (bool extras : {false, true}) {
            for (int signal = 0; signal <= 3; ++signal) {
                auto nal = TestSps(true, signal, high, extras);
                assert(od::InspectSpsColor(nal).has_value());
                const auto result = od::EnsureSpsBt709Limited(nal);
                assert(result == (signal == 3 ? od::SpsColorResult::Unchanged : od::SpsColorResult::Updated));
                // Byte-for-byte checks include cropping/geometry, scaling
                // lists, extended aspect ratio, timing, HRD and restrictions.
                assert(nal.payload == TestSps(true, 3, high, extras).payload);
                assert(od::InspectSpsColor(nal)->IsBt709Limited());
                auto once = nal.payload;
                assert(od::EnsureSpsBt709Limited(nal) == od::SpsColorResult::Unchanged);
                assert(nal.payload == once);
                auto access = od::BuildAccessUnit({nal});
                auto parsed = od::ScanStartCodes(access.data(), access.size());
                assert(parsed.size() == 1 && parsed.front().payload == nal.payload);
            }
        }
    }
    for (const std::vector<uint8_t>& bytes : std::vector<std::vector<uint8_t>>{
             {}, {0x67}, {0x67, 100, 0, 40}, {0x67, 100, 0, 40, 0}, {0x67, 100, 0, 40, 0, 0, 3, 4}}) {
        od::NalUnit invalid{od::kNalTypeSps, bytes};
        assert(!od::InspectSpsColor(invalid));
        assert(od::EnsureSpsBt709Limited(invalid) == od::SpsColorResult::Invalid);
        assert(invalid.payload == bytes);
    }
    od::NalUnit notSps{od::kNalTypePps, {0x68, 0xce, 0x3c, 0x80}};
    assert(!od::InspectSpsColor(notSps));
    od::SpsPpsCache cache;
    auto normalized = TestSps(false, 0, true, false);
    assert(od::EnsureSpsBt709Limited(normalized) == od::SpsColorResult::Updated);
    cache.EnsureParameterSets({normalized, notSps});
    const auto keyFrame = cache.EnsureParameterSets({od::NalUnit{od::kNalTypeIdrSlice, {0x65, 0x88}}});
    assert(keyFrame.size() == 3 && od::InspectSpsColor(keyFrame.front())->IsBt709Limited());
}

} // namespace

int main()
{
    TestColor();
    TestSpsColor();
    std::cout << "BT.709 color conversion and SPS VUI tests passed\n";
}
