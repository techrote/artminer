#include "core/recipe.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <tuple>
#include <utility>

#include "core/hash.hpp"
#include "core/local_text.hpp"

namespace artminer::core {
namespace {

struct TokenizeResult {
    bool ok{true};
    std::vector<std::string> tokens;
    std::string error;
};

[[nodiscard]] int hex_digit(const char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return 10 + (character - 'a');
    }
    if (character >= 'A' && character <= 'F') {
        return 10 + (character - 'A');
    }
    return -1;
}

[[nodiscard]] TokenizeResult tokenize_line(const std::string_view line) {
    TokenizeResult result;
    std::size_t index = 0U;

    while (index < line.size()) {
        while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) {
            ++index;
        }
        if (index >= line.size() || line[index] == '#') {
            break;
        }

        std::string token;
        if (line[index] == '"') {
            ++index;
            bool closed = false;
            while (index < line.size()) {
                const char character = line[index++];
                if (character == '"') {
                    closed = true;
                    break;
                }
                if (character != '\\') {
                    token.push_back(character);
                    continue;
                }
                if (index >= line.size()) {
                    result.ok = false;
                    result.error = "trailing escape in quoted token";
                    return result;
                }
                const char escaped = line[index++];
                switch (escaped) {
                case '\\':
                    token.push_back('\\');
                    break;
                case '"':
                    token.push_back('"');
                    break;
                case 'n':
                    token.push_back('\n');
                    break;
                case 'r':
                    token.push_back('\r');
                    break;
                case 't':
                    token.push_back('\t');
                    break;
                case 'x': {
                    if (index + 1U >= line.size()) {
                        result.ok = false;
                        result.error = "incomplete hexadecimal escape";
                        return result;
                    }
                    const int high = hex_digit(line[index]);
                    const int low = hex_digit(line[index + 1U]);
                    if (high < 0 || low < 0) {
                        result.ok = false;
                        result.error = "invalid hexadecimal escape";
                        return result;
                    }
                    token.push_back(static_cast<char>((high << 4) | low));
                    index += 2U;
                    break;
                }
                default:
                    result.ok = false;
                    result.error = "unsupported escape sequence";
                    return result;
                }
            }
            if (!closed) {
                result.ok = false;
                result.error = "unterminated quoted token";
                return result;
            }
            if (index < line.size() && line[index] != ' ' && line[index] != '\t' && line[index] != '#') {
                result.ok = false;
                result.error = "quoted token must be followed by whitespace or a comment";
                return result;
            }
        } else {
            const std::size_t start = index;
            while (index < line.size() && line[index] != ' ' && line[index] != '\t' && line[index] != '#') {
                if (line[index] == '"') {
                    result.ok = false;
                    result.error = "quotes must start at a token boundary";
                    return result;
                }
                ++index;
            }
            token.assign(line.substr(start, index - start));
        }
        result.tokens.push_back(std::move(token));
    }

    return result;
}

