#include "core/topology_mutation.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
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

constexpr u64 kTopologyDomain = 0x544f504f4c4f4759ULL;  // "TOPOLOGY"
constexpr u64 kStepDomain = 0x535445504f50534cULL;      // "STEPOPSL"

struct Opportunity final {
    TopologyOperatorKind kind{TopologyOperatorKind::insert_node};
    std::string key;
    std::string node_id;
    std::string type_id;
    std::string from_node;
    std::string from_port;
    std::string to_node;
    std::string to_port;
    std::string input_port;
    std::string output_port;
};

[[nodiscard]] TopologyMutationError make_error(
    const TopologyMutationErrorCode code,
    std::string message) {
    return TopologyMutationError{code, std::move(message)};
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

[[nodiscard]] const PortSpec* find_port(
    const std::vector<PortSpec>& ports,
    const std::string_view name) {
    const auto found = std::find_if(ports.begin(), ports.end(), [name](const PortSpec& port) {
        return port.name == name;
    });
    return found == ports.end() ? nullptr : &*found;
}

[[nodiscard]] bool same_ports(const std::vector<PortSpec>& left, const std::vector<PortSpec>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index) {
        const PortSpec& a = left[index];
        const PortSpec& b = right[index];
        if (a.name != b.name || a.kind != b.kind || a.required != b.required ||
            a.allow_multiple != b.allow_multiple) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool output_bound(const Recipe& recipe, const std::string_view node_id) {
    return std::any_of(recipe.outputs.begin(), recipe.outputs.end(), [node_id](const OutputBinding& output) {
        return output.node_id == node_id;
    });
}

[[nodiscard]] bool protected_node(
    const Recipe& recipe,
    const std::string_view node_id,
    const StructuralLocks& locks,
    const NodeRegistry& registry) {
    if (locks.node_locked(node_id)) {
        return true;
    }
    const NodeInstance* node = find_node(recipe, node_id);
    if (node == nullptr) {
        return true;
    }
    const NodeMetadata* metadata = registry.find(node->type_id);
    return metadata == nullptr || metadata->state_class != NodeStateClass::stateless;
}

[[nodiscard]] bool editable_edge(
    const Recipe& recipe,
    const Edge& edge,
    const StructuralLocks& locks,
    const NodeRegistry& registry) {
    return !protected_node(recipe, edge.from_node, locks, registry) &&
        !protected_node(recipe, edge.to_node, locks, registry);
}

[[nodiscard]] NodeInstance make_default_node(const std::string& id, const NodeMetadata& metadata) {
    NodeInstance node;
    node.id = id;
    node.type_id = metadata.type_id;
    node.semantic_version = metadata.semantic_version;
    node.parameters.reserve(metadata.parameters.size());
    for (const ParameterSpec& parameter : metadata.parameters) {
        node.parameters.push_back(ParameterAssignment{parameter.name, parameter.default_value});
    }
    return node;
}

[[nodiscard]] std::string make_generated_node_id(
    const Recipe& recipe,
    const u64 operation_root,
    const u32 step_index,
    const std::string_view opportunity_key) {
    const u64 domain = fnv1a64(opportunity_key) ^ (static_cast<u64>(step_index) << 32U);
    const std::string base = "topo_" + hex_u64(derive_seed(operation_root, domain));
    if (find_node(recipe, base) == nullptr) {
        return base;
    }
    for (u32 suffix = 1U; suffix <= 255U; ++suffix) {
        const std::string candidate = base + "_" + std::to_string(suffix);
        if (find_node(recipe, candidate) == nullptr) {
            return candidate;
        }
    }
    return {};
}

void erase_operation_metadata(Recipe& recipe) {
    std::erase_if(recipe.metadata, [](const RecipeMetadata& metadata) {
        return metadata.key.starts_with("artminer.mutation.") ||
            metadata.key.starts_with("artminer.crossover.") ||
            metadata.key.starts_with("artminer.topology.");
    });
}

void set_metadata(Recipe& recipe, std::string key, std::string value) {
    const auto found = std::find_if(
        recipe.metadata.begin(),
        recipe.metadata.end(),
        [&key](const RecipeMetadata& item) { return item.key == key; });
    if (found == recipe.metadata.end()) {
        recipe.metadata.push_back(RecipeMetadata{std::move(key), std::move(value)});
    } else {
        found->value = std::move(value);
    }
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

[[nodiscard]] Result<std::size_t, TopologyMutationError> graph_depth(
    const Recipe& recipe,
    const NodeRegistry& registry) {
    std::map<std::string, std::size_t, std::less<>> indices;
    for (std::size_t index = 0U; index < recipe.nodes.size(); ++index) {
        indices.emplace(recipe.nodes[index].id, index);
    }
    std::vector<std::vector<std::size_t>> adjacency(recipe.nodes.size());
    std::vector<std::size_t> indegree(recipe.nodes.size(), 0U);
    for (const Edge& edge : recipe.edges) {
        const auto from = indices.find(edge.from_node);
        const auto to = indices.find(edge.to_node);
        if (from == indices.end() || to == indices.end()) {
            return Result<std::size_t, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::invalid_child, "depth analysis encountered a missing node"));
        }
        const NodeMetadata* to_metadata = registry.find(recipe.nodes[to->second].type_id);
        if (to_metadata != nullptr && to_metadata->state_class == NodeStateClass::state_boundary) {
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
    if (visited != recipe.nodes.size()) {
        return Result<std::size_t, TopologyMutationError>::failure(
            make_error(TopologyMutationErrorCode::invalid_child, "depth analysis found an ordinary graph cycle"));
    }
    return Result<std::size_t, TopologyMutationError>::success(maximum);
}

[[nodiscard]] Result<void, TopologyMutationError> check_resource_limits(
    const Recipe& recipe,
    const NodeRegistry& registry) {
    if (recipe.nodes.size() > kMaximumTopologyNodes) {
        return Result<void, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::resource_limit,
            "topology mutation node count exceeds the 256-node limit"));
    }
    if (recipe.edges.size() > kMaximumTopologyEdges) {
        return Result<void, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::resource_limit,
            "topology mutation edge count exceeds the 512-edge limit"));
    }
    auto depth = graph_depth(recipe, registry);
    if (depth.is_error()) {
        return Result<void, TopologyMutationError>::failure(depth.error());
    }
    if (depth.value() > kMaximumTopologyDepth) {
        return Result<void, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::resource_limit,
            "topology mutation dependency depth exceeds the 128-node limit"));
    }
    return Result<void, TopologyMutationError>::success();
}

