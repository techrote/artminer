#include "core/graph.hpp"

#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace artminer::core {

// node_registry.cpp is compiled with its original registry exported under this
// implementation name; this file extends that catalog without duplicating the
// established static-node definitions.
[[nodiscard]] const NodeRegistry& builtin_node_registry_static();

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

[[nodiscard]] ParameterSpec boundary_parameter(std::string default_value, std::vector<std::string> values) {
    return enum_parameter("boundary", std::move(default_value), std::move(values), "boundary");
}

[[nodiscard]] ParameterSpec palette_parameter() {
    return enum_parameter("palette", "mono", {"mono", "ember", "cyan", "forest"}, "palette");
}

[[nodiscard]] std::vector<NodeMetadata> make_growth_nodes() {
    constexpr EvaluatorCapabilities cpu_reference{true, false};
    std::vector<NodeMetadata> nodes;

    nodes.push_back(NodeMetadata{
        "core.growth.reaction_diffusion",
        1U,
        {},
        {output("image", DataKind::image)},
        {
            real_parameter("diff_a", 0.16, 0.01, 0.50, "dynamics"),
            real_parameter("diff_b", 0.08, 0.01, 0.50, "dynamics"),
            real_parameter("feed", 0.035, 0.0, 0.10, "dynamics"),
            real_parameter("kill", 0.062, 0.0, 0.10, "dynamics"),
            real_parameter("dt", 0.80, 0.05, 1.0, "dynamics"),
            real_parameter("seed_density", 0.035, 0.001, 0.50, "initial"),
            boundary_parameter("wrap", {"wrap", "clamp"}),
            palette_parameter(),
        },
        NodeStateClass::stateful,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.growth.cellular_automaton",
        1U,
        {},
        {output("image", DataKind::image)},
        {
            integer_parameter("states", 4, 2, 16, "rule"),
            integer_parameter("birth_min", 3, 0, 8, "rule"),
            integer_parameter("birth_max", 3, 0, 8, "rule"),
            integer_parameter("survive_min", 2, 0, 8, "rule"),
            integer_parameter("survive_max", 3, 0, 8, "rule"),
            real_parameter("initial_density", 0.35, 0.0, 1.0, "initial"),
            boundary_parameter("wrap", {"wrap", "clamp"}),
            palette_parameter(),
        },
        NodeStateClass::stateful,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.growth.walkers",
        1U,
        {},
        {output("image", DataKind::image)},
        {
            integer_parameter("walkers", 96, 1, 4096, "population"),
            integer_parameter("steps_per_tick", 2, 1, 8, "motion"),
            real_parameter("turn_chance", 0.25, 0.0, 1.0, "motion"),
            real_parameter("deposit", 0.08, 0.001, 1.0, "deposition", MutationScale::logarithmic),
            real_parameter("decay", 0.005, 0.0, 0.25, "deposition"),
            boundary_parameter("wrap", {"wrap", "reflect"}),
            palette_parameter(),
        },
        NodeStateClass::stateful,
        cpu_reference,
    });

    nodes.push_back(NodeMetadata{
        "core.growth.branching",
        1U,
        {},
        {output("image", DataKind::image)},
        {
            integer_parameter("initial_branches", 4, 1, 8, "population"),
            integer_parameter("max_tips", 512, 1, 4096, "population"),
            real_parameter("turn_chance", 0.18, 0.0, 1.0, "growth"),
            real_parameter("branch_chance", 0.035, 0.0, 1.0, "growth"),
            boundary_parameter("clamp", {"clamp", "wrap"}),
            palette_parameter(),
        },
        NodeStateClass::stateful,
        cpu_reference,
    });

    return nodes;
}

}  // namespace

const NodeRegistry& builtin_node_registry() {
    static const NodeRegistry registry([] {
        std::vector<NodeMetadata> nodes = builtin_node_registry_static().nodes();
        auto growth = make_growth_nodes();
        nodes.insert(nodes.end(), std::make_move_iterator(growth.begin()), std::make_move_iterator(growth.end()));
        return nodes;
    }());
    return registry;
}

}  // namespace artminer::core
