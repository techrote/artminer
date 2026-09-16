#pragma once

#include <vector>

#include "core/graph.hpp"

namespace artminer::core {

// Append the AM-006 self-contained deterministic growth node metadata.
// Kept in core so mutation, validation and every execution surface consume one
// authoritative parameter/port/version contract.
void append_growth_node_metadata(std::vector<NodeMetadata>& nodes);

}  // namespace artminer::core
