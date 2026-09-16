#pragma once

#include <filesystem>
#include <string>

#include "core/result.hpp"
#include "nodes/static_evaluator.hpp"

namespace artminer::exporting::windows {

enum class WicImageFormat {
    png,
    bmp,
};

struct WicImageError final {
    std::string message;
};

// Low-level Windows Imaging Component encoder used by the AM-009 export
// subsystem. This function only writes image bytes; provenance/transaction
// semantics are owned by the higher-level exporter.
[[nodiscard]] core::Result<void, WicImageError> write_wic_image(
    const std::filesystem::path& path,
    const nodes::Image& image,
    WicImageFormat format);

}  // namespace artminer::exporting::windows
