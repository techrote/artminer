#include "core/graph.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <set>
#include <tuple>
#include <utility>

#include "core/checked_math.hpp"
#include "core/recipe.hpp"

namespace artminer::core {
namespace {

[[nodiscard]] bool is_identifier_character(const char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '.';
}

[[nodiscard]] bool is_valid_identifier(const std::string_view value) noexcept {
    if (value.empty() || value.size() > 96U) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), is_identifier_character);
}

[[nodiscard]] const PortSpec* find_port(const std::vector<PortSpec>& ports, const std::string_view name) noexcept {
    const auto found = std::find_if(ports.begin(), ports.end(), [&](const PortSpec& port) { return port.name == name; });
    return found == ports.end() ? nullptr : &*found;
}

[[nodiscard]] const ParameterSpec* find_parameter(
    const std::vector<ParameterSpec>& parameters,
    const std::string_view name) noexcept {
    const auto found = std::find_if(
        parameters.begin(), parameters.end(), [&](const ParameterSpec& parameter) { return parameter.name == name; });
    return found == parameters.end() ? nullptr : &*found;
}

[[nodiscard]] ParameterKind parameter_kind(const ParameterValue& value) noexcept {
    if (std::holds_alternative<i64>(value)) {
        return ParameterKind::integer;
    }
    if (std::holds_alternative<double>(value)) {
        return ParameterKind::real;
    }
    if (std::holds_alternative<bool>(value)) {
        return ParameterKind::boolean;
    }
    return ParameterKind::enumeration;
}

[[nodiscard]] bool parameter_in_domain(const ParameterValue& value, const ParameterSpec& spec) {
    switch (spec.kind) {
    case ParameterKind::integer: {
        const i64 integer = std::get<i64>(value);
        if (spec.domain.integer_min.has_value() && integer < *spec.domain.integer_min) {
            return false;
        }
        return !spec.domain.integer_max.has_value() || integer <= *spec.domain.integer_max;
    }
    case ParameterKind::real: {
        const double real = std::get<double>(value);
        if (!std::isfinite(real)) {
            return false;
        }
        if (spec.domain.real_min.has_value() && real < *spec.domain.real_min) {
            return false;
        }
        return !spec.domain.real_max.has_value() || real <= *spec.domain.real_max;
    }
    case ParameterKind::boolean:
        return true;
    case ParameterKind::enumeration: {
        const std::string& enumeration = std::get<std::string>(value);
        return std::find(spec.domain.enum_values.begin(), spec.domain.enum_values.end(), enumeration) !=
               spec.domain.enum_values.end();
    }
    }
    return false;
}

void add_error(
    std::vector<ValidationError>& errors,
    const ValidationErrorCode code,
    std::string message) {
    errors.push_back(ValidationError{code, std::move(message)});
}

}  // namespace

