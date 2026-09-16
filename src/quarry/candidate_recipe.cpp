#include "quarry/quarry.hpp"

#include <string>

#include "core/prng.hpp"
#include "core/specimen_browser.hpp"

namespace artminer::quarry {
namespace {

constexpr core::u64 kCandidateSeedDomain = 0x514152525943414eULL;

[[nodiscard]] QuarryError make_error(const QuarryErrorCode code, std::string message) {
    return QuarryError{code, std::move(message)};
}

}  // namespace

core::Result<core::Recipe, QuarryError> reconstruct_candidate_recipe(
    const JobManifest& manifest,
    const core::u64 candidate_index) {
    if (candidate_index < manifest.first_candidate ||
        candidate_index >= manifest.first_candidate + manifest.candidate_count) {
        return core::Result<core::Recipe, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_job, "candidate index is outside the manifest range"));
    }
    const core::u64 operation_seed = core::derive_seed(
        manifest.root_seed,
        core::splitmix64(kCandidateSeedDomain ^ candidate_index));
    core::ParameterLocks no_locks;
    auto mutated = core::mutate_recipe_parameters(
        manifest.base_recipe,
        operation_seed,
        manifest.mutation_operator_version,
        manifest.mutation_strength,
        no_locks);
    if (mutated.is_error()) {
        return core::Result<core::Recipe, QuarryError>::failure(make_error(
            QuarryErrorCode::mutation_failed,
            "Quarry deterministic mutation failed: " + mutated.error().message));
    }
    core::Recipe recipe = std::move(mutated).value();
    recipe.render.width = manifest.render_width;
    recipe.render.height = manifest.render_height;
    return core::Result<core::Recipe, QuarryError>::success(std::move(recipe));
}

}  // namespace artminer::quarry
