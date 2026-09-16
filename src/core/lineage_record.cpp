#include "core/breeding.hpp"

#include <charconv>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <utility>

namespace artminer::core {
namespace {

[[nodiscard]] LineageError make_error(const LineageErrorCode code, std::string message) {
    return LineageError{code, std::move(message)};
}

[[nodiscard]] std::string kind_text(const LineageOperationKind kind) {
    switch (kind) {
    case LineageOperationKind::parameter_mutation:
        return "parameter";
    case LineageOperationKind::seed_variant:
        return "seed";
    case LineageOperationKind::crossover:
        return "crossover";
    }
    return "unknown";
}

[[nodiscard]] bool parse_u64(const std::string_view text, u64& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_u32(const std::string_view text, u32& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_real(const std::string_view text, double& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(value);
}

[[nodiscard]] std::string format_real(const double value) {
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
    std::ostringstream output;
    output.precision(std::numeric_limits<double>::max_digits10);
    output << value;
    return output.str();
}

[[nodiscard]] bool fingerprint_token(const std::string_view value) noexcept {
    if (value.size() != 32U) {
        return false;
    }
    for (const char ch : value) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool lock_token(const std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (const char ch : value) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-' ||
            ch == '/' || ch == ';';
        if (!ok) {
            return false;
        }
    }
    return true;
}

}  // namespace

LineageRecord make_crossover_lineage_record(
    const Recipe& child,
    const Recipe& parent_a,
    const Recipe& parent_b,
    const u64 crossover_seed,
    const u32 operator_version,
    const ParameterLocks& locks) {
    return LineageRecord{
        LineageOperationKind::crossover,
        semantic_fingerprint(child),
        semantic_fingerprint(parent_a),
        semantic_fingerprint(parent_b),
        operator_version,
        crossover_seed,
        std::nullopt,
        locks.serialize_canonical()};
}

LineageRecord make_parameter_mutation_lineage_record(
    const Recipe& child,
    const Recipe& parent,
    const u64 mutation_seed,
    const u32 operator_version,
    const double strength,
    const ParameterLocks& locks) {
    return LineageRecord{
        LineageOperationKind::parameter_mutation,
        semantic_fingerprint(child),
        semantic_fingerprint(parent),
        std::nullopt,
        operator_version,
        mutation_seed,
        strength,
        locks.serialize_canonical()};
}

LineageRecord make_seed_variant_lineage_record(
    const Recipe& child,
    const Recipe& parent,
    const u64 variation_seed,
    const u32 operator_version) {
    return LineageRecord{
        LineageOperationKind::seed_variant,
        semantic_fingerprint(child),
        semantic_fingerprint(parent),
        std::nullopt,
        operator_version,
        variation_seed,
        std::nullopt,
        {}};
}

std::string serialize_lineage_record(const LineageRecord& record) {
    std::ostringstream output;
    output << "aml " << kLineageFormatVersion << '\n';
    output << "kind " << kind_text(record.kind) << '\n';
    output << "child " << record.child_fingerprint << '\n';
    output << "parent_a " << record.parent_a_fingerprint << '\n';
    if (record.parent_b_fingerprint.has_value()) {
        output << "parent_b " << *record.parent_b_fingerprint << '\n';
    }
    output << "operator " << record.operator_version << '\n';
    output << "seed " << record.operation_seed << '\n';
    if (record.mutation_strength.has_value()) {
        output << "strength " << format_real(*record.mutation_strength) << '\n';
    }
    output << "locks " << (record.locks.empty() ? "-" : record.locks) << '\n';
    return output.str();
}

Result<LineageRecord, LineageError> parse_lineage_record(const std::string_view text) {
    std::istringstream input{std::string(text)};
    std::map<std::string, std::string> fields;
    std::string line;
    std::size_t line_number = 0U;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }
        const std::size_t separator = line.find(' ');
        if (separator == std::string::npos || separator == 0U || separator + 1U >= line.size()) {
            return Result<LineageRecord, LineageError>::failure(
                make_error(LineageErrorCode::malformed_lineage, "malformed lineage record at line " + std::to_string(line_number)));
        }
        const std::string key = line.substr(0U, separator);
        const std::string value = line.substr(separator + 1U);
        if (!fields.emplace(key, value).second) {
            return Result<LineageRecord, LineageError>::failure(
                make_error(LineageErrorCode::malformed_lineage, "duplicate lineage field: " + key));
        }
    }

