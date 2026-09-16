#include "core/material_nodes.hpp"

#include <string>
#include <utility>
#include <vector>

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
    std::string group,
    const MutationScale scale = MutationScale::linear) {
    ParameterDomain domain;
    domain.real_min = minimum;
    domain.real_max = maximum;
    return ParameterSpec{
        std::move(name), ParameterKind::real, default_value, std::move(domain),
        MutationMetadata{true, scale, std::move(group)}};
}

[[nodiscard]] ParameterSpec integer_parameter(
    std::string name,
    const i64 default_value,
    const i64 minimum,
    const i64 maximum,
    std::string group) {
    ParameterDomain domain;
    domain.integer_min = minimum;
    domain.integer_max = maximum;
    return ParameterSpec{
        std::move(name), ParameterKind::integer, default_value, std::move(domain),
        MutationMetadata{true, MutationScale::discrete, std::move(group)}};
}

[[nodiscard]] ParameterSpec enum_parameter(
    std::string name,
    std::string default_value,
    std::vector<std::string> values,
    std::string group) {
    ParameterDomain domain;
    domain.enum_values = std::move(values);
    return ParameterSpec{
        std::move(name), ParameterKind::enumeration, std::move(default_value), std::move(domain),
        MutationMetadata{true, MutationScale::discrete, std::move(group)}};
}

[[nodiscard]] std::vector<std::string> image_channels() {
    return {"luminance", "r", "g", "b", "a"};
}

}  // namespace

void append_material_node_metadata(std::vector<NodeMetadata>& nodes) {
    // These nodes are canonical, deterministic CPU workflow operations, but are
    // intentionally not advertised through NodeMetadata::evaluators.cpu because
    // the generic AM-003 evaluator has no semantic HeightField subtype. The
    // AM-013 workflow evaluator enforces that subtype explicitly.
    constexpr EvaluatorCapabilities workflow_only{false, false};

    nodes.push_back(NodeMetadata{
        "core.material.height_from_image",
        1U,
        {input("source", DataKind::image)},
        {output("height", DataKind::scalar_field)},
        {
            enum_parameter("channel", "luminance", image_channels(), "material.height.source"),
            real_parameter("minimum", 0.0, 0.0, 1.0, "material.height.range"),
            real_parameter("maximum", 1.0, 0.0, 1.0, "material.height.range"),
            enum_parameter("invert", "no", {"no", "yes"}, "material.height.range"),
        },
        NodeStateClass::stateless,
        workflow_only,
    });

    nodes.push_back(NodeMetadata{
        "core.material.height_image",
        1U,
        {input("height", DataKind::scalar_field)},
        {output("value", DataKind::image)},
        {},
        NodeStateClass::stateless,
        workflow_only,
    });

    nodes.push_back(NodeMetadata{
        "core.material.normal_from_height",
        1U,
        {input("height", DataKind::scalar_field)},
        {output("value", DataKind::image)},
        {
            enum_parameter("filter", "central", {"central", "sobel"}, "material.normal.filter"),
            enum_parameter("edge", "repeat", {"clamp", "repeat"}, "material.normal.filter"),
            real_parameter("strength", 2.0, 0.001, 64.0, "material.normal.scale", MutationScale::logarithmic),
            real_parameter("texel_scale", 1.0, 0.001, 64.0, "material.normal.scale", MutationScale::logarithmic),
            enum_parameter("handedness", "right", {"right", "left"}, "material.normal.convention"),
            enum_parameter("convention", "opengl", {"opengl", "directx"}, "material.normal.convention"),
        },
        NodeStateClass::stateless,
        workflow_only,
    });

    nodes.push_back(NodeMetadata{
        "core.material.mask_from_image",
        1U,
        {input("source", DataKind::image)},
        {output("mask", DataKind::mask)},
        {
            enum_parameter("channel", "luminance", image_channels(), "material.mask.source"),
            real_parameter("threshold", 0.5, 0.0, 1.0, "material.mask.threshold"),
            enum_parameter("invert", "no", {"no", "yes"}, "material.mask.threshold"),
        },
        NodeStateClass::stateless,
        workflow_only,
    });

    nodes.push_back(NodeMetadata{
        "core.material.mask_image",
        1U,
        {input("mask", DataKind::mask)},
        {output("value", DataKind::image)},
        {},
        NodeStateClass::stateless,
        workflow_only,
    });

    nodes.push_back(NodeMetadata{
        "core.material.pack_masks_rgba",
        1U,
        {
            input("r", DataKind::mask),
            input("g", DataKind::mask),
            input("b", DataKind::mask),
            input("a", DataKind::mask),
        },
        {output("value", DataKind::image)},
        {},
        NodeStateClass::stateless,
        workflow_only,
    });

    // Tick-derived phase is analytically periodic rather than an accumulated
    // simulation state. It is marked stateful because tick changes its value;
    // loop validation has an explicit proof rule for this node type.
    nodes.push_back(NodeMetadata{
        "core.loop.phase",
        1U,
        {},
        {output("phase", DataKind::scalar_field)},
        {
            integer_parameter("loop_length", 32, 2, 4096, "loop.period"),
            real_parameter("phase_offset", 0.0, 0.0, 1.0, "loop.phase", MutationScale::periodic),
        },
        NodeStateClass::stateful,
        workflow_only,
    });

    nodes.push_back(NodeMetadata{
        "core.loop.wave_image",
        1U,
        {input("phase", DataKind::scalar_field)},
        {output("value", DataKind::image)},
        {
            enum_parameter("axis", "x", {"x", "y"}, "loop.wave"),
            integer_parameter("spatial_cycles", 4, 1, 64, "loop.wave"),
            integer_parameter("phase_cycles", 1, -16, 16, "loop.wave"),
            real_parameter("contrast", 1.0, 0.0, 4.0, "loop.wave"),
        },
        NodeStateClass::stateless,
        workflow_only,
    });
}

}  // namespace artminer::core
