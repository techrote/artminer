#include "core/topology_mutation.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

#include "core/hash.hpp"
#include "core/prng.hpp"

namespace artminer::core {
namespace {

constexpr u64 kTopologySelectionDomain = 0x544f504f4c4f4759ULL;  // "TOPOLOGY"
constexpr u64 kTopologyIdDomain = 0x544f504f4c4f4944ULL;         // "TOPOLOID"

struct Opportunity final {
    TopologyOperatorKind kind{TopologyOperatorKind::insert_node};
    std::string key;
    std::string a;
    std::string b;
    std::string c;
    std::string d;
    std::string e;
};

[[nodiscard]] TopologyMutationError make_error(const TopologyMutationErrorCode code, std::string message) {
    return TopologyMutationError{code, std::move(message)};
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

[[nodiscard]] const PortSpec* find_port(const std::vector<PortSpec>& ports, const std::string_view name) {
    const auto found = std::find_if(ports.begin(), ports.end(), [name](const PortSpec& port) { return port.name == name; });
    return found == ports.end() ? nullptr : &*found;
}

[[nodiscard]] NodeInstance make_default_node(const std::string& id, const NodeMetadata& metadata) {
    NodeInstance node;
    node.id = id;
    node.type_id = metadata.type_id;
    node.semantic_version = metadata.semantic_version;
    node.parameters.reserve(metadata.parameters.size());
    for (const auto& parameter : metadata.parameters) {
        node.parameters.push_back(ParameterAssignment{parameter.name, parameter.default_value});
    }
    return node;
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

[[nodiscard]] const NodeMetadata* node_metadata(
    const Recipe& recipe,
    const NodeRegistry& registry,
    const std::string_view id) {
    const NodeInstance* node = find_node(recipe, id);
    return node == nullptr ? nullptr : registry.find(node->type_id);
}

[[nodiscard]] bool protected_node(
    const Recipe& recipe,
    const NodeRegistry& registry,
    const StructuralLocks& locks,
    const std::string_view id) {
    if (locks.node_locked(id)) {
        return true;
    }
    const NodeMetadata* metadata = node_metadata(recipe, registry, id);
    return metadata != nullptr && metadata->state_class == NodeStateClass::state_boundary;
}

[[nodiscard]] bool is_output_node(const Recipe& recipe, const std::string_view id) {
    return std::any_of(recipe.outputs.begin(), recipe.outputs.end(), [id](const OutputBinding& output) {
        return output.node_id == id;
    });
}

[[nodiscard]] bool edge_equal(const Edge& edge, const Opportunity& op) {
    return edge.from_node == op.a && edge.from_port == op.b && edge.to_node == op.c && edge.to_port == op.d;
}

[[nodiscard]] std::vector<const NodeMetadata*> sorted_metadata(const NodeRegistry& registry) {
    std::vector<const NodeMetadata*> result;
    result.reserve(registry.nodes().size());
    for (const auto& metadata : registry.nodes()) {
        result.push_back(&metadata);
    }
    std::sort(result.begin(), result.end(), [](const NodeMetadata* left, const NodeMetadata* right) {
        return std::tie(left->type_id, left->semantic_version) < std::tie(right->type_id, right->semantic_version);
    });
    return result;
}

[[nodiscard]] std::vector<Edge> sorted_edges(const Recipe& recipe) {
    std::vector<Edge> result = recipe.edges;
    std::sort(result.begin(), result.end(), [](const Edge& left, const Edge& right) {
        return std::tie(left.from_node, left.from_port, left.to_node, left.to_port) <
            std::tie(right.from_node, right.from_port, right.to_node, right.to_port);
    });
    return result;
}

[[nodiscard]] std::vector<PortSpec> sorted_ports(const std::vector<PortSpec>& ports) {
    std::vector<PortSpec> result = ports;
    std::sort(result.begin(), result.end(), [](const PortSpec& left, const PortSpec& right) {
        return std::tie(left.name, left.kind, left.required, left.allow_multiple) <
            std::tie(right.name, right.kind, right.required, right.allow_multiple);
    });
    return result;
}

[[nodiscard]] std::string generated_node_id(
    const Recipe& recipe,
    const u64 operation_root,
    const u32 edit_index,
    const std::string_view opportunity_key) {
    std::string material = "ArtMiner.Topology.GeneratedNode.v1\n";
    material += std::to_string(operation_root);
    material += '\n';
    material += std::to_string(edit_index);
    material += '\n';
    material.append(opportunity_key);
    u64 candidate = fnv1a64(material) ^ kTopologyIdDomain;
    for (u32 attempt = 0U; attempt < 8U; ++attempt) {
        const std::string id = "tm-" + hex_u64(candidate);
        if (find_node(recipe, id) == nullptr) {
            return id;
        }
        candidate = splitmix64(candidate ^ static_cast<u64>(attempt + 1U));
    }
    return {};
}

[[nodiscard]] bool valid_limits(const TopologyLimits& limits) noexcept {
    return limits.max_nodes > 0U && limits.max_edges > 0U && limits.max_depth > 0U &&
        limits.max_nodes <= kHardMaximumTopologyNodes && limits.max_edges <= kHardMaximumTopologyEdges &&
        limits.max_depth <= kHardMaximumTopologyDepth;
}

[[nodiscard]] bool within_limits(
    const Recipe& recipe,
    const TopologyLimits& limits,
    const NodeRegistry& registry) {
    if (recipe.nodes.size() > limits.max_nodes || recipe.edges.size() > limits.max_edges) {
        return false;
    }
    const std::size_t depth = topology_depth(recipe, registry);
    return depth != (std::numeric_limits<std::size_t>::max)() && depth <= limits.max_depth;
}

[[nodiscard]] bool edge_exists(const Recipe& recipe, const Edge& candidate) {
    return std::any_of(recipe.edges.begin(), recipe.edges.end(), [&](const Edge& edge) {
        return edge.from_node == candidate.from_node && edge.from_port == candidate.from_port &&
            edge.to_node == candidate.to_node && edge.to_port == candidate.to_port;
    });
}

[[nodiscard]] Result<Recipe, TopologyMutationError> apply_op(
    const Recipe& source,
    const Opportunity& op,
    const u64 operation_root,
    const u32 edit_index,
    const TopologyLimits& limits,
    const NodeRegistry& registry) {
    Recipe child = source;

    if (op.kind == TopologyOperatorKind::insert_node) {
        const NodeMetadata* metadata = registry.find(op.e);
        if (metadata == nullptr) {
            return Result<Recipe, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::invalid_child, "insert opportunity references missing node metadata"));
        }
        const std::string id = generated_node_id(source, operation_root, edit_index, op.key);
        if (id.empty()) {
            return Result<Recipe, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::resource_limit, "bounded generated-node id attempts were exhausted"));
        }
        const auto edge = std::find_if(child.edges.begin(), child.edges.end(), [&](const Edge& candidate) {
            return candidate.from_node == op.a && candidate.from_port == op.b && candidate.to_node == op.c &&
                candidate.to_port == op.d;
        });
        if (edge == child.edges.end()) {
            return Result<Recipe, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::invalid_child, "insert source edge disappeared"));
        }
        const std::size_t separator = op.key.rfind('/');
        const std::size_t separator2 = separator == std::string::npos ? std::string::npos : op.key.rfind('/', separator - 1U);
        if (separator == std::string::npos || separator2 == std::string::npos) {
            return Result<Recipe, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::invalid_child, "insert opportunity key is malformed"));
        }
        const std::string input_port = op.key.substr(separator2 + 1U, separator - separator2 - 1U);
        const std::string output_port = op.key.substr(separator + 1U);
        const Edge old = *edge;
        child.edges.erase(edge);
        child.nodes.push_back(make_default_node(id, *metadata));
        child.edges.push_back(Edge{old.from_node, old.from_port, id, input_port});
        child.edges.push_back(Edge{id, output_port, old.to_node, old.to_port});
    } else if (op.kind == TopologyOperatorKind::replace_node) {
        NodeInstance* node = find_node(child, op.a);
        const NodeMetadata* metadata = registry.find(op.e);
        if (node == nullptr || metadata == nullptr) {
            return Result<Recipe, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::invalid_child, "replacement target or metadata disappeared"));
        }
        *node = make_default_node(op.a, *metadata);
    } else if (op.kind == TopologyOperatorKind::delete_bypass) {
        const auto incoming = std::find_if(child.edges.begin(), child.edges.end(), [&](const Edge& edge) {
            return edge.to_node == op.a;
        });
        const auto outgoing = std::find_if(child.edges.begin(), child.edges.end(), [&](const Edge& edge) {
            return edge.from_node == op.a;
        });
        if (incoming == child.edges.end() || outgoing == child.edges.end()) {
            return Result<Recipe, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::invalid_child, "delete/bypass edges disappeared"));
        }
        const Edge bypass{incoming->from_node, incoming->from_port, outgoing->to_node, outgoing->to_port};
        std::erase_if(child.edges, [&](const Edge& edge) { return edge.from_node == op.a || edge.to_node == op.a; });
        std::erase_if(child.nodes, [&](const NodeInstance& node) { return node.id == op.a; });
        if (!edge_exists(child, bypass)) {
            child.edges.push_back(bypass);
        }
    } else if (op.kind == TopologyOperatorKind::duplicate_branch) {
        const NodeInstance* original = find_node(source, op.a);
        if (original == nullptr) {
            return Result<Recipe, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::invalid_child, "duplicate source disappeared"));
        }
        const std::string id = generated_node_id(source, operation_root, edit_index, op.key);
        if (id.empty()) {
            return Result<Recipe, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::resource_limit, "bounded generated-node id attempts were exhausted"));
        }
        NodeInstance duplicate = *original;
        duplicate.id = id;
        child.nodes.push_back(std::move(duplicate));
        const std::vector<Edge> original_edges = child.edges;
        for (const auto& edge : original_edges) {
            if (edge.to_node == op.a) {
                child.edges.push_back(Edge{edge.from_node, edge.from_port, id, edge.to_port});
            }
        }
        const auto outgoing = std::find_if(child.edges.begin(), child.edges.end(), [&](const Edge& edge) {
            return edge.from_node == op.a && edge.from_port == op.b && edge.to_node == op.c && edge.to_port == op.d;
        });
        if (outgoing == child.edges.end()) {
            return Result<Recipe, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::invalid_child, "duplicate branch edge disappeared"));
        }
        outgoing->from_node = id;
    } else if (op.kind == TopologyOperatorKind::rewire_edge) {
        const auto edge = std::find_if(child.edges.begin(), child.edges.end(), [&](const Edge& candidate) {
            return candidate.from_node == op.a && candidate.from_port == op.b && candidate.to_node == op.c &&
                candidate.to_port == op.d;
        });
        if (edge == child.edges.end()) {
            return Result<Recipe, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::invalid_child, "rewire source edge disappeared"));
        }
        const std::size_t separator = op.e.find('/');
        if (separator == std::string::npos || separator == 0U || separator + 1U >= op.e.size()) {
            return Result<Recipe, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::invalid_child, "rewire source encoding is malformed"));
        }
        edge->from_node = op.e.substr(0U, separator);
        edge->from_port = op.e.substr(separator + 1U);
    }

    if (!within_limits(child, limits, registry)) {
        return Result<Recipe, TopologyMutationError>::failure(
            make_error(TopologyMutationErrorCode::resource_limit, "topology edit exceeds configured node/edge/depth limits"));
    }
    const auto errors = validate_recipe(child, registry);
    if (!errors.empty()) {
        return Result<Recipe, TopologyMutationError>::failure(
            make_error(TopologyMutationErrorCode::invalid_child, validation_summary(errors)));
    }
    return Result<Recipe, TopologyMutationError>::success(std::move(child));
}

