#include "core/glyph_nodes.hpp"

#include <string>
#include <utility>
#include <vector>

namespace artminer::core {
namespace {

[[nodiscard]] ParameterSpec real_parameter(
    std::string name,
    const double default_value,
    const double minimum,
    const double maximum,
    std::string group) {
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
    std::string group) {
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

}  // namespace

void append_glyph_node_metadata(std::vector<NodeMetadata>& nodes) {
    constexpr EvaluatorCapabilities output_stage_only{false, false};
    nodes.push_back(NodeMetadata{
        "core.glyph.settings",
        1U,
        {},
        {},
        {
            integer_parameter("cell_width", 8, 1, 128, "glyph.layout"),
            integer_parameter("cell_height", 16, 1, 128, "glyph.layout"),
            enum_parameter(
                "glyph_set",
                "lines",
                {"ascii-density", "blocks", "lines", "sparkle"},
                "glyph.mapping"),
            enum_parameter(
                "choice_mode",
                "balanced",
                {"density", "structure", "balanced"},
                "glyph.mapping"),
            real_parameter("density_weight", 1.0, 0.0, 8.0, "glyph.mapping"),
            real_parameter("orientation_weight", 2.0, 0.0, 8.0, "glyph.mapping"),
            real_parameter("detail_weight", 0.75, 0.0, 8.0, "glyph.mapping"),
            real_parameter("corner_weight", 0.5, 0.0, 8.0, "glyph.mapping"),
            real_parameter("motion_weight", 0.0, 0.0, 8.0, "glyph.mapping"),
            enum_parameter(
                "colour_mode",
                "ansi256",
                {"monochrome", "limited", "ansi16", "ansi256", "truecolor"},
                "glyph.colour"),
            enum_parameter(
                "limited_palette",
                "cool8",
                {"mono4", "warm8", "cool8"},
                "glyph.colour"),
            enum_parameter(
                "background_mode",
                "black",
                {"black", "terminal-default"},
                "glyph.colour"),
            enum_parameter(
                "alpha_mode",
                "composite-black",
                {"composite-black", "ignore"},
                "glyph.colour"),
        },
        NodeStateClass::stateless,
        output_stage_only,
    });
}

}  // namespace artminer::core
