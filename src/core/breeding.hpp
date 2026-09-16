#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/recipe.hpp"
#include "core/result.hpp"
#include "core/specimen_browser.hpp"
#include "core/topology_mutation.hpp"
#include "core/types.hpp"

namespace artminer::core {

inline constexpr u32 kCrossoverOperatorVersion = 1U;
inline constexpr u32 kLineageFormatVersion = 2U;

enum class CrossoverErrorCode {
    invalid_parent,
    incompatible_parents,
    unsupported_operator_version,
    invalid_child,
};

struct CrossoverError {
    CrossoverErrorCode code{CrossoverErrorCode::invalid_child};
    std::string message;
};

[[nodiscard]] Result<Recipe, CrossoverError> crossover_recipes(
    const Recipe& parent_a,
    const Recipe& parent_b,
    u64 crossover_seed,
    u32 operator_version,
    const ParameterLocks& locks,
    const NodeRegistry& registry = builtin_node_registry());

enum class LineageOperationKind {
    parameter_mutation,
    seed_variant,
    crossover,
    topology_mutation,
};

struct LineageRecord {
    LineageOperationKind kind{LineageOperationKind::parameter_mutation};
    std::string child_fingerprint;
    std::string parent_a_fingerprint;
    std::optional<std::string> parent_b_fingerprint;
    u32 operator_version{0U};
    u64 operation_seed{0U};
    std::optional<double> mutation_strength;
    std::string locks;
    std::optional<u32> topology_budget;
    std::string structural_locks;
};

enum class LineageErrorCode {
    no_lineage,
    malformed_lineage,
    unsupported_lineage_version,
    parent_mismatch,
    replay_failed,
};

struct LineageError {
    LineageErrorCode code{LineageErrorCode::malformed_lineage};
    std::string message;
};

[[nodiscard]] LineageRecord make_crossover_lineage_record(
    const Recipe& child,
    const Recipe& parent_a,
    const Recipe& parent_b,
    u64 crossover_seed,
    u32 operator_version,
    const ParameterLocks& locks);

[[nodiscard]] LineageRecord make_parameter_mutation_lineage_record(
    const Recipe& child,
    const Recipe& parent,
    u64 mutation_seed,
    u32 operator_version,
    double strength,
    const ParameterLocks& locks);

[[nodiscard]] LineageRecord make_seed_variant_lineage_record(
    const Recipe& child,
    const Recipe& parent,
    u64 variation_seed,
    u32 operator_version = kParameterMutationOperatorVersion);

[[nodiscard]] LineageRecord make_topology_mutation_lineage_record(
    const Recipe& child,
    const Recipe& parent,
    u64 topology_seed,
    u32 operator_version,
    double strength,
    u32 budget,
    const StructuralLocks& locks);

[[nodiscard]] std::string serialize_lineage_record(const LineageRecord& record);
[[nodiscard]] Result<LineageRecord, LineageError> parse_lineage_record(std::string_view text);
[[nodiscard]] Result<LineageRecord, LineageError> lineage_record_from_recipe(const Recipe& recipe);

[[nodiscard]] Result<Recipe, LineageError> replay_lineage_record(
    const LineageRecord& record,
    const Recipe& parent_a,
    const Recipe* parent_b = nullptr,
    const NodeRegistry& registry = builtin_node_registry());

struct RecipeDiffEntry {
    std::string path;
    std::string before;
    std::string after;
};

struct RecipeDiff {
    std::vector<RecipeDiffEntry> semantic;
    std::vector<RecipeDiffEntry> provenance;
};

[[nodiscard]] RecipeDiff diff_recipes(const Recipe& before, const Recipe& after);
[[nodiscard]] std::string format_recipe_diff(const RecipeDiff& diff);

}  // namespace artminer::core