void maybe_add_valid(
    std::vector<Opportunity>& opportunities,
    Opportunity opportunity,
    const Recipe& recipe,
    const u64 operation_root,
    const u32 edit_index,
    const TopologyLimits& limits,
    const NodeRegistry& registry) {
    if (opportunities.size() >= kMaximumTopologyOpportunities) {
        return;
    }
    auto applied = apply_op(recipe, opportunity, operation_root, edit_index, limits, registry);
    if (applied.is_ok()) {
        opportunities.push_back(std::move(opportunity));
    }
}

[[nodiscard]] Result<std::vector<Opportunity>, TopologyMutationError> enumerate_opportunities(
    const Recipe& recipe,
    const StructuralLocks& locks,
    const u64 operation_root,
    const u32 edit_index,
    const TopologyLimits& limits,
    const NodeRegistry& registry) {
    std::vector<Opportunity> result;
    const auto metadata_catalog = sorted_metadata(registry);
    const auto edges = sorted_edges(recipe);

    // Safe insertion: split one existing typed edge with a stateless one-input node.
    for (const auto& edge : edges) {
        if (protected_node(recipe, registry, locks, edge.from_node) ||
            protected_node(recipe, registry, locks, edge.to_node)) {
            continue;
        }
        const NodeMetadata* from_meta = node_metadata(recipe, registry, edge.from_node);
        const NodeMetadata* to_meta = node_metadata(recipe, registry, edge.to_node);
        if (from_meta == nullptr || to_meta == nullptr) {
            continue;
        }
        const PortSpec* from_port = find_port(from_meta->outputs, edge.from_port);
        const PortSpec* to_port = find_port(to_meta->inputs, edge.to_port);
        if (from_port == nullptr || to_port == nullptr || from_port->kind != to_port->kind) {
            continue;
        }
        for (const NodeMetadata* candidate : metadata_catalog) {
            if (candidate->state_class != NodeStateClass::stateless) {
                continue;
            }
            std::size_t required_count = 0U;
            for (const auto& input : candidate->inputs) {
                required_count += input.required ? 1U : 0U;
            }
            if (required_count != 1U) {
                continue;
            }
            const auto inputs = sorted_ports(candidate->inputs);
            const auto outputs = sorted_ports(candidate->outputs);
            for (const auto& input : inputs) {
                if (!input.required || input.kind != from_port->kind) {
                    continue;
                }
                for (const auto& output : outputs) {
                    if (output.kind != from_port->kind) {
                        continue;
                    }
                    Opportunity op;
                    op.kind = TopologyOperatorKind::insert_node;
                    op.a = edge.from_node;
                    op.b = edge.from_port;
                    op.c = edge.to_node;
                    op.d = edge.to_port;
                    op.e = candidate->type_id;
                    op.key = "insert/" + edge.from_node + "." + edge.from_port + ">" + edge.to_node + "." +
                        edge.to_port + "/" + candidate->type_id + "/" + input.name + "/" + output.name;
                    maybe_add_valid(result, std::move(op), recipe, operation_root, edit_index, limits, registry);
                }
            }
        }
    }

    // Compatible replacement: preserve node id and all existing port names/types.
    std::vector<std::string> node_ids;
    node_ids.reserve(recipe.nodes.size());
    for (const auto& node : recipe.nodes) {
        node_ids.push_back(node.id);
    }
    std::sort(node_ids.begin(), node_ids.end());
    for (const auto& node_id : node_ids) {
        if (protected_node(recipe, registry, locks, node_id)) {
            continue;
        }
        const NodeInstance* node = find_node(recipe, node_id);
        const NodeMetadata* original_meta = node == nullptr ? nullptr : registry.find(node->type_id);
        if (node == nullptr || original_meta == nullptr) {
            continue;
        }
        for (const NodeMetadata* candidate : metadata_catalog) {
            if (candidate->type_id == node->type_id || candidate->state_class != original_meta->state_class ||
                candidate->state_class == NodeStateClass::state_boundary) {
                continue;
            }
            bool compatible = true;
            std::set<std::string, std::less<>> supplied_inputs;
            for (const auto& edge : recipe.edges) {
                if (edge.to_node == node_id) {
                    const PortSpec* old_port = find_port(original_meta->inputs, edge.to_port);
                    const PortSpec* new_port = find_port(candidate->inputs, edge.to_port);
                    if (old_port == nullptr || new_port == nullptr || old_port->kind != new_port->kind) {
                        compatible = false;
                        break;
                    }
                    supplied_inputs.insert(edge.to_port);
                }
                if (edge.from_node == node_id) {
                    const PortSpec* old_port = find_port(original_meta->outputs, edge.from_port);
                    const PortSpec* new_port = find_port(candidate->outputs, edge.from_port);
                    if (old_port == nullptr || new_port == nullptr || old_port->kind != new_port->kind) {
                        compatible = false;
                        break;
                    }
                }
            }
            if (!compatible) {
                continue;
            }
            for (const auto& input : candidate->inputs) {
                if (input.required && !supplied_inputs.contains(input.name)) {
                    compatible = false;
                    break;
                }
            }
            if (!compatible) {
                continue;
            }
            for (const auto& output : recipe.outputs) {
                if (output.node_id == node_id) {
                    const PortSpec* old_port = find_port(original_meta->outputs, output.port);
                    const PortSpec* new_port = find_port(candidate->outputs, output.port);
                    if (old_port == nullptr || new_port == nullptr || old_port->kind != new_port->kind) {
                        compatible = false;
                        break;
                    }
                }
            }
            if (!compatible) {
                continue;
            }
            Opportunity op;
            op.kind = TopologyOperatorKind::replace_node;
            op.a = node_id;
            op.e = candidate->type_id;
            op.key = "replace/" + node_id + "/" + node->type_id + ">" + candidate->type_id;
            maybe_add_valid(result, std::move(op), recipe, operation_root, edit_index, limits, registry);
        }
    }

    // Removable one-in/one-out stateless nodes may be bypassed when endpoint
    // structures are not protected and the resulting typed graph validates.
    for (const auto& node_id : node_ids) {
        if (protected_node(recipe, registry, locks, node_id) || is_output_node(recipe, node_id)) {
            continue;
        }
        const NodeMetadata* metadata = node_metadata(recipe, registry, node_id);
        if (metadata == nullptr || metadata->state_class != NodeStateClass::stateless) {
            continue;
        }
        std::vector<Edge> incoming;
        std::vector<Edge> outgoing;
        for (const auto& edge : recipe.edges) {
            if (edge.to_node == node_id) {
                incoming.push_back(edge);
            }
            if (edge.from_node == node_id) {
                outgoing.push_back(edge);
            }
        }
        if (incoming.size() != 1U || outgoing.size() != 1U ||
            protected_node(recipe, registry, locks, incoming.front().from_node) ||
            protected_node(recipe, registry, locks, outgoing.front().to_node)) {
            continue;
        }
        Opportunity op;
        op.kind = TopologyOperatorKind::delete_bypass;
        op.a = node_id;
        op.key = "delete/" + node_id + "/" + incoming.front().from_node + ">" + outgoing.front().to_node;
        maybe_add_valid(result, std::move(op), recipe, operation_root, edit_index, limits, registry);
    }

    // Duplicate one stateless branch node, sharing its typed inputs and moving
    // exactly one outgoing edge to the duplicate.
    for (const auto& node_id : node_ids) {
        if (protected_node(recipe, registry, locks, node_id)) {
            continue;
        }
        const NodeMetadata* metadata = node_metadata(recipe, registry, node_id);
        if (metadata == nullptr || metadata->state_class != NodeStateClass::stateless) {
            continue;
        }
        for (const auto& edge : edges) {
            if (edge.from_node != node_id || protected_node(recipe, registry, locks, edge.to_node)) {
                continue;
            }
            bool input_protected = false;
            for (const auto& input_edge : recipe.edges) {
                if (input_edge.to_node == node_id && protected_node(recipe, registry, locks, input_edge.from_node)) {
                    input_protected = true;
                    break;
                }
            }
            if (input_protected) {
                continue;
            }
            Opportunity op;
            op.kind = TopologyOperatorKind::duplicate_branch;
            op.a = node_id;
            op.b = edge.from_port;
            op.c = edge.to_node;
            op.d = edge.to_port;
            op.key = "duplicate/" + node_id + "/" + edge.from_port + ">" + edge.to_node + "." + edge.to_port;
            maybe_add_valid(result, std::move(op), recipe, operation_root, edit_index, limits, registry);
        }
    }

    // Type-compatible source rewiring. Normal validation filters ordinary cycles.
    for (const auto& edge : edges) {
        if (protected_node(recipe, registry, locks, edge.from_node) ||
            protected_node(recipe, registry, locks, edge.to_node)) {
            continue;
        }
        const NodeMetadata* target_meta = node_metadata(recipe, registry, edge.to_node);
        const PortSpec* target_port = target_meta == nullptr ? nullptr : find_port(target_meta->inputs, edge.to_port);
        if (target_port == nullptr) {
            continue;
        }
        for (const auto& source_id : node_ids) {
            if (source_id == edge.from_node || protected_node(recipe, registry, locks, source_id)) {
                continue;
            }
            const NodeMetadata* source_meta = node_metadata(recipe, registry, source_id);
            if (source_meta == nullptr) {
                continue;
            }
            const auto outputs = sorted_ports(source_meta->outputs);
            for (const auto& output : outputs) {
                if (output.kind != target_port->kind) {
                    continue;
                }
                Edge replacement{source_id, output.name, edge.to_node, edge.to_port};
                if (edge_exists(recipe, replacement)) {
                    continue;
                }
                Opportunity op;
                op.kind = TopologyOperatorKind::rewire_edge;
                op.a = edge.from_node;
                op.b = edge.from_port;
                op.c = edge.to_node;
                op.d = edge.to_port;
                op.e = source_id + "/" + output.name;
                op.key = "rewire/" + edge.from_node + "." + edge.from_port + ">" + edge.to_node + "." +
                    edge.to_port + "/from/" + source_id + "." + output.name;
                maybe_add_valid(result, std::move(op), recipe, operation_root, edit_index, limits, registry);
            }
        }
    }

    if (result.size() >= kMaximumTopologyOpportunities) {
        return Result<std::vector<Opportunity>, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::resource_limit,
            "topology opportunity set reached the bounded 65,536-opportunity safety limit"));
    }
    std::sort(result.begin(), result.end(), [](const Opportunity& left, const Opportunity& right) {
        return std::tie(left.key, left.kind) < std::tie(right.key, right.kind);
    });
    return Result<std::vector<Opportunity>, TopologyMutationError>::success(std::move(result));
}