    const auto version = fields.find("aml");
    if (version == fields.end()) {
        return Result<LineageRecord, LineageError>::failure(
            make_error(LineageErrorCode::malformed_lineage, "lineage record has no format version"));
    }
    u32 format_version = 0U;
    if (!parse_u32(version->second, format_version)) {
        return Result<LineageRecord, LineageError>::failure(
            make_error(LineageErrorCode::malformed_lineage, "lineage format version is malformed"));
    }
    if (format_version != kLineageFormatVersion) {
        return Result<LineageRecord, LineageError>::failure(
            make_error(LineageErrorCode::unsupported_lineage_version, "unsupported lineage format version"));
    }

    static constexpr std::string_view known[] = {
        "aml", "kind", "child", "parent_a", "parent_b", "operator", "seed", "strength", "locks"};
    for (const auto& [key, value] : fields) {
        (void)value;
        bool recognized = false;
        for (const auto candidate : known) {
            if (key == candidate) {
                recognized = true;
                break;
            }
        }
        if (!recognized) {
            return Result<LineageRecord, LineageError>::failure(
                make_error(LineageErrorCode::malformed_lineage, "unknown lineage field: " + key));
        }
    }

    const auto kind = fields.find("kind");
    const auto child = fields.find("child");
    const auto parent_a = fields.find("parent_a");
    const auto operator_field = fields.find("operator");
    const auto seed_field = fields.find("seed");
    const auto locks_field = fields.find("locks");
    if (kind == fields.end() || child == fields.end() || parent_a == fields.end() ||
        operator_field == fields.end() || seed_field == fields.end() || locks_field == fields.end()) {
        return Result<LineageRecord, LineageError>::failure(
            make_error(LineageErrorCode::malformed_lineage, "lineage record is missing a required field"));
    }
    if (!fingerprint_token(child->second) || !fingerprint_token(parent_a->second)) {
        return Result<LineageRecord, LineageError>::failure(
            make_error(LineageErrorCode::malformed_lineage, "lineage fingerprint must be 32 lowercase hexadecimal digits"));
    }

    u32 operator_version = 0U;
    u64 operation_seed = 0U;
    if (!parse_u32(operator_field->second, operator_version) || !parse_u64(seed_field->second, operation_seed)) {
        return Result<LineageRecord, LineageError>::failure(
            make_error(LineageErrorCode::malformed_lineage, "lineage operator/seed field is malformed"));
    }

    LineageRecord record;
    record.child_fingerprint = child->second;
    record.parent_a_fingerprint = parent_a->second;
    record.operator_version = operator_version;
    record.operation_seed = operation_seed;
    record.locks = locks_field->second == "-" ? std::string{} : locks_field->second;
    if (!record.locks.empty() && !lock_token(record.locks)) {
        return Result<LineageRecord, LineageError>::failure(
            make_error(LineageErrorCode::malformed_lineage, "lineage lock token is malformed"));
    }

    if (kind->second == "crossover") {
        const auto parent_b = fields.find("parent_b");
        if (parent_b == fields.end() || !fingerprint_token(parent_b->second) || fields.contains("strength")) {
            return Result<LineageRecord, LineageError>::failure(
                make_error(LineageErrorCode::malformed_lineage, "crossover lineage requires a valid parent_b fingerprint and no strength"));
        }
        record.kind = LineageOperationKind::crossover;
        record.parent_b_fingerprint = parent_b->second;
    } else if (kind->second == "parameter") {
        const auto strength = fields.find("strength");
        double parsed_strength = 0.0;
        if (strength == fields.end() || !parse_real(strength->second, parsed_strength) ||
            parsed_strength < 0.0 || parsed_strength > 1.0 || fields.contains("parent_b")) {
            return Result<LineageRecord, LineageError>::failure(
                make_error(LineageErrorCode::malformed_lineage, "parameter lineage requires strength in [0,1] and one parent"));
        }
        record.kind = LineageOperationKind::parameter_mutation;
        record.mutation_strength = parsed_strength;
    } else if (kind->second == "seed") {
        if (fields.contains("parent_b") || fields.contains("strength") || !record.locks.empty()) {
            return Result<LineageRecord, LineageError>::failure(
                make_error(LineageErrorCode::malformed_lineage, "seed lineage accepts one parent and no locks/strength"));
        }
        record.kind = LineageOperationKind::seed_variant;
    } else {
        return Result<LineageRecord, LineageError>::failure(
            make_error(LineageErrorCode::malformed_lineage, "unknown lineage operation kind"));
    }

    return Result<LineageRecord, LineageError>::success(std::move(record));
}

}  // namespace artminer::core
