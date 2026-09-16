#pragma once

#include <string>
#include <string_view>

#include "core/topology_mutation.hpp"
#include "quarry/quarry.hpp"

namespace artminer::quarry {

inline constexpr core::u32 kTopologySearchConfigVersion = 1U;

struct TopologySearchConfig final {
    core::u32 version{kTopologySearchConfigVersion};
    core::u32 operator_version{core::kTopologyMutationOperatorVersion};
    core::u32 budget{1U};
    core::TopologyLimits limits{};
    core::StructuralLocks locks;
};

struct TopologyCandidate final {
    core::u64 index{0U};
    core::u64 operation_seed{0U};
    std::string search_identity;
    std::string candidate_id;
    std::string recipe_fingerprint;
    core::Recipe recipe;
};

[[nodiscard]] std::string serialize_topology_search_config(const TopologySearchConfig& config);
[[nodiscard]] core::Result<TopologySearchConfig, QuarryError> parse_topology_search_config(std::string_view text);
[[nodiscard]] core::Result<std::string, QuarryError> topology_search_identity(
    const JobManifest& manifest,
    const TopologySearchConfig& config);

// Explicit opt-in structural candidate enumeration. Ordinary Quarry manifests
// continue to use parameter mutation; callers must deliberately supply this
// separately serialized topology-search config.
[[nodiscard]] core::Result<TopologyCandidate, QuarryError> reconstruct_topology_candidate(
    const JobManifest& manifest,
    core::u64 candidate_index,
    const TopologySearchConfig& config,
    const core::NodeRegistry& registry = core::builtin_node_registry());

}  // namespace artminer::quarry