void set_metadata(Recipe& recipe, std::string key, std::string value) {
    const auto found = std::find_if(recipe.metadata.begin(), recipe.metadata.end(), [&key](const RecipeMetadata& item) {
        return item.key == key;
    });
    if (found == recipe.metadata.end()) {
        recipe.metadata.push_back(RecipeMetadata{std::move(key), std::move(value)});
    } else {
        found->value = std::move(value);
    }
}

[[nodiscard]] const std::string* metadata_value(const Recipe& recipe, const std::string_view key) {
    const auto found = std::find_if(recipe.metadata.begin(), recipe.metadata.end(), [key](const RecipeMetadata& item) {
        return item.key == key;
    });
    return found == recipe.metadata.end() ? nullptr : &found->value;
}

void erase_operation_metadata(Recipe& recipe) {
    std::erase_if(recipe.metadata, [](const RecipeMetadata& metadata) {
        return metadata.key.starts_with("artminer.mutation.") || metadata.key.starts_with("artminer.crossover.") ||
            metadata.key.starts_with("artminer.topology.");
    });
}

[[nodiscard]] bool parse_u64(const std::string_view text, u64& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_u32(const std::string_view text, u32& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] std::string operation_domain(
    const std::string_view parent_fingerprint,
    const TopologyMutationOptions& options,
    const StructuralLocks& locks) {
    std::string text = "ArtMiner.TopologyMutation.v1\nparent=";
    text.append(parent_fingerprint);
    text += "\nseed=" + std::to_string(options.seed);
    text += "\noperator=" + std::to_string(options.operator_version);
    text += "\nbudget=" + std::to_string(options.budget);
    text += "\nlimits=" + serialize_topology_limits(options.limits);
    text += "\nlocks=" + locks.serialize_canonical();
    return text;
}

[[nodiscard]] std::string trace_text(const std::vector<TopologyMutationStep>& steps) {
    std::string trace;
    for (std::size_t index = 0U; index < steps.size(); ++index) {
        if (index != 0U) {
            trace.push_back(';');
        }
        trace += std::string(to_string(steps[index].kind));
        trace.push_back('@');
        trace += steps[index].opportunity_key;
    }
    return trace;
}

}  // namespace

