#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "core/recipe.hpp"
#include "core/result.hpp"
#include "core/types.hpp"
#include "export/export.hpp"

namespace artminer::exporting {

enum class GlyphTextFormat {
    utf8,
    ansi,
};

enum class GlyphExportKind {
    single,
    sequence,
};

struct GlyphExportRequest final {
    GlyphExportKind kind{GlyphExportKind::single};
    GlyphTextFormat format{GlyphTextFormat::utf8};
    std::filesystem::path destination_directory;
    std::string stem{"glyph"};
    std::string settings_node_id;
    std::optional<core::u64> tick;
    core::u64 start_tick{0U};
    core::u64 end_tick{0U};
};

[[nodiscard]] std::string_view to_string(GlyphTextFormat format) noexcept;
[[nodiscard]] std::string deterministic_glyph_frame_filename(core::u64 tick, GlyphTextFormat format);

// AM-012 text/terminal exporter. It deliberately reuses AM-009's ExportResult,
// checksumming/version contract and same-parent staging path. Canonical payloads
// are UTF-8 cell sequences (optionally with ANSI SGR bytes), not font pixels.
[[nodiscard]] core::Result<ExportResult, ExportError> export_glyph_recipe(
    const core::Recipe& recipe,
    const GlyphExportRequest& request);

}  // namespace artminer::exporting
