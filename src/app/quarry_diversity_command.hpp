#pragma once

#include <optional>

namespace artminer::app {

// Returns nullopt when argv[2] is not an AM-011 Quarry analysis action.
[[nodiscard]] std::optional<int> try_run_quarry_diversity_command(int argc, wchar_t* argv[]);

}  // namespace artminer::app
