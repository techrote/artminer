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
inline constexpr std::size_t kHardMaximumTopologyNodes = 512U;
inline constexpr std::size_t kHardMaximumTopologyEdges = 1024U;
inline constexpr std::size_t kHardMaximumTopologyDepth = 128U;
inline constexpr std::size_t kMaximumTopologyOpportunities = 65536U;

struct TopologyLimits final {
    std::size_t max_nodes{128U};
    std::size_t max_edges{256U};
    std::size_t max_depth{64U};
};

[[nodiscard]] bool operator==(const TopologyLimits& left, const TopologyLimits& right) noexcept;
[[nodiscard]] std::string serialize_topology_limits(const TopologyLimits& limits);
[[nodiscard]] Result<TopologyLimits, std::string> parse_topology_limits(std::string_view text);

enum class TopologyOperatorKind {
    insert_node,
    replace_node,
    delete_bypass,
    duplicate_branch,
    rewire_edge,
};

[[nodiscard]] std::string_view to_string(TopologyOperatorKind kind) noexcept;

struct TopologyOperatorInfo final {
    TopologyOperatorKind kind{TopologyOperatorKind::insert_node};
    std::string_view name;
};

[[nodiscard]] const std::vector<TopologyOperatorInfo>& topology_operator_catalog();

class StructuralLocks final {
public:
    [[nodiscard]] bool node_locked(std::string_view node_id) const;
    void set_node(std::string node_id, bool locked);
    void clear() noexcept;

    // Locks root and every graph descendant reachable from it. This is a model
    // operation, not a UI-only compatibility table; traversal uses recipe edges.
    [[nodiscard]] bool lock_subgraph(const Recipe& recipe, std::string_view root_node);

    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::string serialize_canonical() const;

private:
    std::set<std::string, std::less<>> nodes_;
};

[[nodiscard]] Result<StructuralLocks, std::string> parse_structural_locks(std::string_view text);

enum class TopologyMutationErrorCode {
    invalid_parent,
    unsupported_operator_version,
    invalid_budget,
    invalid_limits,
    resource_limit,
    no_legal_mutation,
    invalid_child,
    malformed_provenance,
    parent_mismatch,
};

struct TopologyMutationError final {
    TopologyMutationErrorCode code{TopologyMutationErrorCode::invalid_child};
    std::string message;
};

struct TopologyMutationOptions final {
    u64 seed{0U};
    u32 operator_version{kTopologyMutationOperatorVersion};
    u32 budget{1U};
    TopologyLimits limits{};
};

struct TopologyMutationStep final {
    TopologyOperatorKind kind{TopologyOperatorKind::insert_node};
    std::string opportunity_key;
};

struct TopologyMutationResult final {
    Recipe recipe;
    std::vector<TopologyMutationStep> steps;
};

struct TopologyMutationProvenance final {
    std::string parent_fingerprint;
    u64 seed{0U};
    u32 operator_version{0U};
    u32 budget{0U};
    TopologyLimits limits{};
    StructuralLocks locks;
    std::string trace;
};

[[nodiscard]] Result<TopologyMutationResult, TopologyMutationError> mutate_recipe_topology(
    const Recipe& parent,
    const TopologyMutationOptions& options,
    const StructuralLocks& locks,
    const NodeRegistry& registry = builtin_node_registry());

[[nodiscard]] Result<TopologyMutationProvenance, TopologyMutationError> topology_provenance_from_recipe(
    const Recipe& child);

[[nodiscard]] Result<Recipe, TopologyMutationError> replay_topology_mutation(
    const Recipe& child_with_provenance,
    const Recipe& parent,
    const NodeRegistry& registry = builtin_node_registry());

[[nodiscard]] std::size_t topology_depth(
    const Recipe& recipe,
    const NodeRegistry& registry = builtin_node_registry());

}  // namespace artminer::core
