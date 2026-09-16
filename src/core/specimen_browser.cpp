#include "core/specimen_browser.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <sstream>
#include <type_traits>

#include "core/hash.hpp"
#include "core/prng.hpp"

namespace artminer::core {
namespace {

constexpr u64 kMutationSequenceDomain = 0x4d55544154494f4eULL;  // "MUTATION"
constexpr u64 kSeedVariantDomain = 0x5345454456415249ULL;      // "SEEDVARI"
constexpr u64 kGridMutationDomain = 0x475249444d555441ULL;      // "GRIDMUTA"
constexpr u64 kGridSeedDomain = 0x4752494453454544ULL;          // "GRIDSEED"

[[nodiscard]] MutationError make_error(const MutationErrorCode code, std::string message) {
    return MutationError{code, std::move(message)};
}

[[nodiscard]] const NodeInstance* find_node(const Recipe& recipe, const std::string_view node_id) {
    const auto found = std::find_if(recipe.nodes.begin(), recipe.nodes.end(), [node_id](const NodeInstance& node) {
        return node.id == node_id;
    });
    return found == recipe.nodes.end() ? nullptr : &*found;
}

[[nodiscard]] NodeInstance* find_node(Recipe& recipe, const std::string_view node_id) {
    const auto found = std::find_if(recipe.nodes.begin(), recipe.nodes.end(), [node_id](const NodeInstance& node) {
        return node.id == node_id;
    });
    return found == recipe.nodes.end() ? nullptr : &*found;
}

[[nodiscard]] const ParameterAssignment* find_assignment(
    const NodeInstance& node,
    const std::string_view parameter_name) {
    const auto found = std::find_if(
        node.parameters.begin(),
        node.parameters.end(),
        [parameter_name](const ParameterAssignment& assignment) { return assignment.name == parameter_name; });
    return found == node.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] ParameterAssignment* find_assignment(
    NodeInstance& node,
    const std::string_view parameter_name) {
    const auto found = std::find_if(
        node.parameters.begin(),
        node.parameters.end(),
        [parameter_name](const ParameterAssignment& assignment) { return assignment.name == parameter_name; });
    return found == node.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] const ParameterSpec* find_parameter_spec(
    const NodeMetadata& metadata,
    const std::string_view parameter_name) {
    const auto found = std::find_if(
        metadata.parameters.begin(),
        metadata.parameters.end(),
        [parameter_name](const ParameterSpec& spec) { return spec.name == parameter_name; });
    return found == metadata.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] double unit_interval(Pcg32& rng) noexcept {
    constexpr double denominator = 4294967296.0;
    return static_cast<double>(rng.next_u32()) / denominator;
}

[[nodiscard]] double signed_unit(Pcg32& rng) noexcept {
    return unit_interval(rng) * 2.0 - 1.0;
}

[[nodiscard]] double stable_round(const double value) noexcept {
    if (!std::isfinite(value)) {
        return value;
    }
    constexpr double scale = 1000000000.0;
    return std::round(value * scale) / scale;
}

[[nodiscard]] double clamp_real(const double value, const double minimum, const double maximum) noexcept {
    return (std::max)(minimum, (std::min)(maximum, value));
}

[[nodiscard]] double wrap_real(const double value, const double minimum, const double maximum) noexcept {
    const double width = maximum - minimum;
    if (!(width > 0.0) || !std::isfinite(width)) {
        return clamp_real(value, minimum, maximum);
    }
    double wrapped = std::fmod(value - minimum, width);
    if (wrapped < 0.0) {
        wrapped += width;
    }
    return minimum + wrapped;
}

[[nodiscard]] std::string parameter_domain_key(
    const std::string_view node_id,
    const std::string_view parameter_name) {
    std::string key;
    key.reserve(node_id.size() + parameter_name.size() + 1U);
    key.append(node_id);
    key.push_back('.');
    key.append(parameter_name);
    return key;
}

void set_metadata(Recipe& recipe, std::string key, std::string value) {
    const auto found = std::find_if(
        recipe.metadata.begin(),
        recipe.metadata.end(),
        [&key](const RecipeMetadata& metadata) { return metadata.key == key; });
    if (found != recipe.metadata.end()) {
        found->value = std::move(value);
        return;
    }
    recipe.metadata.push_back(RecipeMetadata{std::move(key), std::move(value)});
}

[[nodiscard]] std::string format_double(const double value) {
    char buffer[96]{};
    const auto converted = std::to_chars(
        std::begin(buffer),
        std::end(buffer),
        value,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10);
    if (converted.ec == std::errc{}) {
        return std::string(buffer, converted.ptr);
    }
    std::ostringstream stream;
    stream.precision(std::numeric_limits<double>::max_digits10);
    stream << value;
    return stream.str();
}

[[nodiscard]] bool value_matches_kind(const ParameterValue& value, const ParameterKind kind) noexcept {
    switch (kind) {
    case ParameterKind::integer:
        return std::holds_alternative<i64>(value);
    case ParameterKind::real:
        return std::holds_alternative<double>(value);
    case ParameterKind::boolean:
        return std::holds_alternative<bool>(value);
    case ParameterKind::enumeration:
        return std::holds_alternative<std::string>(value);
    }
    return false;
}

[[nodiscard]] Result<ParameterValue, MutationError> parse_parameter_text(
    const ParameterSpec& spec,
    const std::string_view text) {
    switch (spec.kind) {
    case ParameterKind::integer: {
        i64 value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
            return Result<ParameterValue, MutationError>::failure(
                make_error(MutationErrorCode::invalid_parameter_value, "invalid integer parameter text"));
        }
        if ((spec.domain.integer_min.has_value() && value < *spec.domain.integer_min) ||
            (spec.domain.integer_max.has_value() && value > *spec.domain.integer_max)) {
            return Result<ParameterValue, MutationError>::failure(
                make_error(MutationErrorCode::invalid_parameter_value, "integer parameter is outside its legal domain"));
        }
        return Result<ParameterValue, MutationError>::success(ParameterValue{value});
    }
    case ParameterKind::real: {
        double value = 0.0;
        const auto parsed = std::from_chars(
            text.data(),
            text.data() + text.size(),
            value,
            std::chars_format::general);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value)) {
            return Result<ParameterValue, MutationError>::failure(
                make_error(MutationErrorCode::invalid_parameter_value, "invalid finite real parameter text"));
        }
        if ((spec.domain.real_min.has_value() && value < *spec.domain.real_min) ||
            (spec.domain.real_max.has_value() && value > *spec.domain.real_max)) {
            return Result<ParameterValue, MutationError>::failure(
                make_error(MutationErrorCode::invalid_parameter_value, "real parameter is outside its legal domain"));
        }
        return Result<ParameterValue, MutationError>::success(ParameterValue{value});
    }
    case ParameterKind::boolean:
        if (text == "true" || text == "1") {
            return Result<ParameterValue, MutationError>::success(ParameterValue{true});
        }
        if (text == "false" || text == "0") {
            return Result<ParameterValue, MutationError>::success(ParameterValue{false});
        }
        return Result<ParameterValue, MutationError>::failure(
            make_error(MutationErrorCode::invalid_parameter_value, "boolean parameters accept true/false or 1/0"));
    case ParameterKind::enumeration: {
        const std::string candidate(text);
        if (std::find(spec.domain.enum_values.begin(), spec.domain.enum_values.end(), candidate) ==
            spec.domain.enum_values.end()) {
            return Result<ParameterValue, MutationError>::failure(
                make_error(MutationErrorCode::invalid_parameter_value, "enumeration value is not in the declared domain"));
        }
        return Result<ParameterValue, MutationError>::success(ParameterValue{candidate});
    }
    }
    return Result<ParameterValue, MutationError>::failure(
        make_error(MutationErrorCode::invalid_parameter_value, "unsupported parameter kind"));
}

