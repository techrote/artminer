#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "core/graph.hpp"
#include "core/local_text.hpp"
#include "core/recipe.hpp"
#include "core/specimen_browser.hpp"
#include "export/export.hpp"
#include "nodes/motion_evaluator.hpp"
#include "nodes/static_evaluator.hpp"
#include "quarry/quarry.hpp"

#ifndef ARTMINER_SOURCE_DIR
#define ARTMINER_SOURCE_DIR "."
#endif

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] bool load_recipe(const std::filesystem::path& path, artminer::core::Recipe& recipe) {
    auto text = artminer::core::read_local_text_file(path);
    if (text.is_error()) {
        std::cerr << "benchmark input error: " << path.string() << '\n';
        return false;
    }
    auto parsed = artminer::core::parse_recipe(text.value());
    if (parsed.is_error()) {
        std::cerr << "benchmark parse error: " << parsed.error().message << '\n';
        return false;
    }
    recipe = std::move(parsed).value();
    const auto validation = artminer::core::validate_recipe(recipe);
    if (!validation.empty()) {
        std::cerr << "benchmark validation error: " << validation.front().message << '\n';
        return false;
    }
    return true;
}

template <typename Function>
[[nodiscard]] double timed_ms(Function&& function) {
    const auto start = Clock::now();
    function();
    const auto end = Clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

}  // namespace

int main() {
    const std::filesystem::path source = ARTMINER_SOURCE_DIR;
    artminer::core::Recipe static_recipe;
    artminer::core::Recipe growth_recipe;
    artminer::core::Recipe particle_recipe;
    if (!load_recipe(source / L"examples" / L"am003-fbm-warp.amr", static_recipe) ||
        !load_recipe(source / L"examples" / L"am006-reaction-diffusion.amr", growth_recipe) ||
        !load_recipe(source / L"examples" / L"am007-particle-flow.amr", particle_recipe)) {
        return 1;
    }

    bool failed = false;
    const double preview_ms = timed_ms([&]() {
        auto rendered = artminer::nodes::render_reference(static_recipe);
        if (rendered.is_error()) {
            std::cerr << "preview benchmark failed: " << rendered.error().message << '\n';
            failed = true;
        }
    });

    double grid_ms = 0.0;
    grid_ms = timed_ms([&]() {
        artminer::core::ParameterLocks locks;
        auto grid = artminer::core::generate_specimen_grid(
            static_recipe,
            0x1500ULL,
            artminer::core::kParameterMutationOperatorVersion,
            0.25,
            artminer::core::SpecimenGenerationMode::parameter_mutation,
            locks);
        if (grid.is_error()) {
            std::cerr << "mutation-grid benchmark failed: " << grid.error().message << '\n';
            failed = true;
            return;
        }
        for (auto& specimen : grid.value()) {
            specimen.recipe.render.width = 96U;
            specimen.recipe.render.height = 96U;
            auto rendered = artminer::nodes::render_reference(specimen.recipe);
            if (rendered.is_error()) {
                std::cerr << "mutation-grid thumbnail benchmark failed: " << rendered.error().message << '\n';
                failed = true;
                return;
            }
        }
    });

    const double growth_ms = timed_ms([&]() {
        auto rendered = artminer::nodes::render_reference(growth_recipe);
        if (rendered.is_error()) {
            std::cerr << "growth benchmark failed: " << rendered.error().message << '\n';
            failed = true;
        }
    });

    const double particle_ms = timed_ms([&]() {
        artminer::nodes::FrameSnapshotCache cache(8U);
        auto rendered = artminer::nodes::render_animation_reference(particle_recipe, 12U, "main", &cache);
        if (rendered.is_error()) {
            std::cerr << "particle benchmark failed: " << rendered.error().message << '\n';
            failed = true;
        }
    });

    const auto scratch = std::filesystem::temp_directory_path() / L"artminer-am015-benchmark";
    std::error_code ignored;
    std::filesystem::remove_all(scratch, ignored);
    const double export_ms = timed_ms([&]() {
        artminer::exporting::ExportRequest request;
        request.kind = artminer::exporting::ExportKind::still;
        request.raster_format = artminer::exporting::RasterFormat::png;
        request.destination_directory = scratch / L"export";
        request.stem = "benchmark";
        auto exported = artminer::exporting::export_recipe(static_recipe, request);
        if (exported.is_error()) {
            std::cerr << "export benchmark failed: " << exported.error().message << '\n';
            failed = true;
        }
    });

    const double quarry_ms = timed_ms([&]() {
        auto manifest = artminer::quarry::make_job_manifest(static_recipe, 0x1501ULL, 8U, 96U, 96U);
        if (manifest.is_error()) {
            std::cerr << "Quarry benchmark manifest failed: " << manifest.error().message << '\n';
            failed = true;
            return;
        }
        const auto cache = scratch / L"quarry-cache";
        for (artminer::core::u64 index = 0U; index < 8U; ++index) {
            auto evaluated = artminer::quarry::evaluate_candidate(manifest.value(), index, cache);
            if (evaluated.is_error()) {
                std::cerr << "Quarry benchmark failed: " << evaluated.error().message << '\n';
                failed = true;
                return;
            }
        }
    });
    std::filesystem::remove_all(scratch, ignored);

    std::cout << "ARTMINER_BENCHMARK_V1\n"
              << "preview_reference_ms=" << preview_ms << '\n'
              << "mutation_grid_16x96_ms=" << grid_ms << '\n'
              << "growth_reference_ms=" << growth_ms << '\n'
              << "particle_tick12_ms=" << particle_ms << '\n'
              << "export_png_ms=" << export_ms << '\n'
              << "quarry_8x96_ms=" << quarry_ms << '\n';
    return failed ? 1 : 0;
}
