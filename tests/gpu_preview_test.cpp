#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#include "core/recipe.hpp"
#include "gpu/d3d11_preview.hpp"
#include "nodes/static_evaluator.hpp"

#ifndef ARTMINER_SOURCE_DIR
#define ARTMINER_SOURCE_DIR "."
#endif

namespace {

int g_failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        ++g_failures;
        std::cerr << "FAIL: could not open " << path.string() << '\n';
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

[[nodiscard]] artminer::core::Recipe load_recipe(const std::filesystem::path& path) {
    const std::string text = read_file(path);
    auto parsed = artminer::core::parse_recipe(text);
    if (parsed.is_error()) {
        ++g_failures;
        std::cerr << "FAIL: could not parse " << path.string() << ": " << parsed.error().message << '\n';
        return {};
    }
    auto recipe = std::move(parsed).value();
    const auto errors = artminer::core::validate_recipe(recipe);
    if (!errors.empty()) {
        ++g_failures;
        std::cerr << "FAIL: invalid recipe " << path.string() << ": " << errors.front().message << '\n';
    }
    return recipe;
}

void test_sdf_gpu_equivalence() {
    const std::filesystem::path root(ARTMINER_SOURCE_DIR);
    const auto path = root / "examples" / "am003-sdf.amr";
    auto recipe = load_recipe(path);
    const std::string fingerprint = artminer::core::semantic_fingerprint(recipe);

    const auto plan = artminer::gpu::build_preview_plan(recipe);
    expect(plan.gpu_supported, "SDF example is supported by the AM-004 pointwise GPU preview subset");
    expect(plan.unsupported_nodes.empty(), "supported SDF preview has no hidden unsupported nodes");
    expect(plan.pixel_shader_source.find("PSMain") != std::string::npos, "supported plan emits a reproducible pixel shader");

    auto canonical = artminer::nodes::render_reference(recipe);
    expect(canonical.is_ok(), "SDF canonical reference renders");
    artminer::gpu::PreviewStatus status;
    auto preview = artminer::gpu::render_preview_warp(recipe, &status);
    expect(preview.is_ok(), "SDF D3D11 WARP preview renders");
    if (canonical.is_ok() && preview.is_ok()) {
        expect(status.path == artminer::gpu::PreviewPath::gpu, "SDF WARP test exercises the GPU path rather than fallback");
        const auto stats = artminer::gpu::compare_images(canonical.value(), preview.value());
        expect(stats.maximum_channel_error <= 3U, "SDF GPU preview stays within the documented 3/255 channel tolerance");
        expect(stats.mean_absolute_channel_error <= 0.50, "SDF GPU preview mean absolute channel error stays within tolerance");
    }
    expect(artminer::core::semantic_fingerprint(recipe) == fingerprint, "GPU planning/rendering does not mutate recipe semantics");
}

void test_unsupported_graph_falls_back_canonically() {
    const std::filesystem::path root(ARTMINER_SOURCE_DIR);
    const auto path = root / "examples" / "am003-fbm-warp.amr";
    auto recipe = load_recipe(path);
    const std::string fingerprint = artminer::core::semantic_fingerprint(recipe);

    const auto plan = artminer::gpu::build_preview_plan(recipe);
    expect(!plan.gpu_supported, "fBm/warp example is explicitly outside the AM-004 GPU subset");
    expect(!plan.unsupported_nodes.empty(), "unsupported GPU graph reports capability reasons");

    auto canonical = artminer::nodes::render_reference(recipe);
    artminer::gpu::PreviewStatus status;
    auto preview = artminer::gpu::render_preview_warp(recipe, &status);
    expect(canonical.is_ok() && preview.is_ok(), "unsupported graph still previews through canonical CPU fallback");
    if (canonical.is_ok() && preview.is_ok()) {
        expect(status.path == artminer::gpu::PreviewPath::canonical_cpu_fallback, "fallback path is surfaced explicitly");
        expect(canonical.value().rgba == preview.value().rgba, "CPU fallback is byte-identical to canonical reference output");
    }
    expect(artminer::core::semantic_fingerprint(recipe) == fingerprint, "fallback preview does not mutate recipe semantics");
}

void test_all_static_examples_have_honest_preview_path() {
    const std::filesystem::path root(ARTMINER_SOURCE_DIR);
    for (const char* filename : {
             "am003-fbm-warp.amr",
             "am003-worley.amr",
             "am003-sdf.amr",
             "am003-angular-repeat.amr"}) {
        auto recipe = load_recipe(root / "examples" / filename);
        artminer::gpu::PreviewStatus status;
        auto preview = artminer::gpu::render_preview_warp(recipe, &status);
        expect(preview.is_ok(), "every committed AM-003 example has either GPU preview or canonical CPU fallback");
        if (preview.is_ok()) {
            expect(
                status.path == artminer::gpu::PreviewPath::gpu ||
                    status.path == artminer::gpu::PreviewPath::canonical_cpu_fallback,
                "example preview path is explicit");
            expect(!status.message.empty(), "example preview path includes user-visible status text");
        }
    }
}

}  // namespace

int main() {
    test_sdf_gpu_equivalence();
    test_unsupported_graph_falls_back_canonically();
    test_all_static_examples_have_honest_preview_path();

    if (g_failures != 0) {
        std::cerr << g_failures << " AM-004 GPU preview test assertion(s) failed.\n";
        return 1;
    }
    std::cout << "ArtMiner AM-004 D3D11 preview tests passed.\n";
    return 0;
}
