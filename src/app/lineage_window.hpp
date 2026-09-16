#pragma once

#include <filesystem>
#include <optional>

#include "platform/windows/portable_workspace.hpp"

namespace artminer::app {

int run_lineage_application(
    const platform::windows::WorkspaceLayout& workspace,
    const std::optional<std::filesystem::path>& initial_parent_a = std::nullopt,
    const std::optional<std::filesystem::path>& initial_parent_b = std::nullopt);

}  // namespace artminer::app