[[nodiscard]] bool lock_ids_exist(const Recipe& recipe, const StructuralLocks& locks) {
    return std::all_of(locks.nodes().begin(), locks.nodes().end(), [&](const std::string& id) {
        return find_node(recipe, id) != nullptr;
    });
}

void push_opportunity(std::vector<Opportunity>& result, Opportunity opportunity, bool& overflow) {
    if (result.size() >= kMaximumTopologyOpportunities) {
        overflow = true;
        return;
    }
    result.push_back(std::move(opportunity));
}

[[nodiscard]] Result<std::vector<Opportunity>, TopologyMutationError> enumerate_opportunities(
    const Recipe& recipe,
    const StructuralLocks& locks,
    const NodeRegistry& registry) {
    std::vector<Opportunity> result;
    bool overflow = false;

    // Safe one-input/one-output insertion. All compatibility comes from port metadata.
    for (const Edge& edge : recipe.edges) {
        if (!editable_edge(recipe, edge, locks, registry)) {
            continue;
        }
        const NodeInstance* source_node = find_node(recipe, edge.from_node);
        const NodeMetadata* source_meta = source_node == nullptr ? nullptr : registry.find(source_node->type_id);
        const PortSpec* source_port = source_meta == nullptr ? nullptr : find_port(source_meta->outputs, edge.from_port);
        if (source_port == nullptr) {
            continue;
        }
        for (const NodeMetadata& metadata : registry.nodes()) {
            if (metadata.state_class != NodeStateClass::stateless || !metadata.evaluators.cpu) {
                continue;
            }
            std::size_t required_inputs = 0U;
            for (const PortSpec& input : metadata.inputs) {
                if (input.required) {
                    ++required_inputs;
                }
            }
            if (required_inputs != 1U) {
                continue;
            }
            for (const PortSpec& input : metadata.inputs) {
                if (!input.required || input.kind != source_port->kind) {
                    continue;
                }
                for (const PortSpec& output : metadata.outputs) {
                    if (output.kind != source_port->kind) {
                        continue;
                    }
                    Opportunity opportunity;
                    opportunity.kind = TopologyOperatorKind::insert_node;
                    opportunity.from_node = edge.from_node;
                    opportunity.from_port = edge.from_port;
                    opportunity.to_node = edge.to_node;
                    opportunity.to_port = edge.to_port;
                    opportunity.type_id = metadata.type_id;
                    opportunity.input_port = input.name;
                    opportunity.output_port = output.name;
                    opportunity.key = "insert|" + edge.from_node + "." + edge.from_port + "->" +
                        edge.to_node + "." + edge.to_port + "|" + metadata.type_id + "|" +
                        input.name + "|" + output.name;
                    push_opportunity(result, std::move(opportunity), overflow);
                    if (overflow) {
                        break;
                    }
                }
                if (overflow) {
                    break;
                }
            }
            if (overflow) {
                break;
            }
        }
        if (overflow) {
            break;
        }
    }

    // Compatible replacement preserves the exact named interface and is limited to stateless nodes.
    if (!overflow) {
        for (const NodeInstance& node : recipe.nodes) {
            const NodeMetadata* current = registry.find(node.type_id);
            if (current == nullptr || current->state_class != NodeStateClass::stateless ||
                locks.node_locked(node.id)) {
                continue;
            }
            const bool touches_protected = std::any_of(
                recipe.edges.begin(),
                recipe.edges.end(),
                [&](const Edge& edge) {
                    return (edge.from_node == node.id || edge.to_node == node.id) &&
                        !editable_edge(recipe, edge, locks, registry);
                });
            if (touches_protected) {
                continue;
            }
            for (const NodeMetadata& replacement : registry.nodes()) {
                if (replacement.type_id == current->type_id ||
                    replacement.state_class != NodeStateClass::stateless ||
                    !replacement.evaluators.cpu ||
                    !same_ports(current->inputs, replacement.inputs) ||
                    !same_ports(current->outputs, replacement.outputs)) {
                    continue;
                }
                Opportunity opportunity;
                opportunity.kind = TopologyOperatorKind::replace_node;
                opportunity.node_id = node.id;
                opportunity.type_id = replacement.type_id;
                opportunity.key = "replace|" + node.id + "|" + replacement.type_id;
                push_opportunity(result, std::move(opportunity), overflow);
                if (overflow) {
                    break;
                }
            }
            if (overflow) {
                break;
            }
        }
    }

    // Removable node deletion with an unambiguous one-input bypass.
    if (!overflow) {
        for (const NodeInstance& node : recipe.nodes) {
            const NodeMetadata* metadata = registry.find(node.type_id);
            if (metadata == nullptr || metadata->state_class != NodeStateClass::stateless ||
                locks.node_locked(node.id) || output_bound(recipe, node.id)) {
                continue;
            }
            std::vector<const Edge*> incoming;
            std::vector<const Edge*> outgoing;
            bool incident_protected = false;
            for (const Edge& edge : recipe.edges) {
                if (edge.to_node == node.id) {
                    incoming.push_back(&edge);
                    incident_protected = incident_protected || !editable_edge(recipe, edge, locks, registry);
                }
                if (edge.from_node == node.id) {
                    outgoing.push_back(&edge);
                    incident_protected = incident_protected || !editable_edge(recipe, edge, locks, registry);
                }
            }
            if (incident_protected || incoming.size() != 1U || outgoing.empty()) {
                continue;
            }
            const Edge& in = *incoming.front();
            const NodeInstance* producer = find_node(recipe, in.from_node);
            const NodeMetadata* producer_meta = producer == nullptr ? nullptr : registry.find(producer->type_id);
            const PortSpec* producer_port =
                producer_meta == nullptr ? nullptr : find_port(producer_meta->outputs, in.from_port);
            if (producer_port == nullptr) {
                continue;
            }
            bool compatible = true;
            for (const Edge* out : outgoing) {
                const NodeInstance* target = find_node(recipe, out->to_node);
                const NodeMetadata* target_meta = target == nullptr ? nullptr : registry.find(target->type_id);
                const PortSpec* target_port =
                    target_meta == nullptr ? nullptr : find_port(target_meta->inputs, out->to_port);
                compatible = compatible && target_port != nullptr && target_port->kind == producer_port->kind;
            }
            if (!compatible) {
                continue;
            }
            Opportunity opportunity;
            opportunity.kind = TopologyOperatorKind::delete_bypass;
            opportunity.node_id = node.id;
            opportunity.key = "delete|" + node.id;
            push_opportunity(result, std::move(opportunity), overflow);
            if (overflow) {
                break;
            }
        }
    }

    // Branch duplication clones one pure stateless node and redirects exactly one outgoing branch.
    if (!overflow) {
        for (const NodeInstance& node : recipe.nodes) {
            const NodeMetadata* metadata = registry.find(node.type_id);
            if (metadata == nullptr || metadata->state_class != NodeStateClass::stateless ||
                !metadata->evaluators.cpu || locks.node_locked(node.id)) {
                continue;
            }
            bool incoming_editable = true;
            for (const Edge& edge : recipe.edges) {
                if (edge.to_node == node.id && !editable_edge(recipe, edge, locks, registry)) {
                    incoming_editable = false;
                    break;
                }
            }
            if (!incoming_editable) {
                continue;
            }
            for (const Edge& edge : recipe.edges) {
                if (edge.from_node != node.id || !editable_edge(recipe, edge, locks, registry)) {
                    continue;
                }
                Opportunity opportunity;
                opportunity.kind = TopologyOperatorKind::duplicate_branch;
                opportunity.node_id = node.id;
                opportunity.from_port = edge.from_port;
                opportunity.to_node = edge.to_node;
                opportunity.to_port = edge.to_port;
                opportunity.key = "duplicate|" + node.id + "|" + edge.from_port + "->" +
                    edge.to_node + "." + edge.to_port;
                push_opportunity(result, std::move(opportunity), overflow);
                if (overflow) {
                    break;
                }
            }
            if (overflow) {
                break;
            }
        }
    }

    // Type-compatible rewire. Cycle legality is checked after application by the normal validator.
    if (!overflow) {
        for (const Edge& edge : recipe.edges) {
            if (!editable_edge(recipe, edge, locks, registry)) {
                continue;
            }
            const NodeInstance* target = find_node(recipe, edge.to_node);
            const NodeMetadata* target_meta = target == nullptr ? nullptr : registry.find(target->type_id);
            const PortSpec* target_port =
                target_meta == nullptr ? nullptr : find_port(target_meta->inputs, edge.to_port);
            if (target_port == nullptr) {
                continue;
            }
            for (const NodeInstance& candidate_node : recipe.nodes) {
                if (candidate_node.id == edge.from_node || candidate_node.id == edge.to_node ||
                    protected_node(recipe, candidate_node.id, locks, registry)) {
                    continue;
                }
                const NodeMetadata* candidate_meta = registry.find(candidate_node.type_id);
                if (candidate_meta == nullptr) {
                    continue;
                }
                for (const PortSpec& candidate_port : candidate_meta->outputs) {
                    if (candidate_port.kind != target_port->kind) {
                        continue;
                    }
                    Opportunity opportunity;
                    opportunity.kind = TopologyOperatorKind::rewire_edge;
                    opportunity.from_node = candidate_node.id;
                    opportunity.from_port = candidate_port.name;
                    opportunity.to_node = edge.to_node;
                    opportunity.to_port = edge.to_port;
                    opportunity.node_id = edge.from_node;
                    opportunity.output_port = edge.from_port;
                    opportunity.key = "rewire|" + edge.from_node + "." + edge.from_port + "->" +
                        edge.to_node + "." + edge.to_port + "|from|" +
                        candidate_node.id + "." + candidate_port.name;
                    push_opportunity(result, std::move(opportunity), overflow);
                    if (overflow) {
                        break;
                    }
                }
                if (overflow) {
                    break;
                }
            }
            if (overflow) {
                break;
            }
        }
    }

    if (overflow) {
        return Result<std::vector<Opportunity>, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::resource_limit,
            "topology opportunity enumeration exceeded the deterministic 65,536-opportunity limit"));
    }

    std::sort(result.begin(), result.end(), [](const Opportunity& left, const Opportunity& right) {
        return std::tie(left.key, left.kind) < std::tie(right.key, right.kind);
    });
    result.erase(
        std::unique(result.begin(), result.end(), [](const Opportunity& left, const Opportunity& right) {
            return left.key == right.key && left.kind == right.kind;
        }),
        result.end());
    return Result<std::vector<Opportunity>, TopologyMutationError>::success(std::move(result));
}