std::vector<ValidationError> validate_recipe(const Recipe& recipe, const NodeRegistry& registry) {
    std::vector<ValidationError> errors;

    if (recipe.schema_version != kRecipeSchemaVersion) {
        add_error(
            errors,
            ValidationErrorCode::unsupported_schema_version,
            "recipe schema version " + std::to_string(recipe.schema_version) + " is unsupported");
    }
    if (recipe.evaluator_version != kEvaluatorSemanticVersion) {
        add_error(
            errors,
            ValidationErrorCode::unsupported_evaluator_version,
            "evaluator semantic version " + std::to_string(recipe.evaluator_version) + " is unsupported");
    }
    if (recipe.render.width == 0U || recipe.render.height == 0U || recipe.render.width > 16384U ||
        recipe.render.height > 16384U || recipe.render.quality != "reference" ||
        checked_image_byte_count(recipe.render.width, recipe.render.height, 4U).is_error()) {
        add_error(
            errors,
            ValidationErrorCode::invalid_render_settings,
            "render settings must use dimensions 1..16384 and quality 'reference'");
    }

    std::map<std::string, std::size_t, std::less<>> node_indices;
    std::map<std::string, const NodeMetadata*, std::less<>> node_metadata;

    for (std::size_t index = 0U; index < recipe.nodes.size(); ++index) {
        const NodeInstance& node = recipe.nodes[index];
        if (!is_valid_identifier(node.id) || !is_valid_identifier(node.type_id)) {
            add_error(
                errors,
                ValidationErrorCode::invalid_identifier,
                "node id/type identifiers must be 1..96 ASCII letters, digits, '.', '_' or '-': " + node.id);
        }

        const auto inserted = node_indices.emplace(node.id, index);
        if (!inserted.second) {
            add_error(errors, ValidationErrorCode::duplicate_node_id, "duplicate node id: " + node.id);
            continue;
        }

        const NodeMetadata* metadata = registry.find(node.type_id);
        node_metadata.emplace(node.id, metadata);
        if (metadata == nullptr) {
            add_error(errors, ValidationErrorCode::unknown_node_type, "unknown node type: " + node.type_id);
            continue;
        }
        if (node.semantic_version != metadata->semantic_version) {
            add_error(
                errors,
                ValidationErrorCode::unsupported_node_version,
                "node '" + node.id + "' requests unsupported semantic version " +
                    std::to_string(node.semantic_version) + " for type " + node.type_id);
        }

        std::set<std::string, std::less<>> seen_parameters;
        for (const auto& assignment : node.parameters) {
            if (!seen_parameters.insert(assignment.name).second) {
                add_error(
                    errors,
                    ValidationErrorCode::duplicate_parameter,
                    "node '" + node.id + "' has duplicate parameter '" + assignment.name + "'");
                continue;
            }
            const ParameterSpec* spec = find_parameter(metadata->parameters, assignment.name);
            if (spec == nullptr) {
                add_error(
                    errors,
                    ValidationErrorCode::unknown_parameter,
                    "node '" + node.id + "' has unknown parameter '" + assignment.name + "'");
                continue;
            }
            const ParameterKind actual_kind = parameter_kind(assignment.value);
            if (actual_kind != spec->kind) {
                add_error(
                    errors,
                    ValidationErrorCode::parameter_type_mismatch,
                    "node '" + node.id + "' parameter '" + assignment.name + "' expected " +
                        std::string(to_string(spec->kind)) + " but received " + std::string(to_string(actual_kind)));
                continue;
            }
            if (!parameter_in_domain(assignment.value, *spec)) {
                add_error(
                    errors,
                    ValidationErrorCode::parameter_out_of_domain,
                    "node '" + node.id + "' parameter '" + assignment.name + "' is outside its declared domain");
            }
        }

        for (const auto& spec : metadata->parameters) {
            if (!seen_parameters.contains(spec.name)) {
                add_error(
                    errors,
                    ValidationErrorCode::missing_parameter,
                    "node '" + node.id + "' is missing explicit parameter '" + spec.name + "'");
            }
        }
    }

    using EdgeKey = std::tuple<std::string, std::string, std::string, std::string>;
    std::set<EdgeKey> seen_edges;
    std::map<std::pair<std::string, std::string>, std::size_t> input_edge_counts;
    std::vector<std::vector<std::size_t>> adjacency(recipe.nodes.size());
    std::vector<std::size_t> indegree(recipe.nodes.size(), 0U);

    for (const auto& edge : recipe.edges) {
        const EdgeKey edge_key{edge.from_node, edge.from_port, edge.to_node, edge.to_port};
        if (!seen_edges.insert(edge_key).second) {
            add_error(
                errors,
                ValidationErrorCode::duplicate_edge,
                "duplicate edge: " + edge.from_node + "." + edge.from_port + " -> " + edge.to_node + "." + edge.to_port);
        }

        const auto from_index = node_indices.find(edge.from_node);
        const auto to_index = node_indices.find(edge.to_node);
        if (from_index == node_indices.end() || to_index == node_indices.end()) {
            add_error(
                errors,
                ValidationErrorCode::missing_node,
                "edge references missing node: " + edge.from_node + "." + edge.from_port + " -> " + edge.to_node + "." +
                    edge.to_port);
            continue;
        }

        const NodeMetadata* from_metadata = node_metadata[edge.from_node];
        const NodeMetadata* to_metadata = node_metadata[edge.to_node];
        if (from_metadata == nullptr || to_metadata == nullptr) {
            continue;
        }

        const PortSpec* from_port = find_port(from_metadata->outputs, edge.from_port);
        const PortSpec* to_port = find_port(to_metadata->inputs, edge.to_port);
        if (from_port == nullptr || to_port == nullptr) {
            add_error(
                errors,
                ValidationErrorCode::unknown_port,
                "edge references an unknown output/input port: " + edge.from_node + "." + edge.from_port + " -> " +
                    edge.to_node + "." + edge.to_port);
            continue;
        }
        if (from_port->kind != to_port->kind) {
            add_error(
                errors,
                ValidationErrorCode::incompatible_port_kind,
                "edge type mismatch: " + edge.from_node + "." + edge.from_port + " is " +
                    std::string(to_string(from_port->kind)) + " but " + edge.to_node + "." + edge.to_port + " expects " +
                    std::string(to_string(to_port->kind)));
        }

        const auto input_key = std::make_pair(edge.to_node, edge.to_port);
        const std::size_t count = ++input_edge_counts[input_key];
        if (count > 1U && !to_port->allow_multiple) {
            add_error(
                errors,
                ValidationErrorCode::multiple_input_edges,
                "input port accepts only one edge: " + edge.to_node + "." + edge.to_port);
        }

        adjacency[from_index->second].push_back(to_index->second);
        ++indegree[to_index->second];
    }

    for (const auto& [node_id, metadata] : node_metadata) {
        if (metadata == nullptr) {
            continue;
        }
        for (const auto& input_spec : metadata->inputs) {
            if (input_spec.required && input_edge_counts[{node_id, input_spec.name}] == 0U) {
                add_error(
                    errors,
                    ValidationErrorCode::missing_required_input,
                    "node '" + node_id + "' is missing required input '" + input_spec.name + "'");
            }
        }
    }

    std::set<std::string, std::less<>> output_names;
    for (const auto& binding : recipe.outputs) {
        if (!is_valid_identifier(binding.name)) {
            add_error(
                errors,
                ValidationErrorCode::invalid_identifier,
                "output name is not a valid identifier: " + binding.name);
        }
        if (!output_names.insert(binding.name).second) {
            add_error(errors, ValidationErrorCode::duplicate_output_name, "duplicate output name: " + binding.name);
        }

        const auto node_index = node_indices.find(binding.node_id);
        if (node_index == node_indices.end()) {
            add_error(
                errors,
                ValidationErrorCode::missing_node,
                "output '" + binding.name + "' references missing node '" + binding.node_id + "'");
            continue;
        }
        const NodeMetadata* metadata = node_metadata[binding.node_id];
        if (metadata != nullptr && find_port(metadata->outputs, binding.port) == nullptr) {
            add_error(
                errors,
                ValidationErrorCode::unknown_port,
                "output '" + binding.name + "' references unknown output port '" + binding.node_id + "." + binding.port + "'");
        }
    }

    std::queue<std::size_t> ready;
    for (std::size_t index = 0U; index < indegree.size(); ++index) {
        if (indegree[index] == 0U) {
            ready.push(index);
        }
    }
    std::size_t visited = 0U;
    while (!ready.empty()) {
        const std::size_t node = ready.front();
        ready.pop();
        ++visited;
        for (const std::size_t next : adjacency[node]) {
            --indegree[next];
            if (indegree[next] == 0U) {
                ready.push(next);
            }
        }
    }
    if (visited != recipe.nodes.size()) {
        add_error(
            errors,
            ValidationErrorCode::cycle_detected,
            "ordinary graph cycles are invalid; state feedback remains reserved for a later explicit boundary contract");
    }

    return errors;
}

}  // namespace artminer::core