bool operator==(const TopologyLimits& left, const TopologyLimits& right) noexcept {
    return left.max_nodes == right.max_nodes && left.max_edges == right.max_edges && left.max_depth == right.max_depth;
}

std::string serialize_topology_limits(const TopologyLimits& limits) {
    return std::to_string(limits.max_nodes) + "/" + std::to_string(limits.max_edges) + "/" +
        std::to_string(limits.max_depth);
}

Result<TopologyLimits, std::string> parse_topology_limits(const std::string_view text) {
    const std::size_t first = text.find('/');
    const std::size_t second = first == std::string_view::npos ? std::string_view::npos : text.find('/', first + 1U);
    if (first == std::string_view::npos || second == std::string_view::npos || text.find('/', second + 1U) != std::string_view::npos) {
        return Result<TopologyLimits, std::string>::failure("topology limits must be nodes/edges/depth");
    }
    auto parse_size = [](const std::string_view token, std::size_t& output) {
        unsigned long long value = 0ULL;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
            value > static_cast<unsigned long long>((std::numeric_limits<std::size_t>::max)())) {
            return false;
        }
        output = static_cast<std::size_t>(value);
        return true;
    };
    TopologyLimits limits;
    if (!parse_size(text.substr(0U, first), limits.max_nodes) ||
        !parse_size(text.substr(first + 1U, second - first - 1U), limits.max_edges) ||
        !parse_size(text.substr(second + 1U), limits.max_depth) || !valid_limits(limits)) {
        return Result<TopologyLimits, std::string>::failure("topology limits are malformed or outside hard safety bounds");
    }
    return Result<TopologyLimits, std::string>::success(limits);
}