[[nodiscard]] bool erase_exact_edge(Recipe& recipe, const Edge& wanted) {
    const auto found = std::find_if(recipe.edges.begin(), recipe.edges.end(), [&](const Edge& edge) {
        return edge.from_node == wanted.from_node && edge.from_port == wanted.from_port &&
            edge.to_node == wanted.to_node && edge.to_port == wanted.to_port;
    });
    if (found == recipe.edges.end()) {
        return false;
    }
    recipe.edges.erase(found);
    return true;
}

[[nodiscard]] Result<std::string, TopologyMutationError> apply_opportunity(
    Recipe& recipe,
    const Opportunity& opportunity,
    const u64 operation_root,
    const u32 step_index,
    const NodeRegistry& registry) {
    switch (opportunity.kind) {
    case TopologyOperatorKind::insert_node: {
        const Edge old{
            opportunity.from_node,
            opportunity.from_port,
            opportunity.to_node,
            opportunity.to_port};
        if (!erase_exact_edge(recipe, old)) {
            return Result<std::string, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::invalid_child, "selected insertion edge disappeared"));
        }
        const NodeMetadata* metadata = registry.find(opportunity.type_id);
        if (metadata == nullptr) {
            return Result<std::string, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::invalid_child, "selected insertion type disappeared"));
        }
        const std::string id = make_generated_node_id(recipe, operation_root, step_index, opportunity.key);
        if (id.empty()) {
            return Result<std::string, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::resource_limit, "could not allocate a bounded unique topology node id"));
        }
        recipe.nodes.push_back(make_default_node(id, *metadata));
        recipe.edges.push_back(Edge{old.from_node, old.from_port, id, opportunity.input_port});
        recipe.edges.push_back(Edge{id, opportunity.output_port, old.to_node, old.to_port});
        return Result<std::string, TopologyMutationError>::success(
            "insert " + id + ":" + opportunity.type_id + " on " +
            old.from_node + "." + old.from_port + "->" + old.to_node + "." + old.to_port);
    }
    case TopologyOperatorKind::replace_node: {
        NodeInstance* node = find_node(recipe, opportunity.node_id);
        const NodeMetadata* metadata = registry.find(opportunity.type_id);
        if (node == nullptr || metadata == nullptr) {
            return Result<std::string, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::invalid_child, "selected replacement node/type disappeared"));
        }
        const std::string previous_type = node->type_id;
        NodeInstance replacement = make_default_node(node->id, *metadata);
        *node = std::move(replacement);
        return Result<std::string, TopologyMutationError>::success(
            "replace " + opportunity.node_id + ":" + previous_type + "->" + opportunity.type_id);
    }
    case TopologyOperatorKind::delete_bypass: {
        std::vector<Edge> incoming;
        std::vector<Edge> outgoing;
        for (const Edge& edge : recipe.edges) {
            if (edge.to_node == opportunity.node_id) {
                incoming.push_back(edge);
            }
            if (edge.from_node == opportunity.node_id) {
                outgoing.push_back(edge);
            }
        }
        if (incoming.size() != 1U || outgoing.empty()) {
            return Result<std::string, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::invalid_child, "selected deletion is no longer an unambiguous bypass"));
        }
        const Edge producer = incoming.front();
        std::erase_if(recipe.edges, [&](const Edge& edge) {
            return edge.from_node == opportunity.node_id || edge.to_node == opportunity.node_id;
        });
        for (const Edge& out : outgoing) {
            recipe.edges.push_back(Edge{producer.from_node, producer.from_port, out.to_node, out.to_port});
        }
        std::erase_if(recipe.nodes, [&](const NodeInstance& node) { return node.id == opportunity.node_id; });
        return Result<std::string, TopologyMutationError>::success(
            "delete-bypass " + opportunity.node_id + " from " +
            producer.from_node + "." + producer.from_port);
    }
    case TopologyOperatorKind::duplicate_branch: {
        const NodeInstance* original = find_node(recipe, opportunity.node_id);
        if (original == nullptr) {
            return Result<std::string, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::invalid_child, "selected duplication source disappeared"));
        }
        const std::string clone_id = make_generated_node_id(recipe, operation_root, step_index, opportunity.key);
        if (clone_id.empty()) {
            return Result<std::string, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::resource_limit, "could not allocate a bounded unique duplicate node id"));
        }
        NodeInstance clone = *original;
        clone.id = clone_id;
        recipe.nodes.push_back(std::move(clone));
        const std::vector<Edge> old_edges = recipe.edges;
        for (const Edge& edge : old_edges) {
            if (edge.to_node == opportunity.node_id) {
                recipe.edges.push_back(Edge{edge.from_node, edge.from_port, clone_id, edge.to_port});
            }
        }
        const Edge selected{
            opportunity.node_id,
            opportunity.from_port,
            opportunity.to_node,
            opportunity.to_port};
        const auto found = std::find_if(recipe.edges.begin(), recipe.edges.end(), [&](const Edge& edge) {
            return edge.from_node == selected.from_node && edge.from_port == selected.from_port &&
                edge.to_node == selected.to_node && edge.to_port == selected.to_port;
        });
        if (found == recipe.edges.end()) {
            return Result<std::string, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::invalid_child, "selected duplicated branch edge disappeared"));
        }
        found->from_node = clone_id;
        return Result<std::string, TopologyMutationError>::success(
            "duplicate " + opportunity.node_id + " as " + clone_id + " for " +
            opportunity.to_node + "." + opportunity.to_port);
    }
    case TopologyOperatorKind::rewire_edge: {
        const auto found = std::find_if(recipe.edges.begin(), recipe.edges.end(), [&](const Edge& edge) {
            return edge.from_node == opportunity.node_id && edge.from_port == opportunity.output_port &&
                edge.to_node == opportunity.to_node && edge.to_port == opportunity.to_port;
        });
        if (found == recipe.edges.end()) {
            return Result<std::string, TopologyMutationError>::failure(
                make_error(TopologyMutationErrorCode::invalid_child, "selected rewire edge disappeared"));
        }
        const std::string old = found->from_node + "." + found->from_port;
        found->from_node = opportunity.from_node;
        found->from_port = opportunity.from_port;
        return Result<std::string, TopologyMutationError>::success(
            "rewire " + opportunity.to_node + "." + opportunity.to_port + ":" + old + "->" +
            opportunity.from_node + "." + opportunity.from_port);
    }
    }
    return Result<std::string, TopologyMutationError>::failure(
        make_error(TopologyMutationErrorCode::invalid_child, "unknown topology opportunity"));
}

