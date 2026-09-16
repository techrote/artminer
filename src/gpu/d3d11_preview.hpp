#pragma once

#include <Windows.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "core/recipe.hpp"
#include "core/result.hpp"
#include "core/types.hpp"
#include "nodes/static_evaluator.hpp"

namespace artminer::gpu {

enum class PreviewPath {
    gpu,
    canonical_cpu_fallback,
};

struct PreviewError final {
    std::string message;
};

struct PreviewPlan final {
    bool gpu_supported{false};
    std::string pixel_shader_source;
    std::vector<std::string> unsupported_nodes;
};

struct PreviewStatus final {
    PreviewPath path{PreviewPath::canonical_cpu_fallback};
    std::string message;
    std::vector<std::string> unsupported_nodes;
};

struct PreviewDiagnostics final {
    std::size_t presented_frames{0U};
    double last_present_milliseconds{0.0};
    bool warp_device{false};
};

struct EquivalenceStats final {
    core::u8 maximum_channel_error{0U};
    double mean_absolute_channel_error{0.0};
    std::size_t differing_channels{0U};
};

// Build the deterministic shader plan for the subset whose GPU semantics are
// explicitly implemented by AM-004. Unsupported graph operations are listed and
// must use the canonical CPU fallback rather than being silently reinterpreted.
[[nodiscard]] PreviewPlan build_preview_plan(const core::Recipe& recipe);

// Headless WARP path used by deterministic CI equivalence tests. Unsupported
// recipes intentionally return canonical CPU output with a fallback status.
[[nodiscard]] core::Result<nodes::Image, PreviewError> render_preview_warp(
    const core::Recipe& recipe,
    PreviewStatus* status = nullptr);

[[nodiscard]] EquivalenceStats compare_images(
    const nodes::Image& canonical,
    const nodes::Image& preview);

class D3d11Preview final {
public:
    D3d11Preview();
    ~D3d11Preview();

    D3d11Preview(const D3d11Preview&) = delete;
    D3d11Preview& operator=(const D3d11Preview&) = delete;
    D3d11Preview(D3d11Preview&&) noexcept;
    D3d11Preview& operator=(D3d11Preview&&) noexcept;

    [[nodiscard]] core::Result<void, PreviewError> initialize(HWND window);
    [[nodiscard]] core::Result<PreviewStatus, PreviewError> set_recipe(const core::Recipe& recipe);
    [[nodiscard]] core::Result<void, PreviewError> resize(core::u32 width, core::u32 height);
    [[nodiscard]] core::Result<void, PreviewError> present();

    [[nodiscard]] PreviewDiagnostics diagnostics() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace artminer::gpu