std::string_view to_string(const TopologyOperatorKind kind) noexcept {
    switch (kind) {
    case TopologyOperatorKind::insert_node:
        return "insert";
    case TopologyOperatorKind::replace_node:
        return "replace";
    case TopologyOperatorKind::delete_bypass:
        return "delete-bypass";
    case TopologyOperatorKind::duplicate_branch:
        return "duplicate-branch";
    case TopologyOperatorKind::rewire_edge:
        return "rewire";
    }
    return "unknown";
}

const std::vector<TopologyOperatorInfo>& topology_operator_catalog() {
    static const std::vector<TopologyOperatorInfo> catalog{
        {TopologyOperatorKind::insert_node, "insert-node"},
        {TopologyOperatorKind::replace_node, "compatible-replacement"},
        {TopologyOperatorKind::delete_bypass, "delete-bypass"},
        {TopologyOperatorKind::duplicate_branch, "duplicate-branch"},
        {TopologyOperatorKind::rewire_edge, "typed-edge-rewire"},
    };
    return catalog;
}

bool StructuralLocks::node_locked(const std::string_view node_id) const {
    return nodes_.contains(std::string(node_id));
}

void StructuralLocks::set_node(std::string node_id, const bool locked) {
    if (locked) {
        nodes_.insert(std::move(node_id));
    } else {
        nodes_.erase(node_id);
    }
}

