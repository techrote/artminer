#pragma once

#include <vector>

#include "core/graph.hpp"

namespace artminer::core {

// Appends the semantic recipe settings owned by the AM-012 glyph/terminal
// output stage. The node is intentionally disconnected from image evaluation:
// it records canonical glyph synthesis settings in the recipe so normal
// fingerprint, mutation and breeding machinery owns their identity.
void append_glyph_node_metadata(std::vector<NodeMetadata>& nodes);

}  // namespace artminer::core