[[nodiscard]] ParameterValue mutate_value(
    const ParameterSpec& spec,
    const ParameterValue& current,
    const double strength,
    Pcg32& rng) {
    if (strength <= 0.0 || !spec.mutation.mutable_parameter || spec.mutation.scale == MutationScale::none) {
        return current;
    }

    switch (spec.kind) {
    case ParameterKind::integer: {
        if (!spec.domain.integer_min.has_value() || !spec.domain.integer_max.has_value()) {
            return current;
        }
        const i64 minimum = *spec.domain.integer_min;
        const i64 maximum = *spec.domain.integer_max;
        const i64 span = maximum - minimum;
        if (span <= 0) {
            return current;
        }
        const i64 current_value = std::get<i64>(current);
        const double scaled = static_cast<double>(span) * 0.5 * strength;
        const i64 max_step = (std::max)(i64{1}, static_cast<i64>(std::llround(scaled)));
        const u64 width = static_cast<u64>(max_step) * 2ULL + 1ULL;
        i64 delta = static_cast<i64>(static_cast<u64>(rng.next_u32()) % width) - max_step;
        if (delta == 0) {
            delta = (rng.next_u32() & 1U) == 0U ? -1 : 1;
        }
        i64 candidate = (std::max)(minimum, (std::min)(maximum, current_value + delta));
        if (candidate == current_value) {
            candidate = (std::max)(minimum, (std::min)(maximum, current_value - delta));
        }
        return ParameterValue{candidate};
    }
    case ParameterKind::real: {
        if (!spec.domain.real_min.has_value() || !spec.domain.real_max.has_value()) {
            return current;
        }
        const double minimum = *spec.domain.real_min;
        const double maximum = *spec.domain.real_max;
        const double current_value = std::get<double>(current);
        if (!(maximum > minimum)) {
            return current;
        }

        double candidate = current_value;
        if (spec.mutation.scale == MutationScale::logarithmic && minimum > 0.0 && current_value > 0.0) {
            const double log_min = std::log(minimum);
            const double log_max = std::log(maximum);
            const double log_current = std::log(clamp_real(current_value, minimum, maximum));
            const double delta = signed_unit(rng) * 0.5 * strength * (log_max - log_min);
            candidate = std::exp(clamp_real(log_current + delta, log_min, log_max));
        } else {
            const double delta = signed_unit(rng) * 0.5 * strength * (maximum - minimum);
            candidate = current_value + delta;
            if (spec.mutation.scale == MutationScale::periodic) {
                candidate = wrap_real(candidate, minimum, maximum);
            } else {
                candidate = clamp_real(candidate, minimum, maximum);
            }
        }
        return ParameterValue{stable_round(candidate)};
    }
    case ParameterKind::boolean: {
        const bool value = std::get<bool>(current);
        return unit_interval(rng) < strength ? ParameterValue{!value} : current;
    }
    case ParameterKind::enumeration: {
        const auto& values = spec.domain.enum_values;
        if (values.size() <= 1U || unit_interval(rng) >= strength) {
            return current;
        }
        const std::string& current_value = std::get<std::string>(current);
        const auto found = std::find(values.begin(), values.end(), current_value);
        if (found == values.end()) {
            return current;
        }
        const std::size_t current_index = static_cast<std::size_t>(std::distance(values.begin(), found));
        const std::size_t offset = 1U + static_cast<std::size_t>(rng.next_u32() % static_cast<u32>(values.size() - 1U));
        return ParameterValue{values[(current_index + offset) % values.size()]};
    }
    }
    return current;
}

