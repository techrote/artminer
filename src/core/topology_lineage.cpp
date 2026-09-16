#include "core/topology_lineage.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <sstream>
#include <utility>

namespace artminer::core {
namespace {

[[nodiscard]] TopologyLineageError make_error(const TopologyLineageErrorCode code, std::string message) {
    return TopologyLineageError{code, std::move(message)};
}

[[nodiscard]] bool parse_u32(const std::string_view text, u32& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_u64(const std::string_view text, u64& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool fingerprint_token(const std::string_view text) noexcept {
    return text.size() == 32U && std::all_of(text.begin(), text.end(), [](const char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

}  // namespace

Result<TopologyLineageRecord, TopologyLineageError> topology_lineage_record_from_recipe(const Recipe& child) {
    auto provenance = topology_provenance_from_recipe(child);
    if (provenance.is_error()) {
        return Result<TopologyLineageRecord, TopologyLineageError>::failure(make_error(
            provenance.error().code == TopologyMutationErrorCode::malformed_provenance
                ? TopologyLineageErrorCode::no_lineage
                : TopologyLineageErrorCode::malformed,
            provenance.error().message));
    }
    TopologyLineageRecord record;
    record.child_fingerprint = semantic_fingerprint(child);
    record.parent_fingerprint = provenance.value().parent_fingerprint;
    record.operator_version = provenance.value().operator_version;
    record.operation_seed = provenance.value().seed;
    record.budget = provenance.value().budget;
    record.limits = provenance.value().limits;
    record.locks = provenance.value().locks;
    record.trace = provenance.value().trace;
    return Result<TopologyLineageRecord, TopologyLineageError>::success(std::move(record));
}

std::string serialize_topology_lineage_record(const TopologyLineageRecord& record) {
    std::ostringstream output;
    output << "aml-topology " << kTopologyLineageFormatVersion << '\n';
    output << "child " << record.child_fingerprint << '\n';
    output << "parent " << record.parent_fingerprint << '\n';
    output << "operator " << record.operator_version << '\n';
    output << "seed " << record.operation_seed << '\n';
    output << "budget " << record.budget << '\n';
    output << "limits " << serialize_topology_limits(record.limits) << '\n';
    output << "locks " << (record.locks.serialize_canonical().empty() ? "-" : record.locks.serialize_canonical()) << '\n';
    output << "trace " << (record.trace.empty() ? "-" : record.trace) << '\n';
    return output.str();
}

Result<TopologyLineageRecord, TopologyLineageError> parse_topology_lineage_record(const std::string_view text) {
    std::istringstream input{std::string(text)};
    std::map<std::string, std::string, std::less<>> fields;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        const std::size_t separator = line.find(' ');
        if (separator == std::string::npos || separator == 0U || separator + 1U >= line.size()) {
            return Result<TopologyLineageRecord, TopologyLineageError>::failure(
                make_error(TopologyLineageErrorCode::malformed, "topology lineage contains a malformed line"));
        }
        if (!fields.emplace(line.substr(0U, separator), line.substr(separator + 1U)).second) {
            return Result<TopologyLineageRecord, TopologyLineageError>::failure(
                make_error(TopologyLineageErrorCode::malformed, "topology lineage contains a duplicate field"));
        }
    }

    static constexpr std::string_view known[] = {
        "aml-topology", "child", "parent", "operator", "seed", "budget", "limits", "locks", "trace"};
    for (const auto& [key, value] : fields) {
        (void)value;
        if (std::find(std::begin(known), std::end(known), key) == std::end(known)) {
            return Result<TopologyLineageRecord, TopologyLineageError>::failure(
                make_error(TopologyLineageErrorCode::malformed, "topology lineage contains an unknown field: " + key));
        }
    }
    for (const auto required : known) {
        if (!fields.contains(std::string(required))) {
            return Result<TopologyLineageRecord, TopologyLineageError>::failure(
                make_error(TopologyLineageErrorCode::malformed, "topology lineage is missing field: " + std::string(required)));
        }
    }

    u32 version = 0U;
    if (!parse_u32(fields["aml-topology"], version)) {
        return Result<TopologyLineageRecord, TopologyLineageError>::failure(
            make_error(TopologyLineageErrorCode::malformed, "topology lineage format version is malformed"));
    }
    if (version != kTopologyLineageFormatVersion) {
        return Result<TopologyLineageRecord, TopologyLineageError>::failure(
            make_error(TopologyLineageErrorCode::unsupported_version, "unsupported topology lineage format version"));
    }

    TopologyLineageRecord record;
    record.child_fingerprint = fields["child"];
    record.parent_fingerprint = fields["parent"];
    if (!fingerprint_token(record.child_fingerprint) || !fingerprint_token(record.parent_fingerprint) ||
        !parse_u32(fields["operator"], record.operator_version) ||
        !parse_u64(fields["seed"], record.operation_seed) || !parse_u32(fields["budget"], record.budget)) {
        return Result<TopologyLineageRecord, TopologyLineageError>::failure(
            make_error(TopologyLineageErrorCode::malformed, "topology lineage identity/numeric field is malformed"));
    }
    if (record.operator_version != kTopologyMutationOperatorVersion || record.budget == 0U ||
        record.budget > kMaximumTopologyMutationBudget) {
        return Result<TopologyLineageRecord, TopologyLineageError>::failure(
            make_error(TopologyLineageErrorCode::malformed, "topology lineage operator/budget is unsupported"));
    }
    auto limits = parse_topology_limits(fields["limits"]);
    auto locks = parse_structural_locks(fields["locks"]);
    if (limits.is_error() || locks.is_error()) {
        return Result<TopologyLineageRecord, TopologyLineageError>::failure(make_error(
            TopologyLineageErrorCode::malformed, limits.is_error() ? limits.error() : locks.error()));
    }
    record.limits = limits.value();
    record.locks = std::move(locks).value();
    record.trace = fields["trace"] == "-" ? std::string{} : fields["trace"];
    if (record.trace.empty()) {
        return Result<TopologyLineageRecord, TopologyLineageError>::failure(
            make_error(TopologyLineageErrorCode::malformed, "topology lineage trace must contain at least one accepted edit"));
    }
    return Result<TopologyLineageRecord, TopologyLineageError>::success(std::move(record));
}

Result<Recipe, TopologyLineageError> replay_topology_lineage_record(
    const TopologyLineageRecord& record,
    const Recipe& parent,
    const NodeRegistry& registry) {
    if (semantic_fingerprint(parent) != record.parent_fingerprint) {
        return Result<Recipe, TopologyLineageError>::failure(
            make_error(TopologyLineageErrorCode::parent_mismatch, "topology lineage parent fingerprint does not match"));
    }
    const TopologyMutationOptions options{
        record.operation_seed, record.operator_version, record.budget, record.limits};
    auto replayed = mutate_recipe_topology(parent, options, record.locks, registry);
    if (replayed.is_error()) {
        return Result<Recipe, TopologyLineageError>::failure(
            make_error(TopologyLineageErrorCode::replay_failed, replayed.error().message));
    }
    const std::string replayed_fingerprint = semantic_fingerprint(replayed.value().recipe);
    auto replayed_record = topology_lineage_record_from_recipe(replayed.value().recipe);
    if (replayed_fingerprint != record.child_fingerprint || replayed_record.is_error() ||
        replayed_record.value().trace != record.trace) {
        return Result<Recipe, TopologyLineageError>::failure(
            make_error(TopologyLineageErrorCode::replay_failed, "topology lineage replay differs from recorded child/trace"));
    }
    return Result<Recipe, TopologyLineageError>::success(std::move(replayed).value().recipe);
}

}  // namespace artminer::core
