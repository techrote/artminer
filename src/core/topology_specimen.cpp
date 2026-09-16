#include "core/topology_specimen.hpp"

#include <utility>

#include "core/prng.hpp"

namespace artminer::core {
namespace {

constexpr u64 kTopologyGridDomain = 0x544f504f47524944ULL;  // "TOPOGRID"

}  // namespace

Result<std::vector<GeneratedSpecimen>, TopologyMutationError> generate_topology_specimen_grid(
    const Recipe& parent,
    const u64 generation_seed,
    const u32 operator_version,
    const u32 budget,
    const StructuralLocks& structural_locks,
    const TopologyLimits limits,
    const NodeRegistry& registry) {
    std::vector<GeneratedSpecimen> specimens;
    specimens.reserve(kSpecimenGridSize);
    for (std::size_t index = 0U; index < kSpecimenGridSize; ++index) {
        const u64 operation_seed = derive_seed(
            generation_seed,
            kTopologyGridDomain ^ splitmix64(static_cast<u64>(index)));
        const TopologyMutationOptions options{operation_seed, operator_version, budget, limits};
        auto mutated = mutate_recipe_topology(parent, options, structural_locks, registry);
        if (mutated.is_error()) {
            return Result<std::vector<GeneratedSpecimen>, TopologyMutationError>::failure(mutated.error());
        }
        Recipe recipe = std::move(mutated).value().recipe;
        const std::string fingerprint = semantic_fingerprint(recipe);
        specimens.push_back(GeneratedSpecimen{index, operation_seed, std::move(recipe), fingerprint});
    }
    return Result<std::vector<GeneratedSpecimen>, TopologyMutationError>::success(std::move(specimens));
}

}  // namespace artminer::core
