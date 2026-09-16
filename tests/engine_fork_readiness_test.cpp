#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/graph.hpp"
#include "core/recipe.hpp"

namespace {

int g_failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

[[nodiscard]] artminer::core::ParameterSpec real_parameter(
    std::string name,
    const double default_value,
    const double minimum,
    const double maximum) {
    artminer::core::ParameterDomain domain;
    domain.real_min = minimum;
    domain.real_max = maximum;
    return artminer::core::ParameterSpec{
        std::move(name),
        artminer::core::ParameterKind::real,
        default_value,
        std::move(domain),
        artminer::core::MutationMetadata{true, artminer::core::MutationScale::linear, "sample"},
    };
}

[[nodiscard]] artminer::core::NodeRegistry make_custom_registry() {
    using namespace artminer::core;
    NodeMetadata sample{
        "example.measure.window",
        1U,
        {},
        {PortSpec{"value", DataKind::scalar_field, false, false}},
        {
            real_parameter("low", 0.2, 0.0, 1.0),
            real_parameter("high", 0.8, 0.0, 1.0),
        },
        NodeStateClass::stateless,
        EvaluatorCapabilities{false, false},
        {ParameterRelation{"low", ParameterRelationKind::less_than, "high"}},
    };
    return NodeRegistry({std::move(sample)});
}

[[nodiscard]] artminer::core::Recipe parse_fixture() {
    constexpr std::string_view text = R"AMR(amr 1
evaluator 1
seed 424242
render 32 24 reference
node sample example.measure.window 1
param sample low f64 0.2
param sample high f64 0.8
output main sample value
meta purpose "engine-only-custom-catalog"
)AMR";
    auto parsed = artminer::core::parse_recipe(text);
    expect(parsed.is_ok(), "custom-catalog fixture parses without a built-in catalog");
    if (parsed.is_error()) {
        return {};
    }
    return std::move(parsed).value();
}

void test_custom_catalog_round_trip() {
    using namespace artminer::core;
    const NodeRegistry registry = make_custom_registry();
    expect(registry.find("example.measure.window") != nullptr, "custom node is present");
    expect(registry.find("core.scalar.constant") == nullptr, "engine-only registry does not contain ArtMiner built-ins");

    const Recipe recipe = parse_fixture();
    expect(validate_recipe(recipe, registry).empty(), "generic validator accepts custom catalog recipe");

    const std::string before = semantic_fingerprint(recipe);
    const std::string canonical = serialize_recipe_canonical(recipe);
    auto reparsed = parse_recipe(canonical);
    expect(reparsed.is_ok(), "canonical custom recipe reparses");
    if (reparsed.is_error()) {
        return;
    }
    expect(validate_recipe(reparsed.value(), registry).empty(), "reparsed custom recipe validates against the same catalog");
    expect(semantic_fingerprint(reparsed.value()) == before, "custom recipe semantic fingerprint survives canonical round trip");
}

void test_relation_contract_is_generic() {
    using namespace artminer::core;
    const NodeRegistry registry = make_custom_registry();
    Recipe recipe = parse_fixture();
    if (recipe.nodes.empty() || recipe.nodes.front().parameters.size() < 2U) {
        expect(false, "relation fixture contains expected parameters");
        return;
    }

    recipe.nodes.front().parameters[1].value = 0.1;
    const auto errors = validate_recipe(recipe, registry);
    bool found_relation_error = false;
    for (const ValidationError& error : errors) {
        if (error.code == ValidationErrorCode::parameter_out_of_domain &&
            error.message.find("'low' < 'high'") != std::string::npos) {
            found_relation_error = true;
            break;
        }
    }
    expect(found_relation_error, "catalog-declared cross-parameter relation is enforced generically");
}

}  // namespace

int main() {
    test_custom_catalog_round_trip();
    test_relation_contract_is_generic();
    if (g_failures != 0) {
        std::cerr << g_failures << " engine fork-readiness test(s) failed\n";
        return 1;
    }
    std::cout << "AM-016 engine fork-readiness tests passed\n";
    return 0;
}
