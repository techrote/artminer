#include "core/graph.hpp"

#include <algorithm>
#include <utility>

namespace artminer::core {

std::string_view to_string(const DataKind kind) noexcept {
    switch (kind) {
    case DataKind::scalar_field:
        return "ScalarField";
    case DataKind::vector_field:
        return "VectorField";
    case DataKind::colour_field:
        return "ColourField";
    case DataKind::mask:
        return "Mask";
    case DataKind::particle_set:
        return "ParticleSet";
    case DataKind::palette:
        return "Palette";
    case DataKind::image:
        return "Image";
    }
    return "Unknown";
}

std::string_view to_string(const ParameterKind kind) noexcept {
    switch (kind) {
    case ParameterKind::integer:
        return "i64";
    case ParameterKind::real:
        return "f64";
    case ParameterKind::boolean:
        return "bool";
    case ParameterKind::enumeration:
        return "enum";
    }
    return "unknown";
}

NodeRegistry::NodeRegistry(std::vector<NodeMetadata> nodes) : nodes_(std::move(nodes)) {
    std::sort(nodes_.begin(), nodes_.end(), [](const NodeMetadata& left, const NodeMetadata& right) {
        return left.type_id < right.type_id;
    });
}

const NodeMetadata* NodeRegistry::find(const std::string_view type_id) const noexcept {
    const auto found = std::lower_bound(
        nodes_.begin(),
        nodes_.end(),
        type_id,
        [](const NodeMetadata& node, const std::string_view value) { return node.type_id < value; });
    if (found == nodes_.end() || found->type_id != type_id) {
        return nullptr;
    }
    return &*found;
}

}  // namespace artminer::core
