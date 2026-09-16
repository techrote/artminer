#pragma once

#include <filesystem>

namespace artminer::app {

[[nodiscard]] int run_quarry_application(const std::filesystem::path& manifest_path);

}  // namespace artminer::app
