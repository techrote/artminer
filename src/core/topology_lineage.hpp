#pragma once

#include <string>
#include <string_view>

#include "core/topology_mutation.hpp"

namespace artminer::core {

inline constexpr u32 kTopologyLineageFormatVersion = 1U;

struct TopologyLineageRecord final {
    std::string child_fingerprint;
    std::string parent_fingerprint;
    u32 operator_version{kTopologyMutationOperatorVersion};
    u64 operation_seed{0U};
    u32 budget{1U};
    TopologyLimits limits{};
    StructuralLocks locks;
    std::string trace;
};

enum class TopologyLineageErrorCode {
    malformed,
    unsupported_version,
    no_lineage,
    parent_mismatch,
    replay_failed,
};

struct TopologyLineageError final {
    TopologyLineageErrorCode code{TopologyLineageErrorCode::malformed};
    std::string message;
};

[[nodiscard]] Result<TopologyLineageRecord, TopologyLineageError> topology_lineage_record_from_recipe(
    const Recipe& child);
[[nodiscard]] std::string serialize_topology_lineage_record(const TopologyLineageRecord& record);
[[nodiscard]] Result<TopologyLineageRecord, TopologyLineageError> parse_topology_lineage_record(
    std::string_view text);
[[nodiscard]] Result<Recipe, TopologyLineageError> replay_topology_lineage_record(
    const TopologyLineageRecord& record,
    const Recipe& parent,
    const NodeRegistry& registry = builtin_node_registry());

}  // namespace artminer::core
