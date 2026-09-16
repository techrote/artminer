#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/types.hpp"

namespace artminer::core {

enum class DataKind {
    scalar_field,
    vector_field,
    colour_field,
    mask,
    particle_set,
    palette,
    image,
};

[[nodiscard]] std::string_view to_string(DataKind kind) noexcept;

enum class ParameterKind {
    integer,
    real,
    boolean,
    enumeration,
};

[[nodiscard]] std::string_view to_string(ParameterKind kind) noexcept;

using ParameterValue = std::variant<i64, double, bool, std::string>;

struct ParameterDomain {
    std::optional<i64> integer_min;
    std::optional<i64> integer_max;
    std::optional<double> real_min;
    std::optional<double> real_max;
    std::vector<std::string> enum_values;
};

enum class MutationScale {
    none,
    linear,
    logarithmic,
    periodic,
    discrete,
};

struct MutationMetadata {
    bool mutable_parameter{true};
    MutationScale scale{MutationScale::linear};
    std::string group;
};

struct PortSpec {
    std::string name;
    DataKind kind{DataKind::scalar_field};
    bool required{true};
    bool allow_multiple{false};
};

struct ParameterSpec {
    std::string name;
    ParameterKind kind{ParameterKind::real};
    ParameterValue default_value{0.0};
    ParameterDomain domain;
    MutationMetadata mutation;
};

enum class ParameterRelationKind {
    less_than,
};

struct ParameterRelation {
    std::string left;
    ParameterRelationKind relation{ParameterRelationKind::less_than};
    std::string right;
};

enum class NodeStateClass {
    stateless,
    stateful,
    state_boundary,
};

struct EvaluatorCapabilities {
    bool cpu{false};
    bool gpu{false};
};

struct NodeMetadata {
    std::string type_id;
    u32 semantic_version{1U};
    std::vector<PortSpec> inputs;
    std::vector<PortSpec> outputs;
    std::vector<ParameterSpec> parameters;
    NodeStateClass state_class{NodeStateClass::stateless};
    EvaluatorCapabilities evaluators;
    // Cross-parameter legality belongs to the catalog, not the generic validator.
    // Relations are evaluated only after the referenced assignments have passed
    // their ordinary type/domain checks.
    std::vector<ParameterRelation> parameter_relations;
};

class NodeRegistry final {
public:
    explicit NodeRegistry(std::vector<NodeMetadata> nodes);

    [[nodiscard]] const NodeMetadata* find(std::string_view type_id) const noexcept;
    [[nodiscard]] const std::vector<NodeMetadata>& nodes() const noexcept { return nodes_; }

private:
    std::vector<NodeMetadata> nodes_;
};

[[nodiscard]] const NodeRegistry& builtin_node_registry();

struct Recipe;

enum class ValidationErrorCode {
    unsupported_schema_version,
    unsupported_evaluator_version,
    resource_limit,
    invalid_render_settings,
    invalid_identifier,
    duplicate_node_id,
    unknown_node_type,
    unsupported_node_version,
    duplicate_parameter,
    unknown_parameter,
    missing_parameter,
    parameter_type_mismatch,
    parameter_out_of_domain,
    missing_node,
    unknown_port,
    incompatible_port_kind,
    duplicate_edge,
    multiple_input_edges,
    missing_required_input,
    duplicate_output_name,
    cycle_detected,
};

struct ValidationError {
    ValidationErrorCode code{};
    std::string message;
};

[[nodiscard]] std::vector<ValidationError> validate_recipe(
    const Recipe& recipe,
    const NodeRegistry& registry = builtin_node_registry());

}  // namespace artminer::core
