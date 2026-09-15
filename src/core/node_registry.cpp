#include "core/graph.hpp"

#include <algorithm>
#include <utility>

namespace artminer::core {
namespace {

[[nodiscard]] PortSpec input(std::string name, const DataKind kind, const bool required = true) {
    return PortSpec{std::move(name), kind, required, false};
}

[[nodiscard]] PortSpec output(std::string name, const DataKind kind) {
    return PortSpec{std::move(name), kind, false, false};
}

[[nodiscard]] ParameterSpec real_parameter(
    std::string name,
    const double default_value,
    const double minimum,
    const double maximum,
    std::string group = {}) {
    ParameterDomain domain;
    domain.real_min = minimum;
    domain.real_max = maximum;
    return ParameterSpec{
        std::move(name),
        ParameterKind::real,
        default_value,
        std::move(domain),
        MutationMetadata{true, MutationScale::linear, std::move(group)},
    };
}

[[nodiscard]] ParameterSpec enum_parameter(
    std::string name,
    std::string default_value,
    std::vector<std::string> values,
    std::string group = {}) {
    ParameterDomain domain;
    domain.enum_values = std::move(values);
    return ParameterSpec{
        std::move(name),
        ParameterKind::enumeration,
        std::move(default_value),
        std::move(domain),
        MutationMetadata{true, MutationScale::discrete, std::move(group)},
    };
}

[[nodiscard]] std::vector<NodeMetadata> make_builtin_nodes() {
    // AM-002 defines semantics/metadata only. Evaluator capability flags remain false
    // until AM-003 (CPU) and AM-004 (GPU) actually implement those evaluators.
    constexpr EvaluatorCapabilities unavailable{false, false};
    std::vector<NodeMetadata> nodes;

    nodes.push_back(NodeMetadata{
        "core.scalar.constant",
        1U,
        {},
        {output("value", DataKind::scalar_field)},
        {real_parameter("value", 0.0, -1000000.0, 1000000.0, "value")},
        NodeStateClass::stateless,
        unavailable,
    });

    nodes.push_back(NodeMetadata{
        "core.scalar.pass",
        1U,
        {input("source", DataKind::scalar_field)},
        {output("value", DataKind::scalar_field)},
        {},
        NodeStateClass::stateless,
        unavailable,
    });

    nodes.push_back(NodeMetadata{
        "core.vector.compose",
        1U,
        {input("x", DataKind::scalar_field), input("y", DataKind::scalar_field)},
        {output("value", DataKind::vector_field)},
        {},
        NodeStateClass::stateless,
        unavailable,
    });

    nodes.push_back(NodeMetadata{
        "core.colour.compose",
        1U,
        {
            input("r", DataKind::scalar_field),
            input("g", DataKind::scalar_field),
            input("b", DataKind::scalar_field),
            input("a", DataKind::scalar_field),
        },
        {output("value", DataKind::colour_field)},
        {},
        NodeStateClass::stateless,
        unavailable,
    });

    nodes.push_back(NodeMetadata{
        "core.mask.from_scalar",
        1U,
        {input("source", DataKind::scalar_field)},
        {output("value", DataKind::mask)},
        {},
        NodeStateClass::stateless,
        unavailable,
    });

    nodes.push_back(NodeMetadata{
        "core.particles.empty",
        1U,
        {},
        {output("value", DataKind::particle_set)},
        {},
        NodeStateClass::stateless,
        unavailable,
    });

    nodes.push_back(NodeMetadata{
        "core.palette.default",
        1U,
        {},
        {output("value", DataKind::palette)},
        {enum_parameter("preset", "mono", {"mono", "warm", "cool"}, "palette")},
        NodeStateClass::stateless,
        unavailable,
    });

    nodes.push_back(NodeMetadata{
        "core.image.from_colour",
        1U,
        {input("source", DataKind::colour_field)},
        {output("value", DataKind::image)},
        {},
        NodeStateClass::stateless,
        unavailable,
    });

    nodes.push_back(NodeMetadata{
        "core.image.from_scalar",
        1U,
        {input("source", DataKind::scalar_field)},
        {output("value", DataKind::image)},
        {enum_parameter("palette", "grayscale", {"grayscale", "heat"}, "palette")},
        NodeStateClass::stateless,
        unavailable,
    });

    // Reserved deterministic state-boundary metadata. AM-002 still rejects all graph cycles;
    // AM-007 may later define legal feedback semantics through this class explicitly.
    nodes.push_back(NodeMetadata{
        "core.state.delay.scalar",
        1U,
        {input("next", DataKind::scalar_field)},
        {output("previous", DataKind::scalar_field)},
        {},
        NodeStateClass::state_boundary,
        unavailable,
    });

    return nodes;
}

}  // namespace

std::string_view to_string(const DataKind kind) noexcept {
    switch (kind) {
    case DataKind::scalar_field:
        return "ScalarField";
    case DataKind::vector_field:
        return "VectorField";
    case DataKind::colour_field:
        return "ColourField";
    case DataKind::mask:
        return "Mask";
    case DataKind::particle_set:
        return "ParticleSet";
    case DataKind::palette:
        return "Palette";
    case DataKind::image:
        return "Image";
    }
    return "Unknown";
}

std::string_view to_string(const ParameterKind kind) noexcept {
    switch (kind) {
    case ParameterKind::integer:
        return "i64";
    case ParameterKind::real:
        return "f64";
    case ParameterKind::boolean:
        return "bool";
    case ParameterKind::enumeration:
        return "enum";
    }
    return "unknown";
}

NodeRegistry::NodeRegistry(std::vector<NodeMetadata> nodes) : nodes_(std::move(nodes)) {
    std::sort(nodes_.begin(), nodes_.end(), [](const NodeMetadata& left, const NodeMetadata& right) {
        return left.type_id < right.type_id;
    });
}

const NodeMetadata* NodeRegistry::find(const std::string_view type_id) const noexcept {
    const auto found = std::lower_bound(
        nodes_.begin(),
        nodes_.end(),
        type_id,
        [](const NodeMetadata& node, const std::string_view value) { return node.type_id < value; });
    if (found == nodes_.end() || found->type_id != type_id) {
        return nullptr;
    }
    return &*found;
}

const NodeRegistry& builtin_node_registry() {
    static const NodeRegistry registry(make_builtin_nodes());
    return registry;
}

}  // namespace artminer::core
