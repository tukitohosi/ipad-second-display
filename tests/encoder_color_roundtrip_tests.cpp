// Encodes generated color bars and decodes the actual Annex-B output locally.
// No screen capture, virtual display, network connection or configuration writes.
#include "encode/AnnexB.h"
#include "encode/H264Encoder.h"
#include "encode/VideoColor.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 384;
constexpr std::array<std::array<uint8_t, 3>, 8> kColors{{
    {0, 0, 0}, {255, 255, 255}, {255, 0, 0}, {0, 255, 0},
    {0, 0, 255}, {255, 255, 0}, {0, 255, 255}, {255, 0, 255}}};

void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void Check(HRESULT hr, const char* message)
{
    if (FAILED(hr)) {
        std::fprintf(stderr, "%s: 0x%08lx\n", message, hr);
        throw std::runtime_error(message);
    }
}

std::vector<uint8_t> ColorBars()
{
    std::vector<uint8_t> bgra(static_cast<size_t>(kWidth) * kHeight * 4);
    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < kWidth; ++x) {
            const auto& color = kColors[x / (kWidth / kColors.size())];
            uint8_t* pixel = bgra.data() + (static_cast<size_t>(y) * kWidth + x) * 4;
            pixel[0] = color[2]; pixel[1] = color[1]; pixel[2] = color[0]; pixel[3] = 255;
        }
    std::vector<uint8_t> nv12;
    Require(od::ConvertBgraToBt709Nv12(bgra.data(), kWidth * 4, kWidth, kHeight, nv12), "color conversion failed");
    return nv12;
}

class LocalDecoder {
public:
    void Open()
    {
        MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_H264};
        MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_NV12};
        IMFActivate** activations = nullptr;
        UINT32 count = 0;
        Check(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                        &input, &output, &activations, &count), "enumerate decoder");
        Require(count != 0, "no local synchronous H.264 decoder");
        const HRESULT activated = activations[0]->ActivateObject(IID_PPV_ARGS(transform_.ReleaseAndGetAddressOf()));
        for (UINT32 i = 0; i < count; ++i) activations[i]->Release();
        CoTaskMemFree(activations);
        Check(activated, "activate decoder");

        ComPtr<IMFMediaType> type;
        Check(MFCreateMediaType(&type), "create decoder input type");
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, kWidth, kHeight);
        MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, 60, 1);
        Check(transform_->SetInputType(0, type.Get(), 0), "set decoder input type");
        SelectOutput();
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }

    void Push(const od::EncodedFrame& frame)
    {
        ComPtr<IMFMediaBuffer> buffer;
        Check(MFCreateMemoryBuffer(static_cast<DWORD>(frame.annexB.size()), &buffer), "decoder buffer");
        BYTE* bytes = nullptr;
        Check(buffer->Lock(&bytes, nullptr, nullptr), "lock decoder input");
        std::memcpy(bytes, frame.annexB.data(), frame.annexB.size());
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(frame.annexB.size()));
        ComPtr<IMFSample> sample;
        Check(MFCreateSample(&sample), "decoder sample");
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(timestamp_);
        sample->SetSampleDuration(10'000'000 / 60);
        timestamp_ += 10'000'000 / 60;
        HRESULT hr = transform_->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            Drain();
            hr = transform_->ProcessInput(0, sample.Get(), 0);
        }
        Check(hr, "decode input");
        Drain();
    }

    uint32_t Frames() const { return frames_; }
    double MaxRgbError() const { return maxRgbError_; }

