#include "quarry/quarry.hpp"

#include <string>
#include <utility>

#include "core/hash.hpp"
#include "core/prng.hpp"

namespace artminer::quarry {
namespace {

constexpr core::u64 kCandidateSeedDomain = 0x514152525943414eULL;  // same AM-010 enumerator domain

[[nodiscard]] QuarryError make_error(const QuarryErrorCode code, std::string message) {
    return QuarryError{code, std::move(message)};
}

[[nodiscard]] std::string mode_text(const CandidateMutationMode mode) {
    return mode == CandidateMutationMode::topology ? "topology" : "parameter";
}

}  // namespace

std::string candidate_generation_identity(
    const JobManifest& manifest,
    const CandidateGenerationSettings& settings) {
    std::string payload = "ArtMiner.QuarryCandidateGeneration.v1\nmanifest=" + manifest.identity;
    payload += "\nmode=" + mode_text(settings.mode);
    if (settings.mode == CandidateMutationMode::topology) {
        payload += "\ntopology-operator=" + std::to_string(settings.topology_operator_version);
        payload += "\ntopology-budget=" + std::to_string(settings.topology_budget);
        payload += "\nstructural-locks=" + settings.structural_locks.serialize_canonical();
    }
    payload += "\n";
    return core::hex_u64(core::fnv1a64(payload));
}

core::Result<core::Recipe, QuarryError> reconstruct_candidate_recipe(
    const JobManifest& manifest,
    const core::u64 candidate_index,
    const CandidateGenerationSettings& settings) {
    if (settings.mode == CandidateMutationMode::parameter) {
        return reconstruct_candidate_recipe(manifest, candidate_index);
    }
    if (candidate_index < manifest.first_candidate ||
        candidate_index >= manifest.first_candidate + manifest.candidate_count) {
        return core::Result<core::Recipe, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_job, "candidate index is outside the manifest range"));
    }
    if (settings.topology_operator_version != core::kTopologyMutationOperatorVersion) {
        return core::Result<core::Recipe, QuarryError>::failure(make_error(
            QuarryErrorCode::unsupported_version, "unsupported topology candidate operator version"));
    }
    if (settings.topology_budget == 0U ||
        settings.topology_budget > core::kMaximumTopologyMutationBudget) {
        return core::Result<core::Recipe, QuarryError>::failure(make_error(
            QuarryErrorCode::resource_limit, "topology candidate budget must be within 1..16"));
    }

    const core::u64 operation_seed = core::derive_seed(
        manifest.root_seed,
        core::splitmix64(kCandidateSeedDomain ^ candidate_index));
    auto mutated = core::mutate_recipe_topology(
        manifest.base_recipe,
        operation_seed,
        settings.topology_operator_version,
        manifest.mutation_strength,
        settings.topology_budget,
        settings.structural_locks);
    if (mutated.is_error()) {
        return core::Result<core::Recipe, QuarryError>::failure(make_error(
            QuarryErrorCode::mutation_failed,
            "Quarry deterministic topology mutation failed: " + mutated.error().message));
    }
    core::Recipe recipe = std::move(mutated).value().recipe;
    recipe.render.width = manifest.render_width;
    recipe.render.height = manifest.render_height;
    return core::Result<core::Recipe, QuarryError>::success(std::move(recipe));
}

}  // namespace artminer::quarry
