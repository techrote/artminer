#pragma once

#include <vector>

#include "core/specimen_browser.hpp"
#include "core/topology_mutation.hpp"

namespace artminer::core {

// Explicit topology-search companion to the ordinary parameter/seed specimen
// generator. It intentionally has a different API so a UI cannot silently turn
// a numeric mutation action into a structural program edit.
[[nodiscard]] Result<std::vector<GeneratedSpecimen>, TopologyMutationError> generate_topology_specimen_grid(
    const Recipe& parent,
    u64 generation_seed,
    u32 operator_version,
    u32 budget,
    const StructuralLocks& structural_locks,
    TopologyLimits limits = {},
    const NodeRegistry& registry = builtin_node_registry());

}  // namespace artminer::core
