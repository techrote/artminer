#include "core/breeding.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

#include "core/hash.hpp"
#include "core/prng.hpp"

namespace artminer::core {
namespace {

constexpr u64 kCrossoverRootDomain = 0x43524f5353524f4fULL;  // "CROSSROO"
constexpr u64 kCrossoverPickDomain = 0x43524f5353504943ULL;  // "CROSSPIC"

[[nodiscard]] CrossoverError make_crossover_error(const CrossoverErrorCode code, std::string message) {
    return CrossoverError{code, std::move(message)};
}

[[nodiscard]] LineageError make_lineage_error(const LineageErrorCode code, std::string message) {
    return LineageError{code, std::move(message)};
}

[[nodiscard]] std::string validation_summary(const std::vector<ValidationError>& errors) {
    if (errors.empty()) {
        return {};
    }
    std::string result = errors.front().message;
    if (errors.size() > 1U) {
        result += " (and " + std::to_string(errors.size() - 1U) + " more validation error(s))";
    }
    return result;
}

[[nodiscard]] const NodeInstance* find_node(const Recipe& recipe, const std::string_view id) {
    const auto found = std::find_if(recipe.nodes.begin(), recipe.nodes.end(), [id](const NodeInstance& node) {
        return node.id == id;
    });
    return found == recipe.nodes.end() ? nullptr : &*found;
}

[[nodiscard]] NodeInstance* find_node(Recipe& recipe, const std::string_view id) {
    const auto found = std::find_if(recipe.nodes.begin(), recipe.nodes.end(), [id](const NodeInstance& node) {
        return node.id == id;
    });
    return found == recipe.nodes.end() ? nullptr : &*found;
}

[[nodiscard]] const ParameterAssignment* find_assignment(
    const NodeInstance& node,
    const std::string_view name) {
    const auto found = std::find_if(node.parameters.begin(), node.parameters.end(), [name](const ParameterAssignment& item) {
        return item.name == name;
    });
    return found == node.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] ParameterAssignment* find_assignment(NodeInstance& node, const std::string_view name) {
    const auto found = std::find_if(node.parameters.begin(), node.parameters.end(), [name](const ParameterAssignment& item) {
        return item.name == name;
    });
    return found == node.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] const ParameterSpec* find_parameter_spec(
    const NodeMetadata& metadata,
    const std::string_view name) {
    const auto found = std::find_if(metadata.parameters.begin(), metadata.parameters.end(), [name](const ParameterSpec& item) {
        return item.name == name;
    });
    return found == metadata.parameters.end() ? nullptr : &*found;
}

void erase_operation_metadata(Recipe& recipe) {
    std::erase_if(recipe.metadata, [](const RecipeMetadata& metadata) {
        return metadata.key.starts_with("artminer.mutation.") ||
            metadata.key.starts_with("artminer.crossover.") ||
            metadata.key.starts_with("artminer.topology.");
    });
}

void set_metadata(Recipe& recipe, std::string key, std::string value) {
    const auto found = std::find_if(recipe.metadata.begin(), recipe.metadata.end(), [&key](const RecipeMetadata& metadata) {
        return metadata.key == key;
    });
    if (found == recipe.metadata.end()) {
        recipe.metadata.push_back(RecipeMetadata{std::move(key), std::move(value)});
    } else {
        found->value = std::move(value);
    }
}

[[nodiscard]] const std::string* metadata_value(const Recipe& recipe, const std::string_view key) {
    const auto found = std::find_if(recipe.metadata.begin(), recipe.metadata.end(), [key](const RecipeMetadata& metadata) {
        return metadata.key == key;
    });
    return found == recipe.metadata.end() ? nullptr : &found->value;
}

[[nodiscard]] bool parse_u64(const std::string_view text, u64& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_u32(const std::string_view text, u32& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_double(const std::string_view text, double& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool render_settings_equal(const RenderSettings& left, const RenderSettings& right) {
    return left.width == right.width && left.height == right.height && left.quality == right.quality;
}

using EdgeKey = std::tuple<std::string, std::string, std::string, std::string>;
using OutputKey = std::tuple<std::string, std::string, std::string>;

[[nodiscard]] std::vector<EdgeKey> sorted_edges(const Recipe& recipe) {
    std::vector<EdgeKey> result;
    result.reserve(recipe.edges.size());
    for (const auto& edge : recipe.edges) {
        result.emplace_back(edge.from_node, edge.from_port, edge.to_node, edge.to_port);
    }
    std::sort(result.begin(), result.end());
    return result;
}

[[nodiscard]] std::vector<OutputKey> sorted_outputs(const Recipe& recipe) {
    std::vector<OutputKey> result;
    result.reserve(recipe.outputs.size());
    for (const auto& output : recipe.outputs) {
        result.emplace_back(output.name, output.node_id, output.port);
    }
    std::sort(result.begin(), result.end());
    return result;
}

[[nodiscard]] bool shared_topology_compatible(const Recipe& a, const Recipe& b) {
    if (a.schema_version != b.schema_version || a.evaluator_version != b.evaluator_version ||
        !render_settings_equal(a.render, b.render) || a.nodes.size() != b.nodes.size() ||
        sorted_edges(a) != sorted_edges(b) || sorted_outputs(a) != sorted_outputs(b)) {
        return false;
    }

    for (const auto& node_a : a.nodes) {
        const NodeInstance* node_b = find_node(b, node_a.id);
        if (node_b == nullptr || node_a.type_id != node_b->type_id ||
            node_a.semantic_version != node_b->semantic_version ||
            node_a.parameters.size() != node_b->parameters.size()) {
            return false;
        }
        for (const auto& parameter_a : node_a.parameters) {
            const auto* parameter_b = find_assignment(*node_b, parameter_a.name);
            if (parameter_b == nullptr || parameter_a.value.index() != parameter_b->value.index()) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] std::string crossover_domain_text(
    const std::string_view parent_a,
    const std::string_view parent_b,
    const u32 operator_version) {
    std::string text = "ArtMiner.Crossover.v1\nA=";
    text.append(parent_a);
    text += "\nB=";
    text.append(parent_b);
    text += "\noperator=" + std::to_string(operator_version) + "\n";
    return text;
}

[[nodiscard]] bool choose_parent_b(
    const u64 operation_root,
    const std::string_view node_id,
    const ParameterSpec& spec) {
    std::string key;
    key.reserve(node_id.size() + spec.name.size() + spec.mutation.group.size() + 16U);
    key.append(node_id);
    if (!spec.mutation.group.empty()) {
        key += "/group/";
        key += spec.mutation.group;
    } else {
        key += "/parameter/";
        key += spec.name;
    }
    const u64 domain = fnv1a64(key) ^ kCrossoverPickDomain;
    const u64 local = derive_seed(operation_root, domain);
    return (local & 1ULL) != 0ULL;
}

[[nodiscard]] Result<ParameterLocks, LineageError> parse_locks(const std::string_view text) {
    ParameterLocks locks;
    std::size_t position = 0U;
    while (position < text.size()) {
        const std::size_t separator = text.find(';', position);
        const std::size_t end = separator == std::string_view::npos ? text.size() : separator;
        const std::string_view token = text.substr(position, end - position);
        if (token.size() < 5U || token[1] != '/') {
            return Result<ParameterLocks, LineageError>::failure(
                make_lineage_error(LineageErrorCode::malformed_lineage, "lineage lock encoding is malformed"));
        }
        const std::size_t slash = token.find('/', 2U);
        if (slash == std::string_view::npos || slash == 2U || slash + 1U >= token.size()) {
            return Result<ParameterLocks, LineageError>::failure(
                make_lineage_error(LineageErrorCode::malformed_lineage, "lineage lock key is malformed"));
        }
        const std::string node(token.substr(2U, slash - 2U));
        const std::string name(token.substr(slash + 1U));
        if (token[0] == 'p') {
            locks.set_parameter(node, name, true);
        } else if (token[0] == 'g') {
            locks.set_group(node, name, true);
        } else {
            return Result<ParameterLocks, LineageError>::failure(
                make_lineage_error(LineageErrorCode::malformed_lineage, "lineage lock kind is unknown"));
        }
        if (separator == std::string_view::npos) {
            break;
        }
        position = separator + 1U;
    }
    return Result<ParameterLocks, LineageError>::success(std::move(locks));
}

using FlatMap = std::map<std::string, std::string>;

[[nodiscard]] FlatMap flatten_semantics(const Recipe& recipe) {
    FlatMap values;
    values["schema"] = std::to_string(recipe.schema_version);
    values["evaluator"] = std::to_string(recipe.evaluator_version);
    values["seed"] = std::to_string(recipe.root_seed);
    values["render.width"] = std::to_string(recipe.render.width);
    values["render.height"] = std::to_string(recipe.render.height);
    values["render.quality"] = recipe.render.quality;

    for (const auto& node : recipe.nodes) {
        const std::string prefix = "node." + node.id;
        values[prefix + ".type"] = node.type_id;
        values[prefix + ".version"] = std::to_string(node.semantic_version);
        for (const auto& parameter : node.parameters) {
            values[prefix + ".param." + parameter.name] = format_parameter_value(parameter.value);
        }
    }
    for (const auto& edge : recipe.edges) {
        const std::string key =
            "edge." + edge.from_node + "." + edge.from_port + "->" + edge.to_node + "." + edge.to_port;
        values[key] = "present";
    }
    for (const auto& output : recipe.outputs) {
        values["output." + output.name] = output.node_id + "." + output.port;
    }
    return values;
}

[[nodiscard]] FlatMap flatten_metadata(const Recipe& recipe) {
    std::map<std::string, std::vector<std::string>> grouped;
    for (const auto& metadata : recipe.metadata) {
        grouped[metadata.key].push_back(metadata.value);
    }
    FlatMap values;
    for (auto& [key, entries] : grouped) {
        std::sort(entries.begin(), entries.end());
        std::string joined;
        for (std::size_t index = 0U; index < entries.size(); ++index) {
            if (index != 0U) {
                joined += " | ";
            }
            joined += entries[index];
        }
        values["meta." + key] = std::move(joined);
    }
    return values;
}

[[nodiscard]] std::vector<RecipeDiffEntry> diff_flat(const FlatMap& before, const FlatMap& after) {
    std::set<std::string> keys;
    for (const auto& [key, value] : before) {
        (void)value;
        keys.insert(key);
    }
    for (const auto& [key, value] : after) {
        (void)value;
        keys.insert(key);
    }

    std::vector<RecipeDiffEntry> result;
    for (const auto& key : keys) {
        const auto left = before.find(key);
        const auto right = after.find(key);
        const std::string left_value = left == before.end() ? "<absent>" : left->second;
        const std::string right_value = right == after.end() ? "<absent>" : right->second;
        if (left_value != right_value) {
            result.push_back(RecipeDiffEntry{key, left_value, right_value});
        }
    }
    return result;
}

}  // namespace

Result<Recipe, CrossoverError> crossover_recipes(
    const Recipe& parent_a,
    const Recipe& parent_b,
    const u64 crossover_seed,
    const u32 operator_version,
    const ParameterLocks& locks,
    const NodeRegistry& registry) {
    if (operator_version != kCrossoverOperatorVersion) {
        return Result<Recipe, CrossoverError>::failure(
            make_crossover_error(CrossoverErrorCode::unsupported_operator_version, "unsupported crossover operator version"));
    }
    const auto errors_a = validate_recipe(parent_a, registry);
    if (!errors_a.empty()) {
        return Result<Recipe, CrossoverError>::failure(
            make_crossover_error(CrossoverErrorCode::invalid_parent, "parent A: " + validation_summary(errors_a)));
    }
    const auto errors_b = validate_recipe(parent_b, registry);
    if (!errors_b.empty()) {
        return Result<Recipe, CrossoverError>::failure(
            make_crossover_error(CrossoverErrorCode::invalid_parent, "parent B: " + validation_summary(errors_b)));
    }
    if (!shared_topology_compatible(parent_a, parent_b)) {
        return Result<Recipe, CrossoverError>::failure(make_crossover_error(
            CrossoverErrorCode::incompatible_parents,
            "crossover v1 requires identical schema/evaluator/render settings, node IDs/types/versions, parameter layout, edges, and outputs"));
    }

    const std::string fingerprint_a = semantic_fingerprint(parent_a);
    const std::string fingerprint_b = semantic_fingerprint(parent_b);
    const std::string domain_text = crossover_domain_text(fingerprint_a, fingerprint_b, operator_version);
    const u64 operation_root = derive_seed(crossover_seed, fnv1a64(domain_text));

    Recipe child = parent_a;
    child.root_seed = derive_seed(operation_root, kCrossoverRootDomain);

    for (auto& child_node : child.nodes) {
        const NodeMetadata* metadata = registry.find(child_node.type_id);
        const NodeInstance* node_b = find_node(parent_b, child_node.id);
        if (metadata == nullptr || node_b == nullptr) {
            return Result<Recipe, CrossoverError>::failure(
                make_crossover_error(CrossoverErrorCode::incompatible_parents, "compatible node metadata disappeared during crossover"));
        }
        for (auto& child_parameter : child_node.parameters) {
            const ParameterSpec* spec = find_parameter_spec(*metadata, child_parameter.name);
            const ParameterAssignment* value_b = find_assignment(*node_b, child_parameter.name);
            if (spec == nullptr || value_b == nullptr) {
                return Result<Recipe, CrossoverError>::failure(
                    make_crossover_error(CrossoverErrorCode::incompatible_parents, "compatible parameter metadata disappeared during crossover"));
            }
            if (!spec->mutation.mutable_parameter || spec->mutation.scale == MutationScale::none ||
                locks.locked(child_node.id, *spec)) {
                continue;
            }
            if (choose_parent_b(operation_root, child_node.id, *spec)) {
                child_parameter.value = value_b->value;
            }
        }
    }

    const auto child_errors = validate_recipe(child, registry);
    if (!child_errors.empty()) {
        return Result<Recipe, CrossoverError>::failure(
            make_crossover_error(CrossoverErrorCode::invalid_child, validation_summary(child_errors)));
    }

    erase_operation_metadata(child);
    set_metadata(child, "artminer.crossover.parent_a", fingerprint_a);
    set_metadata(child, "artminer.crossover.parent_b", fingerprint_b);
    set_metadata(child, "artminer.crossover.seed", std::to_string(crossover_seed));
    set_metadata(child, "artminer.crossover.operator", std::to_string(operator_version));
    set_metadata(child, "artminer.crossover.locks", locks.serialize_canonical());
    return Result<Recipe, CrossoverError>::success(std::move(child));
}

Result<LineageRecord, LineageError> lineage_record_from_recipe(const Recipe& recipe) {
    const std::string child = semantic_fingerprint(recipe);

    if (const std::string* parent = metadata_value(recipe, "artminer.topology.parent"); parent != nullptr) {
        const std::string* seed_text = metadata_value(recipe, "artminer.topology.seed");
        const std::string* operator_text = metadata_value(recipe, "artminer.topology.operator");
        const std::string* strength_text = metadata_value(recipe, "artminer.topology.strength");
        const std::string* budget_text = metadata_value(recipe, "artminer.topology.budget");
        const std::string* structural_locks = metadata_value(recipe, "artminer.topology.locks");
        if (seed_text == nullptr || operator_text == nullptr || strength_text == nullptr ||
            budget_text == nullptr || structural_locks == nullptr) {
            return Result<LineageRecord, LineageError>::failure(
                make_lineage_error(LineageErrorCode::malformed_lineage, "topology lineage metadata is incomplete"));
        }
        u64 seed = 0U;
        u32 operator_version = 0U;
        u32 budget = 0U;
        double strength = 0.0;
        if (!parse_u64(*seed_text, seed) || !parse_u32(*operator_text, operator_version) ||
            !parse_u32(*budget_text, budget) || !parse_double(*strength_text, strength) ||
            strength < 0.0 || strength > 1.0 || budget == 0U || budget > kMaximumTopologyMutationBudget) {
            return Result<LineageRecord, LineageError>::failure(
                make_lineage_error(LineageErrorCode::malformed_lineage, "topology lineage numeric metadata is malformed"));
        }
        auto parsed_structural = parse_structural_locks(*structural_locks);
        if (parsed_structural.is_error()) {
            return Result<LineageRecord, LineageError>::failure(
                make_lineage_error(LineageErrorCode::malformed_lineage, parsed_structural.error().message));
        }
        LineageRecord record;
        record.kind = LineageOperationKind::topology_mutation;
        record.child_fingerprint = child;
        record.parent_a_fingerprint = *parent;
        record.operator_version = operator_version;
        record.operation_seed = seed;
        record.mutation_strength = strength;
        record.topology_budget = budget;
        record.structural_locks = *structural_locks;
        return Result<LineageRecord, LineageError>::success(std::move(record));
    }

    if (const std::string* parent_a = metadata_value(recipe, "artminer.crossover.parent_a"); parent_a != nullptr) {
        const std::string* parent_b = metadata_value(recipe, "artminer.crossover.parent_b");
        const std::string* seed_text = metadata_value(recipe, "artminer.crossover.seed");
        const std::string* operator_text = metadata_value(recipe, "artminer.crossover.operator");
        if (parent_b == nullptr || seed_text == nullptr || operator_text == nullptr) {
            return Result<LineageRecord, LineageError>::failure(
                make_lineage_error(LineageErrorCode::malformed_lineage, "crossover lineage metadata is incomplete"));
        }
        u64 seed = 0U;
        u32 operator_version = 0U;
        if (!parse_u64(*seed_text, seed) || !parse_u32(*operator_text, operator_version)) {
            return Result<LineageRecord, LineageError>::failure(
                make_lineage_error(LineageErrorCode::malformed_lineage, "crossover lineage numeric metadata is malformed"));
        }
        const std::string* locks = metadata_value(recipe, "artminer.crossover.locks");
        return Result<LineageRecord, LineageError>::success(LineageRecord{
            LineageOperationKind::crossover,
            child,
            *parent_a,
            *parent_b,
            operator_version,
            seed,
            std::nullopt,
            locks == nullptr ? std::string{} : *locks,
            std::nullopt,
            {}});
    }

    if (const std::string* kind = metadata_value(recipe, "artminer.mutation.kind"); kind != nullptr) {
        const std::string* parent = metadata_value(recipe, "artminer.mutation.parent");
        const std::string* seed_text = metadata_value(recipe, "artminer.mutation.seed");
        const std::string* operator_text = metadata_value(recipe, "artminer.mutation.operator");
        if (parent == nullptr || seed_text == nullptr || operator_text == nullptr) {
            return Result<LineageRecord, LineageError>::failure(
                make_lineage_error(LineageErrorCode::malformed_lineage, "mutation lineage metadata is incomplete"));
        }
        u64 seed = 0U;
        u32 operator_version = 0U;
        if (!parse_u64(*seed_text, seed) || !parse_u32(*operator_text, operator_version)) {
            return Result<LineageRecord, LineageError>::failure(
                make_lineage_error(LineageErrorCode::malformed_lineage, "mutation lineage numeric metadata is malformed"));
        }
        const std::string* locks = metadata_value(recipe, "artminer.mutation.locks");
        if (*kind == "parameter") {
            const std::string* strength_text = metadata_value(recipe, "artminer.mutation.strength");
            double strength = 0.0;
            if (strength_text == nullptr || !parse_double(*strength_text, strength)) {
                return Result<LineageRecord, LineageError>::failure(
                    make_lineage_error(LineageErrorCode::malformed_lineage, "parameter-mutation strength metadata is malformed"));
            }
            return Result<LineageRecord, LineageError>::success(LineageRecord{
                LineageOperationKind::parameter_mutation,
                child,
                *parent,
                std::nullopt,
                operator_version,
                seed,
                strength,
                locks == nullptr ? std::string{} : *locks,
                std::nullopt,
                {}});
        }
        if (*kind == "seed-only") {
            return Result<LineageRecord, LineageError>::success(LineageRecord{
                LineageOperationKind::seed_variant,
                child,
                *parent,
                std::nullopt,
                operator_version,
                seed,
                std::nullopt,
                {},
                std::nullopt,
                {}});
        }
        return Result<LineageRecord, LineageError>::failure(
            make_lineage_error(LineageErrorCode::malformed_lineage, "mutation lineage kind is unknown"));
    }

    return Result<LineageRecord, LineageError>::failure(
        make_lineage_error(LineageErrorCode::no_lineage, "recipe has no ArtMiner lineage metadata"));
}

Result<Recipe, LineageError> replay_lineage_record(
    const LineageRecord& record,
    const Recipe& parent_a,
    const Recipe* parent_b,
    const NodeRegistry& registry) {
    if (semantic_fingerprint(parent_a) != record.parent_a_fingerprint) {
        return Result<Recipe, LineageError>::failure(
            make_lineage_error(LineageErrorCode::parent_mismatch, "parent A semantic fingerprint does not match lineage record"));
    }

    if (record.kind == LineageOperationKind::topology_mutation) {
        if (!record.mutation_strength.has_value() || !record.topology_budget.has_value()) {
            return Result<Recipe, LineageError>::failure(
                make_lineage_error(LineageErrorCode::malformed_lineage, "topology lineage is missing strength or budget"));
        }
        auto structural = parse_structural_locks(record.structural_locks);
        if (structural.is_error()) {
            return Result<Recipe, LineageError>::failure(
                make_lineage_error(LineageErrorCode::malformed_lineage, structural.error().message));
        }
        auto mutated = mutate_recipe_topology(
            parent_a,
            record.operation_seed,
            record.operator_version,
            *record.mutation_strength,
            *record.topology_budget,
            structural.value(),
            registry);
        if (mutated.is_error()) {
            return Result<Recipe, LineageError>::failure(
                make_lineage_error(LineageErrorCode::replay_failed, mutated.error().message));
        }
        Recipe replayed = std::move(mutated).value().recipe;
        if (semantic_fingerprint(replayed) != record.child_fingerprint) {
            return Result<Recipe, LineageError>::failure(
                make_lineage_error(LineageErrorCode::replay_failed, "replayed child semantic fingerprint differs from lineage record"));
        }
        return Result<Recipe, LineageError>::success(std::move(replayed));
    }

    auto parsed_locks = parse_locks(record.locks);
    if (parsed_locks.is_error()) {
        return Result<Recipe, LineageError>::failure(parsed_locks.error());
    }

    Recipe replayed;
    if (record.kind == LineageOperationKind::crossover) {
        if (parent_b == nullptr || !record.parent_b_fingerprint.has_value() ||
            semantic_fingerprint(*parent_b) != *record.parent_b_fingerprint) {
            return Result<Recipe, LineageError>::failure(
                make_lineage_error(LineageErrorCode::parent_mismatch, "parent B is missing or does not match lineage record"));
        }
        auto crossed = crossover_recipes(
            parent_a,
            *parent_b,
            record.operation_seed,
            record.operator_version,
            parsed_locks.value(),
            registry);
        if (crossed.is_error()) {
            return Result<Recipe, LineageError>::failure(
                make_lineage_error(LineageErrorCode::replay_failed, crossed.error().message));
        }
        replayed = std::move(crossed).value();
    } else if (record.kind == LineageOperationKind::parameter_mutation) {
        if (!record.mutation_strength.has_value()) {
            return Result<Recipe, LineageError>::failure(
                make_lineage_error(LineageErrorCode::malformed_lineage, "parameter-mutation lineage has no strength"));
        }
        auto mutated = mutate_recipe_parameters(
            parent_a,
            record.operation_seed,
            record.operator_version,
            *record.mutation_strength,
            parsed_locks.value(),
            registry);
        if (mutated.is_error()) {
            return Result<Recipe, LineageError>::failure(
                make_lineage_error(LineageErrorCode::replay_failed, mutated.error().message));
        }
        replayed = std::move(mutated).value();
    } else if (record.kind == LineageOperationKind::seed_variant) {
        auto varied = make_seed_variant(parent_a, record.operation_seed, record.operator_version);
        if (varied.is_error()) {
            return Result<Recipe, LineageError>::failure(
                make_lineage_error(LineageErrorCode::replay_failed, varied.error().message));
        }
        replayed = std::move(varied).value();
    } else {
        return Result<Recipe, LineageError>::failure(
            make_lineage_error(LineageErrorCode::malformed_lineage, "unknown lineage operation kind"));
    }

    if (semantic_fingerprint(replayed) != record.child_fingerprint) {
        return Result<Recipe, LineageError>::failure(
            make_lineage_error(LineageErrorCode::replay_failed, "replayed child semantic fingerprint differs from lineage record"));
    }
    return Result<Recipe, LineageError>::success(std::move(replayed));
}

RecipeDiff diff_recipes(const Recipe& before, const Recipe& after) {
    return RecipeDiff{
        diff_flat(flatten_semantics(before), flatten_semantics(after)),
        diff_flat(flatten_metadata(before), flatten_metadata(after))};
}

std::string format_recipe_diff(const RecipeDiff& diff) {
    std::ostringstream output;
    output << "semantic:\n";
    if (diff.semantic.empty()) {
        output << "  (none)\n";
    } else {
        for (const auto& entry : diff.semantic) {
            output << "  " << entry.path << ": " << entry.before << " -> " << entry.after << '\n';
        }
    }
    output << "provenance:\n";
    if (diff.provenance.empty()) {
        output << "  (none)\n";
    } else {
        for (const auto& entry : diff.provenance) {
            output << "  " << entry.path << ": " << entry.before << " -> " << entry.after << '\n';
        }
    }
    return output.str();
}

}  // namespace artminer::core
