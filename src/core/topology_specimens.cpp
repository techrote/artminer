#include "core/specimen_browser.hpp"

#include <utility>

#include "core/prng.hpp"
#include "core/topology_mutation.hpp"

namespace artminer::core {
namespace {

constexpr u64 kTopologyGridDomain = 0x47524944544f504fULL;  // "GRIDTOPO"

[[nodiscard]] MutationError topology_error(const TopologyMutationError& error) {
    return MutationError{MutationErrorCode::topology_failed, error.message};
}

}  // namespace

Result<std::vector<GeneratedSpecimen>, MutationError> generate_topology_specimen_grid(
    const Recipe& parent,
    const u64 generation_seed,
    const u32 operator_version,
    const double strength,
    const u32 budget,
    const StructuralLocks& locks,
    const NodeRegistry& registry) {
    std::vector<GeneratedSpecimen> specimens;
    specimens.reserve(kSpecimenGridSize);
    for (std::size_t index = 0U; index < kSpecimenGridSize; ++index) {
        const u64 operation_seed =
            derive_seed(generation_seed, kTopologyGridDomain ^ static_cast<u64>(index));
        auto generated = mutate_recipe_topology(
            parent,
            operation_seed,
            operator_version,
            strength,
            budget,
            locks,
            registry);
        if (generated.is_error()) {
            return Result<std::vector<GeneratedSpecimen>, MutationError>::failure(
                topology_error(generated.error()));
        }
        Recipe recipe = std::move(generated).value().recipe;
        specimens.push_back(
            GeneratedSpecimen{index, operation_seed, recipe, semantic_fingerprint(recipe)});
    }
    return Result<std::vector<GeneratedSpecimen>, MutationError>::success(std::move(specimens));
}

}  // namespace artminer::core