void StructuralLocks::clear() noexcept {
    nodes_.clear();
}

bool StructuralLocks::lock_subgraph(const Recipe& recipe, const std::string_view root_node) {
    if (find_node(recipe, root_node) == nullptr) {
        return false;
    }
    std::set<std::string, std::less<>> pending;
    pending.insert(std::string(root_node));
    while (!pending.empty()) {
        const std::string current = *pending.begin();
        pending.erase(pending.begin());
        if (!nodes_.insert(current).second) {
            continue;
        }
        std::vector<std::string> children;
        for (const auto& edge : recipe.edges) {
            if (edge.from_node == current) {
                children.push_back(edge.to_node);
            }
        }
        std::sort(children.begin(), children.end());
        for (auto& child : children) {
            if (!nodes_.contains(child)) {
                pending.insert(std::move(child));
            }
        }
    }
    return true;
}

std::string StructuralLocks::serialize_canonical() const {
    std::string text;
    for (const auto& node : nodes_) {
        if (!text.empty()) {
            text.push_back(';');
        }
        text += "n/";
        text += node;
    }
    return text;
}

Result<StructuralLocks, std::string> parse_structural_locks(const std::string_view text) {
    StructuralLocks locks;
    if (text.empty() || text == "-") {
        return Result<StructuralLocks, std::string>::success(std::move(locks));
    }
    std::size_t position = 0U;
    while (position < text.size()) {
        const std::size_t separator = text.find(';', position);
        const std::size_t end = separator == std::string_view::npos ? text.size() : separator;
        const std::string_view token = text.substr(position, end - position);
        if (!token.starts_with("n/") || token.size() <= 2U) {
            return Result<StructuralLocks, std::string>::failure("structural lock token must be n/<node-id>");
        }
        const std::string_view id = token.substr(2U);
        const bool valid = std::all_of(id.begin(), id.end(), [](const char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                ch == '.' || ch == '_' || ch == '-';
        });
        if (!valid || id.size() > 96U) {
            return Result<StructuralLocks, std::string>::failure("structural lock contains an invalid node identifier");
        }
        locks.set_node(std::string(id), true);
        if (separator == std::string_view::npos) {
            break;
        }
        position = separator + 1U;
    }
    return Result<StructuralLocks, std::string>::success(std::move(locks));
}

