#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/graph.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

namespace artminer::core {

inline constexpr u32 kRecipeSchemaVersion = 1U;
inline constexpr u32 kEvaluatorSemanticVersion = 1U;
inline constexpr std::size_t kMaximumRecipeNodes = 4096U;
inline constexpr std::size_t kMaximumRecipeParameters = 65536U;
inline constexpr std::size_t kMaximumRecipeEdges = 16384U;
inline constexpr std::size_t kMaximumRecipeOutputs = 1024U;
inline constexpr std::size_t kMaximumRecipeMetadata = 4096U;

struct RenderSettings {
    u32 width{512U};
    u32 height{512U};
    std::string quality{"reference"};
};

struct ParameterAssignment {
    std::string name;
    ParameterValue value;
};

struct NodeInstance {
    std::string id;
    std::string type_id;
    u32 semantic_version{1U};
    std::vector<ParameterAssignment> parameters;
};

struct Edge {
    std::string from_node;
    std::string from_port;
    std::string to_node;
    std::string to_port;
};

struct OutputBinding {
    std::string name;
    std::string node_id;
    std::string port;
};

struct RecipeMetadata {
    std::string key;
    std::string value;
};

struct Recipe {
    u32 schema_version{kRecipeSchemaVersion};
    u32 evaluator_version{kEvaluatorSemanticVersion};
    u64 root_seed{0U};
    RenderSettings render;
    std::vector<NodeInstance> nodes;
    std::vector<Edge> edges;
    std::vector<OutputBinding> outputs;
    std::vector<RecipeMetadata> metadata;
};

enum class RecipeErrorCode {
    malformed,
    resource_limit,
    unsupported_schema_version,
    unsupported_evaluator_version,
};

struct RecipeError {
    RecipeErrorCode code{RecipeErrorCode::malformed};
    std::size_t line{0U};
    std::string message;
};

[[nodiscard]] Result<Recipe, RecipeError> parse_recipe(std::string_view text);

// Canonical file serialization includes sorted non-semantic metadata. The semantic
// payload intentionally excludes metadata so notes/UI annotations do not change
// rendered recipe identity.
[[nodiscard]] std::string serialize_recipe_canonical(const Recipe& recipe);
[[nodiscard]] std::string serialize_recipe_semantic(const Recipe& recipe);
[[nodiscard]] std::string semantic_fingerprint(const Recipe& recipe);

}  // namespace artminer::core