[[nodiscard]] Result<void, TopologyMutationError> validate_candidate(
    const Recipe& recipe,
    const NodeRegistry& registry) {
    const auto errors = validate_recipe(recipe, registry);
    if (!errors.empty()) {
        return Result<void, TopologyMutationError>::failure(
            make_error(TopologyMutationErrorCode::invalid_child, validation_summary(errors)));
    }
    return check_resource_limits(recipe, registry);
}

[[nodiscard]] std::string operation_domain_text(
    const std::string_view parent_fingerprint,
    const u32 operator_version,
    const double strength,
    const u32 budget,
    const StructuralLocks& locks) {
    std::string text = "ArtMiner.TopologyMutation.v1\nparent=";
    text.append(parent_fingerprint);
    text += "\noperator=" + std::to_string(operator_version);
    text += "\nstrength=" + format_double(strength);
    text += "\nbudget=" + std::to_string(budget);
    text += "\nlocks=" + locks.serialize_canonical();
    text += "\nlimits=256,512,128,65536\n";
    return text;
}

[[nodiscard]] std::string step_metadata_key(const std::size_t index) {
    std::string digits = std::to_string(index);
    while (digits.size() < 3U) {
        digits.insert(digits.begin(), '0');
    }
    return "artminer.topology.step." + digits;
}

}  // namespace

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