Result<TopologyMutationResult, TopologyMutationError> mutate_recipe_topology(
    const Recipe& parent,
    const TopologyMutationOptions& options,
    const StructuralLocks& locks,
    const NodeRegistry& registry) {
    if (options.operator_version != kTopologyMutationOperatorVersion) {
        return Result<TopologyMutationResult, TopologyMutationError>::failure(
            make_error(TopologyMutationErrorCode::unsupported_operator_version, "unsupported topology-mutation operator version"));
    }
    if (options.budget == 0U || options.budget > kMaximumTopologyMutationBudget) {
        return Result<TopologyMutationResult, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::invalid_budget, "topology mutation budget must be within 1..16"));
    }
    if (!valid_limits(options.limits)) {
        return Result<TopologyMutationResult, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::invalid_limits,
            "topology limits must be positive and within hard bounds 512 nodes / 1024 edges / depth 128"));
    }
    const auto parent_errors = validate_recipe(parent, registry);
    if (!parent_errors.empty()) {
        return Result<TopologyMutationResult, TopologyMutationError>::failure(
            make_error(TopologyMutationErrorCode::invalid_parent, validation_summary(parent_errors)));
    }
    if (!within_limits(parent, options.limits, registry)) {
        return Result<TopologyMutationResult, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::resource_limit, "parent graph already exceeds the requested topology limits"));
    }

    const std::string parent_fingerprint = semantic_fingerprint(parent);
    const u64 operation_root = derive_seed(options.seed, fnv1a64(operation_domain(parent_fingerprint, options, locks)) ^ kTopologySelectionDomain);
    Recipe current = parent;
    std::vector<TopologyMutationStep> steps;
    steps.reserve(options.budget);

    for (u32 edit_index = 0U; edit_index < options.budget; ++edit_index) {
        auto opportunities = enumerate_opportunities(current, locks, operation_root, edit_index, options.limits, registry);
        if (opportunities.is_error()) {
            return Result<TopologyMutationResult, TopologyMutationError>::failure(opportunities.error());
        }
        if (opportunities.value().empty()) {
            if (steps.empty()) {
                return Result<TopologyMutationResult, TopologyMutationError>::failure(make_error(
                    TopologyMutationErrorCode::no_legal_mutation,
                    "no legal topology edit exists under the current types, structural locks, state boundaries and limits"));
            }
            break;
        }
        const u64 pick = splitmix64(operation_root ^ (0x9e3779b97f4a7c15ULL * static_cast<u64>(edit_index + 1U)));
        const std::size_t selected_index = static_cast<std::size_t>(pick % opportunities.value().size());
        const Opportunity selected = opportunities.value()[selected_index];
        auto applied = apply_op(current, selected, operation_root, edit_index, options.limits, registry);
        if (applied.is_error()) {
            // Opportunities are validated before selection. Reaching this branch
            // is an internal deterministic contract failure, never a random retry.
            return Result<TopologyMutationResult, TopologyMutationError>::failure(make_error(
                TopologyMutationErrorCode::invalid_child,
                "selected prevalidated topology opportunity failed: " + applied.error().message));
        }
        current = std::move(applied).value();
        steps.push_back(TopologyMutationStep{selected.kind, selected.key});
    }

    const auto final_errors = validate_recipe(current, registry);
    if (!final_errors.empty() || !within_limits(current, options.limits, registry)) {
        return Result<TopologyMutationResult, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::invalid_child,
            final_errors.empty() ? "final topology child exceeds configured limits" : validation_summary(final_errors)));
    }

    erase_operation_metadata(current);
    const std::string trace = trace_text(steps);
    set_metadata(current, "artminer.topology.parent", parent_fingerprint);
    set_metadata(current, "artminer.topology.seed", std::to_string(options.seed));
    set_metadata(current, "artminer.topology.operator", std::to_string(options.operator_version));
    set_metadata(current, "artminer.topology.budget", std::to_string(options.budget));
    set_metadata(current, "artminer.topology.limits", serialize_topology_limits(options.limits));
    set_metadata(current, "artminer.topology.locks", locks.serialize_canonical().empty() ? "-" : locks.serialize_canonical());
    set_metadata(current, "artminer.topology.trace", trace);
    return Result<TopologyMutationResult, TopologyMutationError>::success(
        TopologyMutationResult{std::move(current), std::move(steps)});
}