template <typename T>
[[nodiscard]] bool parse_integer(const std::string_view token, T& value) {
    const char* const begin = token.data();
    const char* const end = token.data() + token.size();
    const auto parsed = std::from_chars(begin, end, value, 10);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

[[nodiscard]] bool parse_real(const std::string_view token, double& value) {
    const char* const begin = token.data();
    const char* const end = token.data() + token.size();
    const auto parsed = std::from_chars(begin, end, value, std::chars_format::general);
    return parsed.ec == std::errc{} && parsed.ptr == end && std::isfinite(value);
}

[[nodiscard]] RecipeError make_error(
    const RecipeErrorCode code,
    const std::size_t line,
    std::string message) {
    return RecipeError{code, line, std::move(message)};
}

[[nodiscard]] std::string quote_token(const std::string_view token) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string output;
    output.reserve(token.size() + 2U);
    output.push_back('"');
    for (const unsigned char byte : token) {
        switch (byte) {
        case '\\':
            output += "\\\\";
            break;
        case '"':
            output += "\\\"";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (byte < 0x20U) {
                output += "\\x";
                output.push_back(kHex[(byte >> 4U) & 0x0fU]);
                output.push_back(kHex[byte & 0x0fU]);
            } else {
                output.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    output.push_back('"');
    return output;
}

[[nodiscard]] std::string canonical_real(const double value) {
    if (value == 0.0) {
        return "0";
    }
    char buffer[64]{};
    const auto converted = std::to_chars(
        std::begin(buffer),
        std::end(buffer),
        value,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10);
    if (converted.ec != std::errc{}) {
        return "0";
    }
    return std::string(buffer, converted.ptr);
}

[[nodiscard]] std::string parameter_type_tag(const ParameterValue& value) {
    if (std::holds_alternative<i64>(value)) {
        return "i64";
    }
    if (std::holds_alternative<double>(value)) {
        return "f64";
    }
    if (std::holds_alternative<bool>(value)) {
        return "bool";
    }
    return "enum";
}

[[nodiscard]] std::string parameter_value_text(const ParameterValue& value) {
    if (const auto* integer = std::get_if<i64>(&value)) {
        return std::to_string(*integer);
    }
    if (const auto* real = std::get_if<double>(&value)) {
        return canonical_real(*real);
    }
    if (const auto* boolean = std::get_if<bool>(&value)) {
        return *boolean ? "true" : "false";
    }
    return quote_token(std::get<std::string>(value));
}

[[nodiscard]] std::string serialize_impl(const Recipe& recipe, const bool include_metadata) {
    std::string output;
    output += "amr " + std::to_string(recipe.schema_version) + "\n";
    output += "evaluator " + std::to_string(recipe.evaluator_version) + "\n";
    output += "seed " + std::to_string(recipe.root_seed) + "\n";
    output += "render " + std::to_string(recipe.render.width) + " " + std::to_string(recipe.render.height) + " " +
              quote_token(recipe.render.quality) + "\n";

    std::vector<const NodeInstance*> nodes;
    nodes.reserve(recipe.nodes.size());
    for (const auto& node : recipe.nodes) {
        nodes.push_back(&node);
    }
    std::sort(nodes.begin(), nodes.end(), [](const NodeInstance* left, const NodeInstance* right) {
        return std::tie(left->id, left->type_id, left->semantic_version) <
               std::tie(right->id, right->type_id, right->semantic_version);
    });

    for (const NodeInstance* node : nodes) {
        output += "node " + quote_token(node->id) + " " + quote_token(node->type_id) + " " +
                  std::to_string(node->semantic_version) + "\n";

        std::vector<const ParameterAssignment*> parameters;
        parameters.reserve(node->parameters.size());
        for (const auto& parameter : node->parameters) {
            parameters.push_back(&parameter);
        }
        std::sort(parameters.begin(), parameters.end(), [](const ParameterAssignment* left, const ParameterAssignment* right) {
            if (left->name != right->name) {
                return left->name < right->name;
            }
            if (left->value.index() != right->value.index()) {
                return left->value.index() < right->value.index();
            }
            return parameter_value_text(left->value) < parameter_value_text(right->value);
        });

        for (const ParameterAssignment* parameter : parameters) {
            output += "param " + quote_token(node->id) + " " + quote_token(parameter->name) + " " +
                      parameter_type_tag(parameter->value) + " " + parameter_value_text(parameter->value) + "\n";
        }
    }

    std::vector<Edge> edges = recipe.edges;
    std::sort(edges.begin(), edges.end(), [](const Edge& left, const Edge& right) {
        return std::tie(left.from_node, left.from_port, left.to_node, left.to_port) <
               std::tie(right.from_node, right.from_port, right.to_node, right.to_port);
    });
    for (const auto& edge : edges) {
        output += "edge " + quote_token(edge.from_node) + " " + quote_token(edge.from_port) + " " +
                  quote_token(edge.to_node) + " " + quote_token(edge.to_port) + "\n";
    }

    std::vector<OutputBinding> outputs = recipe.outputs;
    std::sort(outputs.begin(), outputs.end(), [](const OutputBinding& left, const OutputBinding& right) {
        return std::tie(left.name, left.node_id, left.port) < std::tie(right.name, right.node_id, right.port);
    });
    for (const auto& binding : outputs) {
        output += "output " + quote_token(binding.name) + " " + quote_token(binding.node_id) + " " +
                  quote_token(binding.port) + "\n";
    }

    if (include_metadata) {
        std::vector<RecipeMetadata> metadata = recipe.metadata;
        std::sort(metadata.begin(), metadata.end(), [](const RecipeMetadata& left, const RecipeMetadata& right) {
            return std::tie(left.key, left.value) < std::tie(right.key, right.value);
        });
        for (const auto& item : metadata) {
            output += "meta " + quote_token(item.key) + " " + quote_token(item.value) + "\n";
        }
    }

    return output;
}

}  // namespace

Result<Recipe, RecipeError> parse_recipe(std::string_view text) {
    auto source_validation = validate_local_text(text);
    if (source_validation.is_error()) {
        const RecipeErrorCode code = text.size() > kMaximumRecipeFileBytes
            ? RecipeErrorCode::resource_limit
            : RecipeErrorCode::malformed;
        return Result<Recipe, RecipeError>::failure(
            make_error(code, 0U, "recipe source rejected: " + source_validation.error().message));
    }

    if (text.size() >= 3U && static_cast<unsigned char>(text[0]) == 0xefU &&
        static_cast<unsigned char>(text[1]) == 0xbbU && static_cast<unsigned char>(text[2]) == 0xbfU) {
        text.remove_prefix(3U);
    }

    Recipe recipe;
    bool saw_schema = false;
    bool saw_evaluator = false;
    bool saw_seed = false;
    bool saw_render = false;
    std::size_t total_parameters = 0U;

    std::size_t line_number = 0U;
    std::size_t offset = 0U;
    while (offset <= text.size()) {
        ++line_number;
        const std::size_t newline = text.find('\n', offset);
        const std::size_t line_end = newline == std::string_view::npos ? text.size() : newline;
        std::string_view line = text.substr(offset, line_end - offset);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1U);
        }

        const TokenizeResult tokenized = tokenize_line(line);
        if (!tokenized.ok) {
            return Result<Recipe, RecipeError>::failure(
                make_error(RecipeErrorCode::malformed, line_number, tokenized.error));
        }
        const auto& tokens = tokenized.tokens;
        if (!tokens.empty()) {
            const std::string& record = tokens[0];
            if (record == "amr") {
                if (tokens.size() != 2U || saw_schema) {
                    return Result<Recipe, RecipeError>::failure(make_error(
                        RecipeErrorCode::malformed, line_number, "amr record must appear exactly once with one version"));
                }
                if (!parse_integer(tokens[1], recipe.schema_version)) {
                    return Result<Recipe, RecipeError>::failure(
                        make_error(RecipeErrorCode::malformed, line_number, "invalid recipe schema version"));
                }
                if (recipe.schema_version != kRecipeSchemaVersion) {
                    return Result<Recipe, RecipeError>::failure(make_error(
                        RecipeErrorCode::unsupported_schema_version,
                        line_number,
                        "unsupported recipe schema version " + std::to_string(recipe.schema_version)));
                }
                saw_schema = true;
            } else if (record == "evaluator") {
                if (tokens.size() != 2U || saw_evaluator) {
                    return Result<Recipe, RecipeError>::failure(make_error(
                        RecipeErrorCode::malformed, line_number, "evaluator record must appear exactly once with one version"));
                }
                if (!parse_integer(tokens[1], recipe.evaluator_version)) {
                    return Result<Recipe, RecipeError>::failure(
                        make_error(RecipeErrorCode::malformed, line_number, "invalid evaluator semantic version"));
                }
                if (recipe.evaluator_version != kEvaluatorSemanticVersion) {
                    return Result<Recipe, RecipeError>::failure(make_error(
                        RecipeErrorCode::unsupported_evaluator_version,
                        line_number,
                        "unsupported evaluator semantic version " + std::to_string(recipe.evaluator_version)));
                }
                saw_evaluator = true;
            } else if (record == "seed") {
                if (tokens.size() != 2U || saw_seed || !parse_integer(tokens[1], recipe.root_seed)) {
                    return Result<Recipe, RecipeError>::failure(
                        make_error(RecipeErrorCode::malformed, line_number, "seed record must appear once with a uint64 value"));
                }
                saw_seed = true;
            } else if (record == "render") {
                if (tokens.size() != 4U || saw_render || !parse_integer(tokens[1], recipe.render.width) ||
                    !parse_integer(tokens[2], recipe.render.height)) {
                    return Result<Recipe, RecipeError>::failure(make_error(
                        RecipeErrorCode::malformed,
                        line_number,
                        "render record must appear once as: render <width> <height> <quality>"));
                }
                recipe.render.quality = tokens[3];
                saw_render = true;
            } else if (record == "node") {
                if (recipe.nodes.size() >= kMaximumRecipeNodes) {
                    return Result<Recipe, RecipeError>::failure(make_error(
                        RecipeErrorCode::resource_limit, line_number, "recipe exceeds the 4096-node safety limit"));
                }
                NodeInstance node;
                if (tokens.size() != 4U || !parse_integer(tokens[3], node.semantic_version)) {
                    return Result<Recipe, RecipeError>::failure(make_error(
                        RecipeErrorCode::malformed,
                        line_number,
                        "node record must be: node <id> <type-id> <semantic-version>"));
                }
                node.id = tokens[1];
                node.type_id = tokens[2];
                recipe.nodes.push_back(std::move(node));
            } else if (record == "param") {
                if (total_parameters >= kMaximumRecipeParameters) {
                    return Result<Recipe, RecipeError>::failure(make_error(
                        RecipeErrorCode::resource_limit, line_number, "recipe exceeds the 65536-parameter safety limit"));
                }
                if (tokens.size() != 5U) {
                    return Result<Recipe, RecipeError>::failure(make_error(
                        RecipeErrorCode::malformed,
                        line_number,
                        "param record must be: param <node-id> <name> <i64|f64|bool|enum> <value>"));
                }
                auto node = std::find_if(recipe.nodes.begin(), recipe.nodes.end(), [&](const NodeInstance& candidate) {
                    return candidate.id == tokens[1];
                });
                if (node == recipe.nodes.end()) {
                    return Result<Recipe, RecipeError>::failure(make_error(
                        RecipeErrorCode::malformed,
                        line_number,
                        "param record references a node that has not been declared yet: " + tokens[1]));
                }

                ParameterValue value;
                if (tokens[3] == "i64") {
                    i64 parsed = 0;
                    if (!parse_integer(tokens[4], parsed)) {
                        return Result<Recipe, RecipeError>::failure(
                            make_error(RecipeErrorCode::malformed, line_number, "invalid i64 parameter value"));
                    }
                    value = parsed;
                } else if (tokens[3] == "f64") {
                    double parsed = 0.0;
                    if (!parse_real(tokens[4], parsed)) {
                        return Result<Recipe, RecipeError>::failure(make_error(
                            RecipeErrorCode::malformed, line_number, "invalid or non-finite f64 parameter value"));
                    }
                    value = parsed;
                } else if (tokens[3] == "bool") {
                    if (tokens[4] == "true") {
                        value = true;
                    } else if (tokens[4] == "false") {
                        value = false;
                    } else {
                        return Result<Recipe, RecipeError>::failure(
                            make_error(RecipeErrorCode::malformed, line_number, "bool parameter must be true or false"));
                    }
                } else if (tokens[3] == "enum") {
                    value = tokens[4];
                } else {
                    return Result<Recipe, RecipeError>::failure(make_error(
                        RecipeErrorCode::malformed, line_number, "unsupported parameter value type tag: " + tokens[3]));
                }
                node->parameters.push_back(ParameterAssignment{tokens[2], std::move(value)});
                ++total_parameters;
            } else if (record == "edge") {
                if (recipe.edges.size() >= kMaximumRecipeEdges) {
                    return Result<Recipe, RecipeError>::failure(make_error(
                        RecipeErrorCode::resource_limit, line_number, "recipe exceeds the 16384-edge safety limit"));
                }
                if (tokens.size() != 5U) {
                    return Result<Recipe, RecipeError>::failure(make_error(
                        RecipeErrorCode::malformed,
                        line_number,
                        "edge record must be: edge <from-node> <from-port> <to-node> <to-port>"));
                }
                recipe.edges.push_back(Edge{tokens[1], tokens[2], tokens[3], tokens[4]});
            } else if (record == "output") {
                if (recipe.outputs.size() >= kMaximumRecipeOutputs) {
                    return Result<Recipe, RecipeError>::failure(make_error(
                        RecipeErrorCode::resource_limit, line_number, "recipe exceeds the 1024-output safety limit"));
                }
                if (tokens.size() != 4U) {
                    return Result<Recipe, RecipeError>::failure(make_error(
                        RecipeErrorCode::malformed,
                        line_number,
                        "output record must be: output <name> <node-id> <port>"));
                }
                recipe.outputs.push_back(OutputBinding{tokens[1], tokens[2], tokens[3]});
            } else if (record == "meta") {
                if (recipe.metadata.size() >= kMaximumRecipeMetadata) {
                    return Result<Recipe, RecipeError>::failure(make_error(
                        RecipeErrorCode::resource_limit, line_number, "recipe exceeds the 4096-metadata-record safety limit"));
                }
                if (tokens.size() != 3U) {
                    return Result<Recipe, RecipeError>::failure(make_error(
                        RecipeErrorCode::malformed, line_number, "meta record must be: meta <namespaced-key> <value>"));
                }
                recipe.metadata.push_back(RecipeMetadata{tokens[1], tokens[2]});
            } else {
                return Result<Recipe, RecipeError>::failure(make_error(
                    RecipeErrorCode::malformed,
                    line_number,
                    "unknown semantic record '" + record + "'; use meta for non-semantic extension data"));
            }
        }

        if (newline == std::string_view::npos) {
            break;
        }
        offset = newline + 1U;
    }

    if (!saw_schema || !saw_evaluator || !saw_seed || !saw_render) {
        return Result<Recipe, RecipeError>::failure(make_error(
            RecipeErrorCode::malformed,
            0U,
            "recipe must contain exactly one amr, evaluator, seed, and render record"));
    }

    return Result<Recipe, RecipeError>::success(std::move(recipe));
}

std::string serialize_recipe_canonical(const Recipe& recipe) {
    return serialize_impl(recipe, true);
}

std::string serialize_recipe_semantic(const Recipe& recipe) {
    return serialize_impl(recipe, false);
}

std::string semantic_fingerprint(const Recipe& recipe) {
    const std::string semantic = serialize_recipe_semantic(recipe);
    const u64 primary = fnv1a64(semantic);
    std::string domain_separated = "ArtMiner.SemanticFingerprint.v1\n";
    domain_separated += semantic;
    const u64 secondary = fnv1a64(domain_separated);
    return hex_u64(primary) + hex_u64(secondary);
}

}  // namespace artminer::core