[[nodiscard]] std::string validation_summary(const std::vector<ValidationError>& errors) {
    if (errors.empty()) {
        return {};
    }
    std::string summary = errors.front().message;
    if (errors.size() > 1U) {
        summary += " (and " + std::to_string(errors.size() - 1U) + " more validation error(s))";
    }
    return summary;
}

}  // namespace

bool ParameterLocks::parameter_locked(const std::string_view node_id, const std::string_view parameter) const {
    return parameters_.contains(LockKey{std::string(node_id), std::string(parameter)});
}

bool ParameterLocks::group_locked(const std::string_view node_id, const std::string_view group) const {
    if (group.empty()) {
        return false;
    }
    return groups_.contains(LockKey{std::string(node_id), std::string(group)});
}

bool ParameterLocks::locked(const std::string_view node_id, const ParameterSpec& parameter) const {
    return parameter_locked(node_id, parameter.name) || group_locked(node_id, parameter.mutation.group);
}

void ParameterLocks::set_parameter(std::string node_id, std::string parameter, const bool locked_value) {
    LockKey key{std::move(node_id), std::move(parameter)};
    if (locked_value) {
        parameters_.insert(std::move(key));
    } else {
        parameters_.erase(key);
    }
}

void ParameterLocks::set_group(std::string node_id, std::string group, const bool locked_value) {
    if (group.empty()) {
        return;
    }
    LockKey key{std::move(node_id), std::move(group)};
    if (locked_value) {
        groups_.insert(std::move(key));
    } else {
        groups_.erase(key);
    }
}

void ParameterLocks::toggle_parameter(const std::string_view node_id, const std::string_view parameter) {
    const bool was_locked = parameter_locked(node_id, parameter);
    set_parameter(std::string(node_id), std::string(parameter), !was_locked);
}

void ParameterLocks::toggle_group(const std::string_view node_id, const std::string_view group) {
    if (group.empty()) {
        return;
    }
    const bool was_locked = group_locked(node_id, group);
    set_group(std::string(node_id), std::string(group), !was_locked);
}

void ParameterLocks::clear() noexcept {
    parameters_.clear();
    groups_.clear();
}

