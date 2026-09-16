#include "core/growth_nodes.hpp"

#include <string>
#include <utility>
#include <vector>

namespace artminer::core {
namespace {

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
    std::string group,
    const bool mutable_parameter = true) {
    ParameterDomain domain;
    domain.integer_min = minimum;
    domain.integer_max = maximum;
    return ParameterSpec{
        std::move(name), ParameterKind::integer, default_value, std::move(domain),
        MutationMetadata{
            mutable_parameter,
            mutable_parameter ? MutationScale::discrete : MutationScale::none,
            std::move(group)}};
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

[[nodiscard]] ParameterSpec tick_parameter(const i64 maximum) {
    // The requested tick is semantic state but deliberately not a mutation axis:
    // specimen mutation explores system parameters while the user explicitly
    // chooses the inspection tick. Setting it to zero is the canonical reset.
    return integer_parameter("tick", 0, 0, maximum, "simulation", false);
}

}  // namespace

void append_growth_node_metadata(std::vector<NodeMetadata>& nodes) {
    constexpr EvaluatorCapabilities cpu_reference{true, false};

    nodes.push_back(NodeMetadata{
        "core.growth.reaction_diffusion",
        1U,
        {},
        {output("value", DataKind::scalar_field)},
        {
            tick_parameter(4096),
            real_parameter("feed", 0.0367, 0.0, 0.1, "dynamics"),
            real_parameter("kill", 0.0649, 0.0, 0.1, "dynamics"),
            real_parameter("diffusion_a", 1.0, 0.0, 2.0, "dynamics"),
            real_parameter("diffusion_b", 0.5, 0.0, 2.0, "dynamics"),
            real_parameter("dt", 1.0, 0.01, 1.0, "dynamics"),
            real_parameter("seed_radius", 0.08, 0.005, 0.45, "initial"),
            real_parameter("seed_noise", 0.01, 0.0, 0.25, "initial"),
            enum_parameter("boundary", "repeat", {"repeat", "clamp"}, "boundary"),
        },
        NodeStateClass::stateful,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.growth.cellular_automaton",
        1U,
        {},
        {output("value", DataKind::scalar_field)},
        {
            tick_parameter(8192),
            integer_parameter("states", 4, 2, 16, "rule"),
            real_parameter("initial_fill", 0.28, 0.0, 1.0, "initial"),
            integer_parameter("birth_min", 3, 0, 8, "rule"),
            integer_parameter("birth_max", 3, 0, 8, "rule"),
            integer_parameter("survive_min", 2, 0, 8, "rule"),
            integer_parameter("survive_max", 3, 0, 8, "rule"),
            enum_parameter("neighbourhood", "moore", {"moore", "von_neumann"}, "rule"),
            enum_parameter("boundary", "repeat", {"repeat", "clamp"}, "boundary"),
        },
        NodeStateClass::stateful,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.growth.walkers",
        1U,
        {},
        {output("value", DataKind::scalar_field)},
        {
            tick_parameter(16384),
            integer_parameter("walkers", 48, 1, 512, "population"),
            real_parameter("deposit", 0.08, 0.001, 1.0, "deposit"),
            enum_parameter("spawn", "center", {"center", "seeded"}, "initial"),
            enum_parameter("boundary", "repeat", {"repeat", "clamp"}, "boundary"),
        },
        NodeStateClass::stateful,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.growth.branching",
        1U,
        {},
        {output("value", DataKind::scalar_field)},
        {
            tick_parameter(8192),
            integer_parameter("initial_tips", 4, 1, 16, "population"),
            integer_parameter("max_tips", 192, 1, 512, "population"),
            real_parameter("branch_probability", 0.06, 0.0, 1.0, "branching"),
            real_parameter("turn_probability", 0.25, 0.0, 1.0, "branching"),
            real_parameter("deposit", 0.10, 0.001, 1.0, "deposit"),
            enum_parameter("boundary", "clamp", {"repeat", "clamp"}, "boundary"),
        },
        NodeStateClass::stateful,
        cpu_reference,
    });
}

}  // namespace artminer::core
