#include "quarry/topology_search.hpp"

#include <charconv>
#include <map>
#include <sstream>
#include <utility>

#include "core/hash.hpp"
#include "core/prng.hpp"

namespace artminer::quarry {
namespace {

constexpr core::u64 kTopologyCandidateDomain = 0x51544f504f43414eULL;  // "QTOPOCAN"

[[nodiscard]] QuarryError make_error(const QuarryErrorCode code, std::string message) {
    return QuarryError{code, std::move(message)};
}

[[nodiscard]] bool parse_u32(const std::string_view text, core::u32& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] core::Result<void, QuarryError> validate_config(const TopologySearchConfig& config) {
    if (config.version != kTopologySearchConfigVersion ||
        config.operator_version != core::kTopologyMutationOperatorVersion) {
        return core::Result<void, QuarryError>::failure(make_error(
            QuarryErrorCode::unsupported_version, "unsupported Quarry topology-search config/operator version"));
    }
    if (config.budget == 0U || config.budget > core::kMaximumTopologyMutationBudget) {
        return core::Result<void, QuarryError>::failure(make_error(
            QuarryErrorCode::resource_limit, "Quarry topology budget must be within 1..16"));
    }
    auto parsed_limits = core::parse_topology_limits(core::serialize_topology_limits(config.limits));
    if (parsed_limits.is_error()) {
        return core::Result<void, QuarryError>::failure(make_error(
            QuarryErrorCode::resource_limit, parsed_limits.error()));
    }
    return core::Result<void, QuarryError>::success();
}

}  // namespace

std::string serialize_topology_search_config(const TopologySearchConfig& config) {
    std::ostringstream output;
    output << "ARTMINER_QUARRY_TOPOLOGY " << config.version << '\n';
    output << "operator " << config.operator_version << '\n';
    output << "budget " << config.budget << '\n';
    output << "limits " << core::serialize_topology_limits(config.limits) << '\n';
    const std::string locks = config.locks.serialize_canonical();
    output << "locks " << (locks.empty() ? "-" : locks) << '\n';
    return output.str();
}

core::Result<TopologySearchConfig, QuarryError> parse_topology_search_config(const std::string_view text) {
    std::istringstream input{std::string(text)};
    std::string magic;
    std::string version_text;
    if (!(input >> magic >> version_text) || magic != "ARTMINER_QUARRY_TOPOLOGY") {
        return core::Result<TopologySearchConfig, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_manifest, "Quarry topology-search config header is malformed"));
    }
    TopologySearchConfig config;
    if (!parse_u32(version_text, config.version)) {
        return core::Result<TopologySearchConfig, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_manifest, "Quarry topology-search config version is malformed"));
    }
    std::map<std::string, std::string, std::less<>> fields;
    std::string key;
    std::string value;
    while (input >> key >> value) {
        if (!fields.emplace(key, value).second) {
            return core::Result<TopologySearchConfig, QuarryError>::failure(make_error(
                QuarryErrorCode::invalid_manifest, "Quarry topology-search config contains a duplicate field"));
        }
    }
    if (fields.size() != 4U || !fields.contains("operator") || !fields.contains("budget") ||
        !fields.contains("limits") || !fields.contains("locks")) {
        return core::Result<TopologySearchConfig, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_manifest, "Quarry topology-search config has missing/unknown fields"));
    }
    if (!parse_u32(fields["operator"], config.operator_version) || !parse_u32(fields["budget"], config.budget)) {
        return core::Result<TopologySearchConfig, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_manifest, "Quarry topology operator/budget is malformed"));
    }
    auto limits = core::parse_topology_limits(fields["limits"]);
    auto locks = core::parse_structural_locks(fields["locks"]);
    if (limits.is_error() || locks.is_error()) {
        return core::Result<TopologySearchConfig, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_manifest, limits.is_error() ? limits.error() : locks.error()));
    }
    config.limits = limits.value();
    config.locks = std::move(locks).value();
    auto valid = validate_config(config);
    if (valid.is_error()) {
        return core::Result<TopologySearchConfig, QuarryError>::failure(valid.error());
    }
    return core::Result<TopologySearchConfig, QuarryError>::success(std::move(config));
}

core::Result<std::string, QuarryError> topology_search_identity(
    const JobManifest& manifest,
    const TopologySearchConfig& config) {
    auto valid = validate_config(config);
    if (valid.is_error()) {
        return core::Result<std::string, QuarryError>::failure(valid.error());
    }
    const auto recipe_errors = core::validate_recipe(manifest.base_recipe);
    if (!recipe_errors.empty()) {
        return core::Result<std::string, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_recipe, "Quarry topology base recipe is invalid: " + recipe_errors.front().message));
    }
    const std::string base_fingerprint = core::semantic_fingerprint(manifest.base_recipe);
    std::string payload = "ArtMiner.Quarry.TopologySearch.v1\nmanifest=" + manifest.identity;
    payload += "\nbase=" + base_fingerprint;
    payload += "\nroot=" + std::to_string(manifest.root_seed);
    payload += "\nfirst=" + std::to_string(manifest.first_candidate);
    payload += "\ncount=" + std::to_string(manifest.candidate_count);
    payload += "\nconfig=\n" + serialize_topology_search_config(config);
    return core::Result<std::string, QuarryError>::success(core::hex_u64(core::fnv1a64(payload)));
}

core::Result<TopologyCandidate, QuarryError> reconstruct_topology_candidate(
    const JobManifest& manifest,
    const core::u64 candidate_index,
    const TopologySearchConfig& config,
    const core::NodeRegistry& registry) {
    if (manifest.candidate_count == 0U || candidate_index < manifest.first_candidate ||
        candidate_index - manifest.first_candidate >= manifest.candidate_count) {
        return core::Result<TopologyCandidate, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_job, "topology candidate index is outside the Quarry manifest range"));
    }
    auto identity = topology_search_identity(manifest, config);
    if (identity.is_error()) {
        return core::Result<TopologyCandidate, QuarryError>::failure(identity.error());
    }
    const core::u64 operation_seed = core::derive_seed(
        manifest.root_seed,
        core::splitmix64(kTopologyCandidateDomain ^ candidate_index));
    const core::TopologyMutationOptions options{
        operation_seed, config.operator_version, config.budget, config.limits};
    auto mutated = core::mutate_recipe_topology(manifest.base_recipe, options, config.locks, registry);
    if (mutated.is_error()) {
        return core::Result<TopologyCandidate, QuarryError>::failure(make_error(
            QuarryErrorCode::mutation_failed,
            "Quarry topology mutation failed: " + mutated.error().message));
    }
    core::Recipe recipe = std::move(mutated).value().recipe;
    recipe.render.width = manifest.render_width;
    recipe.render.height = manifest.render_height;
    const std::string fingerprint = core::semantic_fingerprint(recipe);
    const std::string candidate_payload =
        identity.value() + "|" + std::to_string(candidate_index) + "|" + fingerprint;
    TopologyCandidate candidate;
    candidate.index = candidate_index;
    candidate.operation_seed = operation_seed;
    candidate.search_identity = identity.value();
    candidate.candidate_id = core::hex_u64(core::fnv1a64(candidate_payload));
    candidate.recipe_fingerprint = fingerprint;
    candidate.recipe = std::move(recipe);
    return core::Result<TopologyCandidate, QuarryError>::success(std::move(candidate));
}

}  // namespace artminer::quarry
