#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/recipe.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

namespace artminer::core {

inline constexpr u32 kParameterMutationOperatorVersion = 1U;
inline constexpr std::size_t kSpecimenGridSize = 16U;

class StructuralLocks;

enum class SpecimenGenerationMode {
    parameter_mutation,
    seed_only,
};

enum class MutationErrorCode {
    invalid_parent,
    unsupported_operator_version,
    invalid_strength,
    unknown_node,
    unknown_parameter,
    invalid_parameter_value,
    invalid_child,
    topology_failed,
};

struct MutationError {
    MutationErrorCode code{MutationErrorCode::invalid_child};
    std::string message;
};

class ParameterLocks final {
public:
    [[nodiscard]] bool parameter_locked(std::string_view node_id, std::string_view parameter) const;
    [[nodiscard]] bool group_locked(std::string_view node_id, std::string_view group) const;
    [[nodiscard]] bool locked(std::string_view node_id, const ParameterSpec& parameter) const;

    void set_parameter(std::string node_id, std::string parameter, bool locked);
    void set_group(std::string node_id, std::string group, bool locked);
    void toggle_parameter(std::string_view node_id, std::string_view parameter);
    void toggle_group(std::string_view node_id, std::string_view group);
    void clear() noexcept;

    [[nodiscard]] std::size_t parameter_count() const noexcept { return parameters_.size(); }
    [[nodiscard]] std::size_t group_count() const noexcept { return groups_.size(); }
    [[nodiscard]] std::string serialize_canonical() const;

private:
    using LockKey = std::pair<std::string, std::string>;
    std::set<LockKey> parameters_;
    std::set<LockKey> groups_;
};

struct GeneratedSpecimen {
    std::size_t index{0U};
    u64 operation_seed{0U};
    Recipe recipe;
    std::string fingerprint;
};

[[nodiscard]] Result<Recipe, MutationError> mutate_recipe_parameters(
    const Recipe& parent,
    u64 mutation_seed,
    u32 operator_version,
    double strength,
    const ParameterLocks& locks,
    const NodeRegistry& registry = builtin_node_registry());

[[nodiscard]] Result<Recipe, MutationError> make_seed_variant(
    const Recipe& parent,
    u64 variation_seed,
    u32 operator_version = kParameterMutationOperatorVersion);

[[nodiscard]] Result<std::vector<GeneratedSpecimen>, MutationError> generate_specimen_grid(
    const Recipe& parent,
    u64 generation_seed,
    u32 operator_version,
    double strength,
    SpecimenGenerationMode mode,
    const ParameterLocks& locks,
    const NodeRegistry& registry = builtin_node_registry());

// AM-014 structural specimen generation is deliberately separate from parameter
// mutation: topology budget and structural locks are explicit search semantics.
[[nodiscard]] Result<std::vector<GeneratedSpecimen>, MutationError> generate_topology_specimen_grid(
    const Recipe& parent,
    u64 generation_seed,
    u32 operator_version,
    double strength,
    u32 budget,
    const StructuralLocks& locks,
    const NodeRegistry& registry = builtin_node_registry());

[[nodiscard]] Result<Recipe, MutationError> set_parameter_from_text(
    const Recipe& source,
    std::string_view node_id,
    std::string_view parameter_name,
    std::string_view text,
    const NodeRegistry& registry = builtin_node_registry());

[[nodiscard]] std::string format_parameter_value(const ParameterValue& value);

class RecipeHistory final {
public:
    explicit RecipeHistory(Recipe initial);

    [[nodiscard]] const Recipe& current() const noexcept { return entries_[cursor_]; }
    [[nodiscard]] bool can_back() const noexcept { return cursor_ > 0U; }
    [[nodiscard]] bool can_forward() const noexcept { return cursor_ + 1U < entries_.size(); }
    [[nodiscard]] std::size_t position() const noexcept { return cursor_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    void push(Recipe recipe);
    [[nodiscard]] bool back() noexcept;
    [[nodiscard]] bool forward() noexcept;

private:
    std::vector<Recipe> entries_;
    std::size_t cursor_{0U};
};

}  // namespace artminer::core