Result<TopologyMutationProvenance, TopologyMutationError> topology_provenance_from_recipe(const Recipe& child) {
    const std::string* parent = metadata_value(child, "artminer.topology.parent");
    const std::string* seed_text = metadata_value(child, "artminer.topology.seed");
    const std::string* operator_text = metadata_value(child, "artminer.topology.operator");
    const std::string* budget_text = metadata_value(child, "artminer.topology.budget");
    const std::string* limits_text = metadata_value(child, "artminer.topology.limits");
    const std::string* locks_text = metadata_value(child, "artminer.topology.locks");
    const std::string* trace = metadata_value(child, "artminer.topology.trace");
    if (parent == nullptr || seed_text == nullptr || operator_text == nullptr || budget_text == nullptr ||
        limits_text == nullptr || locks_text == nullptr || trace == nullptr) {
        return Result<TopologyMutationProvenance, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::malformed_provenance, "topology mutation provenance is incomplete"));
    }
    if (parent->size() != 32U || !std::all_of(parent->begin(), parent->end(), [](const char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        })) {
        return Result<TopologyMutationProvenance, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::malformed_provenance, "topology parent fingerprint is malformed"));
    }
    TopologyMutationProvenance provenance;
    provenance.parent_fingerprint = *parent;
    if (!parse_u64(*seed_text, provenance.seed) || !parse_u32(*operator_text, provenance.operator_version) ||
        !parse_u32(*budget_text, provenance.budget)) {
        return Result<TopologyMutationProvenance, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::malformed_provenance, "topology provenance numeric field is malformed"));
    }
    auto limits = parse_topology_limits(*limits_text);
    auto locks = parse_structural_locks(*locks_text);
    if (limits.is_error() || locks.is_error()) {
        return Result<TopologyMutationProvenance, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::malformed_provenance,
            limits.is_error() ? limits.error() : locks.error()));
    }
    provenance.limits = limits.value();
    provenance.locks = std::move(locks).value();
    provenance.trace = *trace;
    if (provenance.operator_version != kTopologyMutationOperatorVersion || provenance.budget == 0U ||
        provenance.budget > kMaximumTopologyMutationBudget) {
        return Result<TopologyMutationProvenance, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::malformed_provenance, "topology provenance requests an unsupported operator/budget"));
    }
    return Result<TopologyMutationProvenance, TopologyMutationError>::success(std::move(provenance));
}

Result<Recipe, TopologyMutationError> replay_topology_mutation(
    const Recipe& child_with_provenance,
    const Recipe& parent,
    const NodeRegistry& registry) {
    auto provenance = topology_provenance_from_recipe(child_with_provenance);
    if (provenance.is_error()) {
        return Result<Recipe, TopologyMutationError>::failure(provenance.error());
    }
    if (semantic_fingerprint(parent) != provenance.value().parent_fingerprint) {
        return Result<Recipe, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::parent_mismatch, "topology lineage parent fingerprint does not match"));
    }
    const TopologyMutationOptions options{
        provenance.value().seed,
        provenance.value().operator_version,
        provenance.value().budget,
        provenance.value().limits};
    auto replayed = mutate_recipe_topology(parent, options, provenance.value().locks, registry);
    if (replayed.is_error()) {
        return Result<Recipe, TopologyMutationError>::failure(replayed.error());
    }
    if (semantic_fingerprint(replayed.value().recipe) != semantic_fingerprint(child_with_provenance) ||
        trace_text(replayed.value().steps) != provenance.value().trace) {
        return Result<Recipe, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::invalid_child, "replayed topology child/trace does not match recorded provenance"));
    }
    return Result<Recipe, TopologyMutationError>::success(std::move(replayed).value().recipe);
}

std::size_t topology_depth(const Recipe& recipe, const NodeRegistry& registry) {
    std::map<std::string, std::size_t, std::less<>> indices;
    for (std::size_t index = 0U; index < recipe.nodes.size(); ++index) {
        indices.emplace(recipe.nodes[index].id, index);
    }
    std::vector<std::vector<std::size_t>> adjacency(recipe.nodes.size());
    std::vector<std::size_t> indegree(recipe.nodes.size(), 0U);
    for (const auto& edge : recipe.edges) {
        const auto from = indices.find(edge.from_node);
        const auto to = indices.find(edge.to_node);
        const NodeMetadata* target = node_metadata(recipe, registry, edge.to_node);
        if (from == indices.end() || to == indices.end() || target == nullptr) {
            return (std::numeric_limits<std::size_t>::max)();
        }
        if (target->state_class == NodeStateClass::state_boundary) {
            continue;
        }
        adjacency[from->second].push_back(to->second);
        ++indegree[to->second];
    }
    std::queue<std::size_t> ready;
    std::vector<std::size_t> depth(recipe.nodes.size(), 1U);
    for (std::size_t index = 0U; index < indegree.size(); ++index) {
        if (indegree[index] == 0U) {
            ready.push(index);
        }
    }
    std::size_t visited = 0U;
    std::size_t maximum = recipe.nodes.empty() ? 0U : 1U;
    while (!ready.empty()) {
        const std::size_t current = ready.front();
        ready.pop();
        ++visited;
        maximum = (std::max)(maximum, depth[current]);
        for (const std::size_t next : adjacency[current]) {
            depth[next] = (std::max)(depth[next], depth[current] + 1U);
            --indegree[next];
            if (indegree[next] == 0U) {
                ready.push(next);
            }
        }
    }
    return visited == recipe.nodes.size() ? maximum : (std::numeric_limits<std::size_t>::max)();
}

}  // namespace artminer::core