const std::vector<TopologyOperatorDescriptor>& topology_operator_catalog() {
    static const std::vector<TopologyOperatorDescriptor> catalog{
        {TopologyOperatorKind::insert_node, "safe node insertion"},
        {TopologyOperatorKind::replace_node, "compatible-node replacement"},
        {TopologyOperatorKind::delete_bypass, "removable-node deletion/bypass"},
        {TopologyOperatorKind::duplicate_branch, "branch duplication"},
        {TopologyOperatorKind::rewire_edge, "type-compatible edge rewiring"},
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

void StructuralLocks::toggle_node(const std::string_view node_id) {
    set_node(std::string(node_id), !node_locked(node_id));
}

void StructuralLocks::set_upstream_subgraph(
    const Recipe& recipe,
    const std::string_view root_node_id,
    const bool locked) {
    std::set<std::string, std::less<>> closure;
    std::queue<std::string> pending;
    closure.insert(std::string(root_node_id));
    pending.push(std::string(root_node_id));
    while (!pending.empty()) {
        const std::string current = std::move(pending.front());
        pending.pop();
        for (const Edge& edge : recipe.edges) {
            if (edge.to_node == current && closure.insert(edge.from_node).second) {
                pending.push(edge.from_node);
            }
        }
    }
    for (const std::string& node : closure) {
        set_node(node, locked);
    }
}

std::string StructuralLocks::serialize_canonical() const {
    std::string result;
    for (const std::string& node : nodes_) {
        if (!result.empty()) {
            result.push_back(';');
        }
        result += "n/";
        result += node;
    }
    return result;
}

Result<StructuralLocks, TopologyMutationError> parse_structural_locks(const std::string_view text) {
    StructuralLocks locks;
    if (text.empty()) {
        return Result<StructuralLocks, TopologyMutationError>::success(std::move(locks));
    }
    std::size_t position = 0U;
    while (position < text.size()) {
        const std::size_t separator = text.find(';', position);
        const std::size_t end = separator == std::string_view::npos ? text.size() : separator;
        const std::string_view token = text.substr(position, end - position);
        if (token.size() < 3U || !token.starts_with("n/")) {
            return Result<StructuralLocks, TopologyMutationError>::failure(make_error(
                TopologyMutationErrorCode::malformed_provenance,
                "structural lock encoding must contain canonical n/<node-id> tokens"));
        }
        const std::string id(token.substr(2U));
        if (id.empty() || id.size() > 96U ||
            !std::all_of(id.begin(), id.end(), [](const char ch) {
                return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
            })) {
            return Result<StructuralLocks, TopologyMutationError>::failure(make_error(
                TopologyMutationErrorCode::malformed_provenance,
                "structural lock contains an invalid node identifier"));
        }
        locks.set_node(id, true);
        if (separator == std::string_view::npos) {
            break;
        }
        position = separator + 1U;
        if (position == text.size()) {
            return Result<StructuralLocks, TopologyMutationError>::failure(make_error(
                TopologyMutationErrorCode::malformed_provenance,
                "structural lock encoding has a trailing separator"));
        }
    }
    if (locks.serialize_canonical() != text) {
        return Result<StructuralLocks, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::malformed_provenance,
            "structural locks are not in canonical sorted unique form"));
    }
    return Result<StructuralLocks, TopologyMutationError>::success(std::move(locks));
}

