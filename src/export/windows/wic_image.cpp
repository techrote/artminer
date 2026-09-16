#include "export/windows/wic_image.hpp"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

#include "core/checked_math.hpp"

namespace artminer::exporting::windows {
namespace {

using Microsoft::WRL::ComPtr;

[[nodiscard]] WicImageError hresult_error(const char* operation, const HRESULT result) {
    std::ostringstream stream;
    stream << operation << " failed with HRESULT 0x" << std::hex << std::uppercase
           << static_cast<unsigned long>(result);
    return WicImageError{stream.str()};
}

class ComApartment final {
public:
    ComApartment() noexcept {
        result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owns_uninitialize_ = result_ == S_OK || result_ == S_FALSE;
    }

    ~ComApartment() {
        if (owns_uninitialize_) {
            CoUninitialize();
        }
    }

    [[nodiscard]] HRESULT result() const noexcept { return result_; }
    [[nodiscard]] bool usable() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_{E_FAIL};
    bool owns_uninitialize_{false};
};

[[nodiscard]] core::Result<void, WicImageError> validate_image(const nodes::Image& image) {
    auto byte_count = core::checked_image_byte_count(image.width, image.height, 4U);
    if (byte_count.is_error() || byte_count.value() != static_cast<core::u64>(image.rgba.size())) {
        return core::Result<void, WicImageError>::failure(WicImageError{
            "image byte layout does not match width*height*4 RGBA8"});
    }
    return core::Result<void, WicImageError>::success();
}

}  // namespace

core::Result<void, WicImageError> write_wic_image(
    const std::filesystem::path& path,
    const nodes::Image& image,
    const WicImageFormat format) {
    auto valid = validate_image(image);
    if (valid.is_error()) {
        return valid;
    }

    const bool bmp = format == WicImageFormat::bmp;
    if (bmp) {
        const bool has_transparency = std::any_of(
            image.rgba.begin() + 3,
            image.rgba.end(),
            [index = std::size_t{0U}](const core::u8) mutable {
                ++index;
                return false;
            });
        (void)has_transparency;
        for (std::size_t index = 3U; index < image.rgba.size(); index += 4U) {
            if (image.rgba[index] != 255U) {
                return core::Result<void, WicImageError>::failure(WicImageError{
                    "BMP export rejects non-opaque source pixels because the deterministic BMP contract is 24-bit BGR"});
            }
        }
    }

    const core::u64 bytes_per_pixel = bmp ? 3ULL : 4ULL;
    const core::u64 stride_u64 = static_cast<core::u64>(image.width) * bytes_per_pixel;
    auto transport_bytes = core::checked_image_byte_count(
        image.width,
        image.height,
        static_cast<core::u32>(bytes_per_pixel));
    if (transport_bytes.is_error() ||
        stride_u64 > static_cast<core::u64>((std::numeric_limits<UINT>::max)()) ||
        transport_bytes.value() > static_cast<core::u64>((std::numeric_limits<UINT>::max)())) {
        return core::Result<void, WicImageError>::failure(WicImageError{
            "image exceeds WIC UINT stride or byte-count limits"});
    }

    ComApartment apartment;
    if (!apartment.usable()) {
        return core::Result<void, WicImageError>::failure(hresult_error("CoInitializeEx", apartment.result()));
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result)) {
        return core::Result<void, WicImageError>::failure(
            hresult_error("CoCreateInstance(WICImagingFactory)", result));
    }

    ComPtr<IWICStream> stream;
    result = factory->CreateStream(stream.GetAddressOf());
    if (FAILED(result)) {
        return core::Result<void, WicImageError>::failure(
            hresult_error("IWICImagingFactory::CreateStream", result));
    }
    result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(result)) {
        return core::Result<void, WicImageError>::failure(
            hresult_error("IWICStream::InitializeFromFilename", result));
    }

    const GUID& container = bmp ? GUID_ContainerFormatBmp : GUID_ContainerFormatPng;
    ComPtr<IWICBitmapEncoder> encoder;
    result = factory->CreateEncoder(container, nullptr, encoder.GetAddressOf());
    if (FAILED(result)) {
        return core::Result<void, WicImageError>::failure(
            hresult_error("IWICImagingFactory::CreateEncoder", result));
    }
    result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(result)) {
        return core::Result<void, WicImageError>::failure(
            hresult_error("IWICBitmapEncoder::Initialize", result));
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    result = encoder->CreateNewFrame(frame.GetAddressOf(), nullptr);
    if (FAILED(result)) {
        return core::Result<void, WicImageError>::failure(
            hresult_error("IWICBitmapEncoder::CreateNewFrame", result));
    }
    result = frame->Initialize(nullptr);
    if (FAILED(result)) {
        return core::Result<void, WicImageError>::failure(
            hresult_error("IWICBitmapFrameEncode::Initialize", result));
    }
    result = frame->SetSize(image.width, image.height);
    if (FAILED(result)) {
        return core::Result<void, WicImageError>::failure(
            hresult_error("IWICBitmapFrameEncode::SetSize", result));
    }

    WICPixelFormatGUID pixel_format = bmp ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    const WICPixelFormatGUID required_format = pixel_format;
    result = frame->SetPixelFormat(&pixel_format);
    if (FAILED(result)) {
        return core::Result<void, WicImageError>::failure(
            hresult_error("IWICBitmapFrameEncode::SetPixelFormat", result));
    }
    if (!IsEqualGUID(pixel_format, required_format)) {
        return core::Result<void, WicImageError>::failure(WicImageError{
            bmp
                ? "WIC BMP encoder did not accept required deterministic 24bpp BGR transport format"
                : "WIC PNG encoder did not accept required deterministic 32bpp BGRA transport format"});
    }

    std::vector<BYTE> transport(static_cast<std::size_t>(transport_bytes.value()));
    if (bmp) {
        std::size_t output = 0U;
        for (std::size_t input = 0U; input < image.rgba.size(); input += 4U) {
            transport[output++] = image.rgba[input + 2U];
            transport[output++] = image.rgba[input + 1U];
            transport[output++] = image.rgba[input];
        }
    } else {
        for (std::size_t index = 0U; index < image.rgba.size(); index += 4U) {
            transport[index] = image.rgba[index + 2U];
            transport[index + 1U] = image.rgba[index + 1U];
            transport[index + 2U] = image.rgba[index];
            transport[index + 3U] = image.rgba[index + 3U];
        }
    }

    result = frame->WritePixels(
        image.height,
        static_cast<UINT>(stride_u64),
        static_cast<UINT>(transport.size()),
        transport.data());
    if (FAILED(result)) {
        return core::Result<void, WicImageError>::failure(
            hresult_error("IWICBitmapFrameEncode::WritePixels", result));
    }
    result = frame->Commit();
    if (FAILED(result)) {
        return core::Result<void, WicImageError>::failure(
            hresult_error("IWICBitmapFrameEncode::Commit", result));
    }
    result = encoder->Commit();
    if (FAILED(result)) {
        return core::Result<void, WicImageError>::failure(
            hresult_error("IWICBitmapEncoder::Commit", result));
    }
    return core::Result<void, WicImageError>::success();
}

}  // namespace artminer::exporting::windows