Result<Recipe, MutationError> mutate_recipe_parameters(
    const Recipe& parent,
    const u64 mutation_seed,
    const u32 operator_version,
    const double strength,
    const ParameterLocks& locks,
    const NodeRegistry& registry) {
    if (operator_version != kParameterMutationOperatorVersion) {
        return Result<Recipe, MutationError>::failure(
            make_error(MutationErrorCode::unsupported_operator_version, "unsupported parameter-mutation operator version"));
    }
    if (!std::isfinite(strength) || strength < 0.0 || strength > 1.0) {
        return Result<Recipe, MutationError>::failure(
            make_error(MutationErrorCode::invalid_strength, "mutation strength must be finite and in [0, 1]"));
    }
    const auto parent_errors = validate_recipe(parent, registry);
    if (!parent_errors.empty()) {
        return Result<Recipe, MutationError>::failure(
            make_error(MutationErrorCode::invalid_parent, validation_summary(parent_errors)));
    }

    Recipe child = parent;
    const std::string parent_fingerprint = semantic_fingerprint(parent);
    const u64 parent_domain = fnv1a64(parent_fingerprint);
    const u64 root = derive_seed(mutation_seed, parent_domain ^ (static_cast<u64>(operator_version) << 32U));

    for (auto& node : child.nodes) {
        const NodeMetadata* metadata = registry.find(node.type_id);
        if (metadata == nullptr) {
            return Result<Recipe, MutationError>::failure(
                make_error(MutationErrorCode::unknown_node, "mutation encountered an unknown node type"));
        }
        for (auto& assignment : node.parameters) {
            const ParameterSpec* spec = find_parameter_spec(*metadata, assignment.name);
            if (spec == nullptr) {
                return Result<Recipe, MutationError>::failure(
                    make_error(MutationErrorCode::unknown_parameter, "mutation encountered an unknown parameter"));
            }
            if (!value_matches_kind(assignment.value, spec->kind) || locks.locked(node.id, *spec) ||
                !spec->mutation.mutable_parameter || spec->mutation.scale == MutationScale::none) {
                continue;
            }

            const std::string key = parameter_domain_key(node.id, assignment.name);
            const u64 local_domain = fnv1a64(key) ^ kMutationSequenceDomain;
            const u64 local_seed = derive_seed(root, local_domain);
            const u64 sequence = derive_seed(root, splitmix64(local_domain));
            Pcg32 rng(local_seed, sequence);
            assignment.value = mutate_value(*spec, assignment.value, strength, rng);
        }
    }

    const auto child_errors = validate_recipe(child, registry);
    if (!child_errors.empty()) {
        return Result<Recipe, MutationError>::failure(
            make_error(MutationErrorCode::invalid_child, validation_summary(child_errors)));
    }

    set_metadata(child, "artminer.mutation.kind", "parameter");
    set_metadata(child, "artminer.mutation.parent", parent_fingerprint);
    set_metadata(child, "artminer.mutation.seed", std::to_string(mutation_seed));
    set_metadata(child, "artminer.mutation.operator", std::to_string(operator_version));
    set_metadata(child, "artminer.mutation.strength", format_double(strength));
    return Result<Recipe, MutationError>::success(std::move(child));
}

Result<Recipe, MutationError> make_seed_variant(
    const Recipe& parent,
    const u64 variation_seed,
    const u32 operator_version) {
    if (operator_version != kParameterMutationOperatorVersion) {
        return Result<Recipe, MutationError>::failure(
            make_error(MutationErrorCode::unsupported_operator_version, "unsupported seed-variation operator version"));
    }
    const auto parent_errors = validate_recipe(parent);
    if (!parent_errors.empty()) {
        return Result<Recipe, MutationError>::failure(
            make_error(MutationErrorCode::invalid_parent, validation_summary(parent_errors)));
    }

    Recipe child = parent;
    const std::string parent_fingerprint = semantic_fingerprint(parent);
    const u64 parent_domain = fnv1a64(parent_fingerprint);
    u64 new_seed = derive_seed(variation_seed, parent_domain ^ kSeedVariantDomain);
    if (new_seed == parent.root_seed) {
        new_seed = splitmix64(new_seed ^ kSeedVariantDomain);
    }
    child.root_seed = new_seed;
    set_metadata(child, "artminer.mutation.kind", "seed-only");
    set_metadata(child, "artminer.mutation.parent", parent_fingerprint);
    set_metadata(child, "artminer.mutation.seed", std::to_string(variation_seed));
    set_metadata(child, "artminer.mutation.operator", std::to_string(operator_version));
    return Result<Recipe, MutationError>::success(std::move(child));
}