private:
    void SelectOutput()
    {
        for (DWORD index = 0;; ++index) {
            ComPtr<IMFMediaType> type;
            const HRESULT hr = transform_->GetOutputAvailableType(0, index, &type);
            if (hr == MF_E_NO_MORE_TYPES) break;
            Check(hr, "enumerate decoder output type");
            GUID subtype{};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype != MFVideoFormat_NV12) continue;
            Check(transform_->SetOutputType(0, type.Get(), 0), "set NV12 decoder output");
            UINT32 width = 0, height = 0;
            Check(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height), "decoded dimensions");
            Require(width == kWidth && height == kHeight, "decoder changed picture dimensions");
            stride_ = static_cast<LONG>(MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, width));
            Require(stride_ >= static_cast<LONG>(width), "unexpected decoder output stride");
            return;
        }
        throw std::runtime_error("decoder has no NV12 output");
    }

    void Drain()
    {
        for (int attempts = 0; attempts < 32; ++attempts) {
            MFT_OUTPUT_STREAM_INFO info{};
            Check(transform_->GetOutputStreamInfo(0, &info), "decoder output stream info");
            ComPtr<IMFSample> sample;
            MFT_OUTPUT_DATA_BUFFER output{};
            const bool provides = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
            if (!provides) {
                Check(MFCreateSample(&sample), "decoder output sample");
                ComPtr<IMFMediaBuffer> buffer;
                Check(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, kWidth * kHeight * 3 / 2), &buffer), "decoder output buffer");
                sample->AddBuffer(buffer.Get());
                output.pSample = sample.Get();
            }
            DWORD status = 0;
            const HRESULT hr = transform_->ProcessOutput(0, 1, &output, &status);
            if (output.pEvents) output.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { SelectOutput(); continue; }
            Check(hr, "decoder output");
            if (provides) sample.Attach(output.pSample);
            CheckPicture(sample.Get());
        }
        throw std::runtime_error("decoder did not settle within output bound");
    }

    void CheckPicture(IMFSample* sample)
    {
        Require(sample != nullptr, "decoder returned no sample");
        ComPtr<IMFMediaBuffer> buffer;
        Check(sample->ConvertToContiguousBuffer(&buffer), "contiguous decoded picture");
        BYTE* data = nullptr;
        DWORD length = 0;
        Check(buffer->Lock(&data, nullptr, &length), "lock decoded picture");
        const size_t stride = static_cast<size_t>(stride_);
        if (length < stride * kHeight * 3 / 2) {
            buffer->Unlock();
            throw std::runtime_error("truncated decoded NV12 picture");
        }
        double maximum = 0;
        // Stay well inside each constant patch: 4:2:0 edge chroma filtering
        // is deliberately not misreported as a matrix/color-range error.
        for (size_t bar = 0; bar < kColors.size(); ++bar) {
            double r = 0, g = 0, b = 0;
            for (uint32_t y = 160; y < 176; ++y)
                for (uint32_t x = static_cast<uint32_t>(bar) * 80 + 32; x < bar * 80 + 48; ++x) {
                    const double yy = (data[static_cast<size_t>(y) * stride + x] - 16) * 255.0 / 219.0;
                    const size_t uv = stride * kHeight + static_cast<size_t>(y / 2) * stride + (x & ~1u);
                    const double cb = (data[uv] - 128) * 255.0 / 224.0;
                    const double cr = (data[uv + 1] - 128) * 255.0 / 224.0;
                    r += std::clamp(yy + 1.5748 * cr, 0.0, 255.0);
                    g += std::clamp(yy - 0.187324 * cb - 0.468124 * cr, 0.0, 255.0);
                    b += std::clamp(yy + 1.8556 * cb, 0.0, 255.0);
                }
            maximum = std::max({maximum, std::abs(r / 256 - kColors[bar][0]),
                                std::abs(g / 256 - kColors[bar][1]), std::abs(b / 256 - kColors[bar][2])});
        }
        buffer->Unlock();
        ++frames_;
        maxRgbError_ = std::max(maxRgbError_, maximum);
        Require(maximum <= 8.0, "decoded color bars exceed 8/255 RGB patch tolerance");
    }

    ComPtr<IMFTransform> transform_;
    LONGLONG timestamp_ = 0;
    LONG stride_ = kWidth;
    uint32_t frames_ = 0;
    double maxRgbError_ = 0;
};

void Run(od::EncoderPreference preference)
{
    std::printf("roundtrip start: %s\n", preference == od::EncoderPreference::SoftwareOnly ? "software" : "prefer hardware");
    od::H264Encoder encoder;
    Require(encoder.Configure(kWidth, kHeight, 60, 8'000'000, preference), "encoder configuration failed");
    std::printf("encoder configured: %ls\n", encoder.Diagnostics().name.c_str());
    LocalDecoder decoder;
    decoder.Open();
    std::printf("local decoder configured\n");
    const auto bars = ColorBars();
    uint64_t sps = 0;
    for (int frame = 0; frame < 120 && decoder.Frames() < 3; ++frame) {
        for (const auto& output : encoder.EncodeNv12(bars.data(), bars.size())) {
            for (const auto& nal : od::ScanStartCodes(output.annexB.data(), output.annexB.size())) {
                if (nal.type == od::kNalTypeSps) {
                    const auto color = od::InspectSpsColor(nal);
                    Require(color && color->IsBt709Limited(), "actual output SPS does not declare BT.709 limited");
                    ++sps;
                }
            }
            decoder.Push(output);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto diagnostics = encoder.Diagnostics();
    Require(decoder.Frames() >= 3, "fewer than three frames decoded");
    Require(sps != 0 && diagnostics.colorSignalingVerified && diagnostics.spsRejected == 0, "SPS validation did not pass");
    Require(diagnostics.inputFrames >= diagnostics.outputFrames && diagnostics.outputFrames >= 3, "invalid output accounting");
    std::printf("encoder=%ls hardware=%d async=%d input=%llu output=%llu SPS=%llu rewritten=%llu decoded=%u maxRgbError=%.3f/255\n",
                diagnostics.name.c_str(), diagnostics.hardware, diagnostics.asynchronous,
                static_cast<unsigned long long>(diagnostics.inputFrames), static_cast<unsigned long long>(diagnostics.outputFrames),
                static_cast<unsigned long long>(diagnostics.spsChecked), static_cast<unsigned long long>(diagnostics.spsRewritten),
                decoder.Frames(), decoder.MaxRgbError());
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    try {
        Run(od::EncoderPreference::PreferHardware);
        Run(od::EncoderPreference::SoftwareOnly);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "encoder color roundtrip failed: %s\n", error.what());
        return 1;
    }
}
