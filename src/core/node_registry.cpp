#include "core/graph.hpp"

#include <algorithm>
#include <utility>

#include "core/growth_nodes.hpp"

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
    std::string group = {},
    const MutationScale scale = MutationScale::linear) {
    ParameterDomain domain;
    domain.real_min = minimum;
    domain.real_max = maximum;
    return ParameterSpec{
        std::move(name),
        ParameterKind::real,
        default_value,
        std::move(domain),
        MutationMetadata{true, scale, std::move(group)},
    };
}

[[nodiscard]] ParameterSpec integer_parameter(
    std::string name,
    const i64 default_value,
    const i64 minimum,
    const i64 maximum,
    std::string group = {}) {
    ParameterDomain domain;
    domain.integer_min = minimum;
    domain.integer_max = maximum;
    return ParameterSpec{
        std::move(name),
        ParameterKind::integer,
        default_value,
        std::move(domain),
        MutationMetadata{true, MutationScale::discrete, std::move(group)},
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

[[nodiscard]] std::vector<ParameterSpec> gradient_parameters() {
    return {
        real_parameter("r0", 0.02, 0.0, 1.0, "colour0"),
        real_parameter("g0", 0.04, 0.0, 1.0, "colour0"),
        real_parameter("b0", 0.12, 0.0, 1.0, "colour0"),
        real_parameter("a0", 1.0, 0.0, 1.0, "colour0"),
        real_parameter("r1", 0.95, 0.0, 1.0, "colour1"),
        real_parameter("g1", 0.55, 0.0, 1.0, "colour1"),
        real_parameter("b1", 0.08, 0.0, 1.0, "colour1"),
        real_parameter("a1", 1.0, 0.0, 1.0, "colour1"),
    };
}

[[nodiscard]] std::vector<NodeMetadata> make_builtin_nodes() {
    constexpr EvaluatorCapabilities cpu_reference{true, false};
    constexpr EvaluatorCapabilities unavailable{false, false};
    std::vector<NodeMetadata> nodes;

    nodes.push_back(NodeMetadata{
        "core.scalar.constant",
        1U,
        {},
        {output("value", DataKind::scalar_field)},
        {real_parameter("value", 0.0, -1000000.0, 1000000.0, "value")},
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.scalar.pass",
        1U,
        {input("source", DataKind::scalar_field)},
        {output("value", DataKind::scalar_field)},
        {},
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.scalar.coord_x",
        1U,
        {},
        {output("value", DataKind::scalar_field)},
        {},
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.scalar.coord_y",
        1U,
        {},
        {output("value", DataKind::scalar_field)},
        {},
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.scalar.radial",
        1U,
        {},
        {output("value", DataKind::scalar_field)},
        {
            real_parameter("center_x", 0.5, -2.0, 3.0, "position"),
            real_parameter("center_y", 0.5, -2.0, 3.0, "position"),
            real_parameter("scale", 1.0, 0.001, 100.0, "scale", MutationScale::logarithmic),
        },
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.scalar.angular",
        1U,
        {},
        {output("value", DataKind::scalar_field)},
        {
            real_parameter("center_x", 0.5, -2.0, 3.0, "position"),
            real_parameter("center_y", 0.5, -2.0, 3.0, "position"),
            real_parameter("turns", 1.0, -64.0, 64.0, "angle"),
            real_parameter("phase", 0.0, -64.0, 64.0, "angle", MutationScale::periodic),
        },
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.noise.value",
        1U,
        {},
        {output("value", DataKind::scalar_field)},
        {
            real_parameter("frequency", 8.0, 0.01, 512.0, "noise", MutationScale::logarithmic),
            real_parameter("offset_x", 0.0, -1000000.0, 1000000.0, "offset"),
            real_parameter("offset_y", 0.0, -1000000.0, 1000000.0, "offset"),
        },
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.noise.gradient",
        1U,
        {},
        {output("value", DataKind::scalar_field)},
        {
            real_parameter("frequency", 8.0, 0.01, 512.0, "noise", MutationScale::logarithmic),
            real_parameter("offset_x", 0.0, -1000000.0, 1000000.0, "offset"),
            real_parameter("offset_y", 0.0, -1000000.0, 1000000.0, "offset"),
        },
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.noise.worley",
        1U,
        {},
        {output("value", DataKind::scalar_field)},
        {
            real_parameter("frequency", 10.0, 0.01, 256.0, "noise", MutationScale::logarithmic),
            real_parameter("distance_scale", 1.25, 0.01, 8.0, "noise", MutationScale::logarithmic),
        },
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.noise.fbm",
        1U,
        {},
        {output("value", DataKind::scalar_field)},
        {
            enum_parameter("basis", "gradient", {"value", "gradient"}, "noise"),
            real_parameter("frequency", 4.0, 0.01, 256.0, "noise", MutationScale::logarithmic),
            integer_parameter("octaves", 5, 1, 10, "noise"),
            real_parameter("lacunarity", 2.0, 1.0, 8.0, "noise", MutationScale::logarithmic),
            real_parameter("gain", 0.5, 0.0, 1.0, "noise"),
        },
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.scalar.warp",
        1U,
        {
            input("source", DataKind::scalar_field),
            input("x_offset", DataKind::scalar_field),
            input("y_offset", DataKind::scalar_field),
        },
        {output("value", DataKind::scalar_field)},
        {
            real_parameter("strength", 0.1, -2.0, 2.0, "warp"),
            enum_parameter("wrap", "repeat", {"clamp", "repeat"}, "warp"),
        },
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.sdf.circle",
        1U,
        {},
        {output("value", DataKind::scalar_field)},
        {
            real_parameter("center_x", 0.5, -2.0, 3.0, "position"),
            real_parameter("center_y", 0.5, -2.0, 3.0, "position"),
            real_parameter("radius", 0.25, 0.001, 2.0, "shape", MutationScale::logarithmic),
        },
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.sdf.box",
        1U,
        {},
        {output("value", DataKind::scalar_field)},
        {
            real_parameter("center_x", 0.5, -2.0, 3.0, "position"),
            real_parameter("center_y", 0.5, -2.0, 3.0, "position"),
            real_parameter("half_width", 0.25, 0.001, 2.0, "shape", MutationScale::logarithmic),
            real_parameter("half_height", 0.25, 0.001, 2.0, "shape", MutationScale::logarithmic),
        },
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.scalar.minimum",
        1U,
        {input("a", DataKind::scalar_field), input("b", DataKind::scalar_field)},
        {output("value", DataKind::scalar_field)},
        {},
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.scalar.maximum",
        1U,
        {input("a", DataKind::scalar_field), input("b", DataKind::scalar_field)},
        {output("value", DataKind::scalar_field)},
        {},
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.scalar.threshold",
        1U,
        {input("source", DataKind::scalar_field)},
        {output("value", DataKind::scalar_field)},
        {
            real_parameter("threshold", 0.5, -1000000.0, 1000000.0, "threshold"),
            real_parameter("low", 0.0, -1000000.0, 1000000.0, "range"),
            real_parameter("high", 1.0, -1000000.0, 1000000.0, "range"),
        },
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.scalar.quantize",
        1U,
        {input("source", DataKind::scalar_field)},
        {output("value", DataKind::scalar_field)},
        {
            integer_parameter("levels", 8, 2, 256, "quantize"),
            real_parameter("minimum", 0.0, -1000000.0, 1000000.0, "range"),
            real_parameter("maximum", 1.0, -1000000.0, 1000000.0, "range"),
        },
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.scalar.transform.repeat",
        1U,
        {input("source", DataKind::scalar_field)},
        {output("value", DataKind::scalar_field)},
        {
            integer_parameter("x_count", 2, 1, 32, "repeat"),
            integer_parameter("y_count", 2, 1, 32, "repeat"),
        },
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.scalar.transform.symmetry",
        1U,
        {input("source", DataKind::scalar_field)},
        {output("value", DataKind::scalar_field)},
        {enum_parameter("mode", "xy", {"x", "y", "xy"}, "symmetry")},
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.vector.compose",
        1U,
        {input("x", DataKind::scalar_field), input("y", DataKind::scalar_field)},
        {output("value", DataKind::vector_field)},
        {},
        NodeStateClass::stateless,
        cpu_reference,
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
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.scalar.from_colour",
        1U,
        {input("source", DataKind::colour_field)},
        {output("value", DataKind::scalar_field)},
        {enum_parameter("channel", "luminance", {"r", "g", "b", "a", "luminance"}, "channel")},
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.mask.from_scalar",
        1U,
        {input("source", DataKind::scalar_field)},
        {output("value", DataKind::mask)},
        {},
        NodeStateClass::stateless,
        cpu_reference,
    });

    // Particle semantics remain deliberately deferred to the fixed-tick milestones.
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
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.palette.gradient2",
        1U,
        {},
        {output("value", DataKind::palette)},
        gradient_parameters(),
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.colour.from_palette",
        1U,
        {input("source", DataKind::scalar_field), input("palette", DataKind::palette)},
        {output("value", DataKind::colour_field)},
        {enum_parameter("mode", "linear", {"linear", "nearest"}, "palette")},
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.image.from_colour",
        1U,
        {input("source", DataKind::colour_field)},
        {output("value", DataKind::image)},
        {},
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.image.from_scalar",
        1U,
        {input("source", DataKind::scalar_field)},
        {output("value", DataKind::image)},
        {enum_parameter("palette", "grayscale", {"grayscale", "heat"}, "palette")},
        NodeStateClass::stateless,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.image.ordered_dither",
        1U,
        {input("source", DataKind::colour_field)},
        {output("value", DataKind::image)},
        {
            integer_parameter("levels", 2, 2, 16, "dither"),
            enum_parameter("matrix", "bayer4", {"bayer4", "bayer8"}, "dither"),
        },
        NodeStateClass::stateless,
        cpu_reference,
    });

    // Reserved deterministic state-boundary metadata. AM-003 still rejects all graph cycles;
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

    append_growth_node_metadata(nodes);
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