Result<TopologyMutationResult, TopologyMutationError> mutate_recipe_topology(
    const Recipe& parent,
    const u64 topology_seed,
    const u32 operator_version,
    const double strength,
    const u32 budget,
    const StructuralLocks& locks,
    const NodeRegistry& registry) {
    if (operator_version != kTopologyMutationOperatorVersion) {
        return Result<TopologyMutationResult, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::unsupported_operator_version,
            "unsupported topology-mutation operator version"));
    }
    if (!std::isfinite(strength) || strength < 0.0 || strength > 1.0) {
        return Result<TopologyMutationResult, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::invalid_strength,
            "topology mutation strength must be finite and within [0,1]"));
    }
    if (budget == 0U || budget > kMaximumTopologyMutationBudget) {
        return Result<TopologyMutationResult, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::invalid_budget,
            "topology mutation budget must be within 1..16"));
    }
    const auto parent_errors = validate_recipe(parent, registry);
    if (!parent_errors.empty()) {
        return Result<TopologyMutationResult, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::invalid_parent,
            "topology parent is invalid: " + validation_summary(parent_errors)));
    }
    auto parent_limits = check_resource_limits(parent, registry);
    if (parent_limits.is_error()) {
        return Result<TopologyMutationResult, TopologyMutationError>::failure(parent_limits.error());
    }
    if (!lock_ids_exist(parent, locks)) {
        return Result<TopologyMutationResult, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::invalid_locks,
            "structural locks reference a node absent from the parent recipe"));
    }

    const std::string parent_fingerprint = semantic_fingerprint(parent);
    const std::string domain_text =
        operation_domain_text(parent_fingerprint, operator_version, strength, budget, locks);
    const u64 operation_root = derive_seed(topology_seed, fnv1a64(domain_text) ^ kTopologyDomain);
    const u32 requested_steps = strength <= 0.0
        ? 0U
        : (std::max)(u32{1U}, static_cast<u32>(std::ceil(strength * static_cast<double>(budget))));

    TopologyMutationResult result;
    result.recipe = parent;
    result.requested_steps = requested_steps;
    erase_operation_metadata(result.recipe);

    for (u32 step = 0U; step < requested_steps; ++step) {
        auto opportunities_result = enumerate_opportunities(result.recipe, locks, registry);
        if (opportunities_result.is_error()) {
            return Result<TopologyMutationResult, TopologyMutationError>::failure(opportunities_result.error());
        }
        const std::vector<Opportunity>& opportunities = opportunities_result.value();
        if (opportunities.empty()) {
            result.exhausted = true;
            break;
        }

        const u64 step_seed = derive_seed(
            operation_root,
            splitmix64(kStepDomain ^ static_cast<u64>(step)));
        const std::size_t start = static_cast<std::size_t>(step_seed % opportunities.size());
        bool accepted = false;
        for (std::size_t attempt = 0U; attempt < opportunities.size(); ++attempt) {
            const Opportunity& opportunity = opportunities[(start + attempt) % opportunities.size()];
            Recipe candidate = result.recipe;
            auto applied = apply_opportunity(candidate, opportunity, operation_root, step, registry);
            if (applied.is_error()) {
                continue;
            }
            auto valid = validate_candidate(candidate, registry);
            if (valid.is_error()) {
                continue;
            }
            result.recipe = std::move(candidate);
            result.steps.push_back(TopologyMutationStep{opportunity.kind, std::move(applied).value()});
            accepted = true;
            break;
        }
        if (!accepted) {
            result.exhausted = true;
            break;
        }
    }

    if (requested_steps > 0U && result.steps.empty()) {
        return Result<TopologyMutationResult, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::no_legal_operation,
            "no legal bounded topology operation exists for this parent/lock configuration"));
    }

    const auto child_errors = validate_recipe(result.recipe, registry);
    if (!child_errors.empty()) {
        return Result<TopologyMutationResult, TopologyMutationError>::failure(make_error(
            TopologyMutationErrorCode::invalid_child,
            "topology child failed normal recipe validation: " + validation_summary(child_errors)));
    }
    auto child_limits = check_resource_limits(result.recipe, registry);
    if (child_limits.is_error()) {
        return Result<TopologyMutationResult, TopologyMutationError>::failure(child_limits.error());
    }

    set_metadata(result.recipe, "artminer.topology.parent", parent_fingerprint);
    set_metadata(result.recipe, "artminer.topology.seed", std::to_string(topology_seed));
    set_metadata(result.recipe, "artminer.topology.operator", std::to_string(operator_version));
    set_metadata(result.recipe, "artminer.topology.strength", format_double(strength));
    set_metadata(result.recipe, "artminer.topology.budget", std::to_string(budget));
    set_metadata(result.recipe, "artminer.topology.locks", locks.serialize_canonical());
    set_metadata(result.recipe, "artminer.topology.requested_steps", std::to_string(requested_steps));
    set_metadata(result.recipe, "artminer.topology.accepted_steps", std::to_string(result.steps.size()));
    set_metadata(result.recipe, "artminer.topology.exhausted", result.exhausted ? "true" : "false");
    for (std::size_t index = 0U; index < result.steps.size(); ++index) {
        set_metadata(
            result.recipe,
            step_metadata_key(index),
            std::string(to_string(result.steps[index].kind)) + "|" + result.steps[index].description);
    }

    return Result<TopologyMutationResult, TopologyMutationError>::success(std::move(result));
}

}  // namespace artminer::core
