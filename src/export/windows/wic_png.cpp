#include "export/windows/wic_png.hpp"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/checked_math.hpp"

namespace artminer::exporting::windows {
namespace {

using Microsoft::WRL::ComPtr;

[[nodiscard]] ExportError hresult_error(const char* operation, const HRESULT result) {
    std::ostringstream stream;
    stream << operation << " failed with HRESULT 0x" << std::hex << std::uppercase
           << static_cast<unsigned long>(result);
    return ExportError{stream.str()};
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

[[nodiscard]] core::Result<void, ExportError> write_sidecar(
    const std::filesystem::path& path,
    const nodes::Image& image,
    const core::Recipe& recipe,
    const std::optional<core::u64> simulation_tick) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Result<void, ExportError>::failure(ExportError{
            "could not create provenance sidecar: " + path.string()});
    }

    output << "ArtMiner-Provenance 1\n"
           << "fingerprint " << core::semantic_fingerprint(recipe) << '\n'
           << "schema " << recipe.schema_version << '\n'
           << "evaluator " << recipe.evaluator_version << '\n'
           << "width " << image.width << '\n'
           << "height " << image.height << '\n';
    if (simulation_tick.has_value()) {
        output << "simulation-tick " << *simulation_tick << '\n';
    }
    output << "pixel-format RGBA8-straight-sRGB-encoding\n"
           << "recipe-begin\n"
           << core::serialize_recipe_canonical(recipe)
           << "recipe-end\n";
    output.flush();
    if (!output) {
        return core::Result<void, ExportError>::failure(ExportError{
            "failed while writing provenance sidecar: " + path.string()});
    }
    return core::Result<void, ExportError>::success();
}

}  // namespace

std::filesystem::path provenance_sidecar_path(const std::filesystem::path& image_path) {
    std::filesystem::path sidecar = image_path;
    sidecar += L".artminer.txt";
    return sidecar;
}

core::Result<void, ExportError> write_png_with_provenance(
    const std::filesystem::path& image_path,
    const nodes::Image& image,
    const core::Recipe& recipe,
    const std::optional<core::u64> simulation_tick) {
    auto byte_count = core::checked_image_byte_count(image.width, image.height, 4U);
    if (byte_count.is_error() || byte_count.value() != static_cast<core::u64>(image.rgba.size())) {
        return core::Result<void, ExportError>::failure(ExportError{
            "image byte layout does not match width*height*4 RGBA8"});
    }
    if (image.rgba.size() > static_cast<std::size_t>((std::numeric_limits<UINT>::max)())) {
        return core::Result<void, ExportError>::failure(ExportError{
            "image buffer exceeds the WIC WritePixels UINT byte-count limit"});
    }
    const core::u64 stride_u64 = static_cast<core::u64>(image.width) * 4ULL;
    if (stride_u64 > static_cast<core::u64>((std::numeric_limits<UINT>::max)())) {
        return core::Result<void, ExportError>::failure(ExportError{
            "image stride exceeds the WIC UINT stride limit"});
    }

    ComApartment apartment;
    if (!apartment.usable()) {
        return core::Result<void, ExportError>::failure(hresult_error("CoInitializeEx", apartment.result()));
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result)) {
        return core::Result<void, ExportError>::failure(hresult_error("CoCreateInstance(WICImagingFactory)", result));
    }

    ComPtr<IWICStream> stream;
    result = factory->CreateStream(stream.GetAddressOf());
    if (FAILED(result)) {
        return core::Result<void, ExportError>::failure(hresult_error("IWICImagingFactory::CreateStream", result));
    }
    result = stream->InitializeFromFilename(image_path.c_str(), GENERIC_WRITE);
    if (FAILED(result)) {
        return core::Result<void, ExportError>::failure(hresult_error("IWICStream::InitializeFromFilename", result));
    }

    ComPtr<IWICBitmapEncoder> encoder;
    result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
    if (FAILED(result)) {
        return core::Result<void, ExportError>::failure(hresult_error("IWICImagingFactory::CreateEncoder(PNG)", result));
    }
    result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(result)) {
        return core::Result<void, ExportError>::failure(hresult_error("IWICBitmapEncoder::Initialize", result));
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    result = encoder->CreateNewFrame(frame.GetAddressOf(), nullptr);
    if (FAILED(result)) {
        return core::Result<void, ExportError>::failure(hresult_error("IWICBitmapEncoder::CreateNewFrame", result));
    }
    result = frame->Initialize(nullptr);
    if (FAILED(result)) {
        return core::Result<void, ExportError>::failure(hresult_error("IWICBitmapFrameEncode::Initialize", result));
    }
    result = frame->SetSize(image.width, image.height);
    if (FAILED(result)) {
        return core::Result<void, ExportError>::failure(hresult_error("IWICBitmapFrameEncode::SetSize", result));
    }

    // The Windows PNG encoder's native 32-bit format is BGRA. Keep the
    // canonical evaluator storage RGBA and perform an export-only channel swap.
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
    result = frame->SetPixelFormat(&pixel_format);
    if (FAILED(result)) {
        return core::Result<void, ExportError>::failure(hresult_error("IWICBitmapFrameEncode::SetPixelFormat", result));
    }
    if (!IsEqualGUID(pixel_format, GUID_WICPixelFormat32bppBGRA)) {
        return core::Result<void, ExportError>::failure(ExportError{
            "WIC PNG encoder did not accept required 32bpp BGRA transport format"});
    }

    std::vector<BYTE> bgra(image.rgba.size());
    for (std::size_t index = 0U; index < image.rgba.size(); index += 4U) {
        bgra[index] = image.rgba[index + 2U];
        bgra[index + 1U] = image.rgba[index + 1U];
        bgra[index + 2U] = image.rgba[index];
        bgra[index + 3U] = image.rgba[index + 3U];
    }

    result = frame->WritePixels(
        image.height,
        static_cast<UINT>(stride_u64),
        static_cast<UINT>(bgra.size()),
        bgra.data());
    if (FAILED(result)) {
        return core::Result<void, ExportError>::failure(hresult_error("IWICBitmapFrameEncode::WritePixels", result));
    }
    result = frame->Commit();
    if (FAILED(result)) {
        return core::Result<void, ExportError>::failure(hresult_error("IWICBitmapFrameEncode::Commit", result));
    }
    result = encoder->Commit();
    if (FAILED(result)) {
        return core::Result<void, ExportError>::failure(hresult_error("IWICBitmapEncoder::Commit", result));
    }

    const std::filesystem::path sidecar = provenance_sidecar_path(image_path);
    auto sidecar_result = write_sidecar(sidecar, image, recipe, simulation_tick);
    if (sidecar_result.is_error()) {
        std::error_code remove_error;
        std::filesystem::remove(image_path, remove_error);
        return sidecar_result;
    }
    return core::Result<void, ExportError>::success();
}

}  // namespace artminer::exporting::windows