Result<std::vector<GeneratedSpecimen>, MutationError> generate_specimen_grid(
    const Recipe& parent,
    const u64 generation_seed,
    const u32 operator_version,
    const double strength,
    const SpecimenGenerationMode mode,
    const ParameterLocks& locks,
    const NodeRegistry& registry) {
    std::vector<GeneratedSpecimen> specimens;
    specimens.reserve(kSpecimenGridSize);
    const u64 mode_domain = mode == SpecimenGenerationMode::parameter_mutation ? kGridMutationDomain : kGridSeedDomain;

    for (std::size_t index = 0U; index < kSpecimenGridSize; ++index) {
        const u64 operation_seed = derive_seed(generation_seed, mode_domain ^ static_cast<u64>(index));
        Result<Recipe, MutationError> generated =
            mode == SpecimenGenerationMode::parameter_mutation
                ? mutate_recipe_parameters(parent, operation_seed, operator_version, strength, locks, registry)
                : make_seed_variant(parent, operation_seed, operator_version);
        if (generated.is_error()) {
            return Result<std::vector<GeneratedSpecimen>, MutationError>::failure(generated.error());
        }
        Recipe recipe = std::move(generated).value();
        specimens.push_back(GeneratedSpecimen{index, operation_seed, recipe, semantic_fingerprint(recipe)});
    }
    return Result<std::vector<GeneratedSpecimen>, MutationError>::success(std::move(specimens));
}

Result<Recipe, MutationError> set_parameter_from_text(
    const Recipe& source,
    const std::string_view node_id,
    const std::string_view parameter_name,
    const std::string_view text,
    const NodeRegistry& registry) {
    const NodeInstance* source_node = find_node(source, node_id);
    if (source_node == nullptr) {
        return Result<Recipe, MutationError>::failure(
            make_error(MutationErrorCode::unknown_node, "parameter edit references an unknown node"));
    }
    const NodeMetadata* metadata = registry.find(source_node->type_id);
    if (metadata == nullptr) {
        return Result<Recipe, MutationError>::failure(
            make_error(MutationErrorCode::unknown_node, "parameter edit references an unknown node type"));
    }
    const ParameterSpec* spec = find_parameter_spec(*metadata, parameter_name);
    if (spec == nullptr || find_assignment(*source_node, parameter_name) == nullptr) {
        return Result<Recipe, MutationError>::failure(
            make_error(MutationErrorCode::unknown_parameter, "parameter edit references an unknown parameter"));
    }

    auto parsed = parse_parameter_text(*spec, text);
    if (parsed.is_error()) {
        return Result<Recipe, MutationError>::failure(parsed.error());
    }

    Recipe edited = source;
    NodeInstance* edited_node = find_node(edited, node_id);
    ParameterAssignment* assignment = edited_node == nullptr ? nullptr : find_assignment(*edited_node, parameter_name);
    if (assignment == nullptr) {
        return Result<Recipe, MutationError>::failure(
            make_error(MutationErrorCode::unknown_parameter, "parameter disappeared while applying edit"));
    }
    assignment->value = std::move(parsed).value();
    const auto validation_errors = validate_recipe(edited, registry);
    if (!validation_errors.empty()) {
        return Result<Recipe, MutationError>::failure(
            make_error(MutationErrorCode::invalid_child, validation_summary(validation_errors)));
    }
    return Result<Recipe, MutationError>::success(std::move(edited));
}

std::string format_parameter_value(const ParameterValue& value) {
    return std::visit(
        [](const auto& typed) -> std::string {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, i64>) {
                return std::to_string(typed);
            } else if constexpr (std::is_same_v<T, double>) {
                return format_double(typed);
            } else if constexpr (std::is_same_v<T, bool>) {
                return typed ? "true" : "false";
            } else {
                return typed;
            }
        },
        value);
}

RecipeHistory::RecipeHistory(Recipe initial) : entries_{std::move(initial)} {}

void RecipeHistory::push(Recipe recipe) {
    if (cursor_ + 1U < entries_.size()) {
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(cursor_ + 1U), entries_.end());
    }
    entries_.push_back(std::move(recipe));
    cursor_ = entries_.size() - 1U;
}

bool RecipeHistory::back() noexcept {
    if (!can_back()) {
        return false;
    }
    --cursor_;
    return true;
}

bool RecipeHistory::forward() noexcept {
    if (!can_forward()) {
        return false;
    }
    ++cursor_;
    return true;
}

}  // namespace artminer::core
