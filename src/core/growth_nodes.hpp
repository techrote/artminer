#pragma once

#include <vector>

#include "core/graph.hpp"

namespace artminer::core {

// Append the project-owned fixed-tick/stateful node metadata introduced by
// AM-006 and AM-007. Kept in core so validation, mutation, UI and every
// execution surface consume one authoritative parameter/port/version contract.
void append_growth_node_metadata(std::vector<NodeMetadata>& nodes);

}  // namespace artminer::core
