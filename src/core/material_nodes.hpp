#pragma once

#include <vector>

#include "core/graph.hpp"

namespace artminer::core {

// AM-013 material/loop nodes use the ordinary graph type system, but are
// evaluated by the dedicated workflow evaluator rather than the AM-003 static
// evaluator. This keeps semantic conversions explicit without silently
// reinterpreting arbitrary colour images as physical data.
void append_material_node_metadata(std::vector<NodeMetadata>& nodes);

}  // namespace artminer::core
