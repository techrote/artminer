#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "core/recipe.hpp"
#include "core/result.hpp"
#include "nodes/static_evaluator.hpp"

namespace artminer::exporting::windows {

struct ExportError final {
    std::string message;
};

// Writes RGBA8 through the Windows Imaging Component and a deterministic
// <image>.artminer.txt sidecar containing the complete canonical recipe and
// semantic fingerprint. Stateful renders also record their explicit simulation
// tick; the optional default preserves the accepted AM-003 stateless contract.
[[nodiscard]] core::Result<void, ExportError> write_png_with_provenance(
    const std::filesystem::path& image_path,
    const nodes::Image& image,
    const core::Recipe& recipe,
    std::optional<core::u64> simulation_tick = std::nullopt);

[[nodiscard]] std::filesystem::path provenance_sidecar_path(const std::filesystem::path& image_path);

}  // namespace artminer::exporting::windows
