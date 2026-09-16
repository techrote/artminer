#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/recipe.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

namespace artminer::core {

inline constexpr u32 kTopologyMutationOperatorVersion = 1U;
inline constexpr u32 kMaximumTopologyMutationBudget = 16U;
inline constexpr std::size_t kMaximumTopologyNodes = 256U;
inline constexpr std::size_t kMaximumTopologyEdges = 512U;
inline constexpr std::size_t kMaximumTopologyDepth = 128U;
inline constexpr std::size_t kMaximumTopologyOpportunities = 65536U;

enum class TopologyOperatorKind {
    insert_node,
    replace_node,
    delete_bypass,
    duplicate_branch,
    rewire_edge,
};

[[nodiscard]] std::string_view to_string(TopologyOperatorKind kind) noexcept;

struct TopologyOperatorDescriptor final {
    TopologyOperatorKind kind{TopologyOperatorKind::insert_node};
    std::string_view name;
};

[[nodiscard]] const std::vector<TopologyOperatorDescriptor>& topology_operator_catalog();

class StructuralLocks final {
public:
    [[nodiscard]] bool node_locked(std::string_view node_id) const;
    void set_node(std::string node_id, bool locked);
    void toggle_node(std::string_view node_id);
    void clear() noexcept { nodes_.clear(); }

    // Locks/unlocks root and its complete upstream dependency closure.
    // This is deliberately structural rather than spatial/UI-defined.
    void set_upstream_subgraph(const Recipe& recipe, std::string_view root_node_id, bool locked);

    [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::string serialize_canonical() const;
    [[nodiscard]] const std::set<std::string, std::less<>>& nodes() const noexcept { return nodes_; }

private:
    std::set<std::string, std::less<>> nodes_;
};

enum class TopologyMutationErrorCode {
    invalid_parent,
    unsupported_operator_version,
    invalid_strength,
    invalid_budget,
    invalid_locks,
    resource_limit,
    no_legal_operation,
    invalid_child,
    malformed_provenance,
};

struct TopologyMutationError final {
    TopologyMutationErrorCode code{TopologyMutationErrorCode::invalid_child};
    std::string message;
};

struct TopologyMutationStep final {
    TopologyOperatorKind kind{TopologyOperatorKind::insert_node};
    std::string description;
};

struct TopologyMutationResult final {
    Recipe recipe;
    std::vector<TopologyMutationStep> steps;
    u32 requested_steps{0U};
    bool exhausted{false};
};

[[nodiscard]] Result<StructuralLocks, TopologyMutationError> parse_structural_locks(std::string_view text);

[[nodiscard]] Result<TopologyMutationResult, TopologyMutationError> mutate_recipe_topology(
    const Recipe& parent,
    u64 topology_seed,
    u32 operator_version,
    double strength,
    u32 budget,
    const StructuralLocks& locks,
    const NodeRegistry& registry = builtin_node_registry());

}  // namespace artminer::core
