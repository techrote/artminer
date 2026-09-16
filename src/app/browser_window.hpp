#pragma once

#include <filesystem>
#include <optional>

#include "platform/windows/portable_workspace.hpp"

namespace artminer::app {

[[nodiscard]] int run_browser_application(
    const platform::windows::WorkspaceLayout& workspace,
    const std::optional<std::filesystem::path>& initial_recipe);

}  // namespace artminer::app
