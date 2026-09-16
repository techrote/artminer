#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/recipe.hpp"
#include "core/result.hpp"

namespace artminer::platform::windows {

enum class BrowserStoreErrorCode {
    directory_creation_failed,
    write_failed,
    replace_failed,
    read_failed,
    invalid_recipe,
    remove_failed,
};

struct BrowserStoreError {
    BrowserStoreErrorCode code{BrowserStoreErrorCode::write_failed};
    std::wstring message;
    std::error_code system_error;
};

struct StoredRecipe {
    std::filesystem::path path;
    core::Recipe recipe;
    std::string fingerprint;
};

[[nodiscard]] core::Result<std::filesystem::path, BrowserStoreError> save_recipe_copy(
    const std::filesystem::path& recipes_root,
    const core::Recipe& recipe);

[[nodiscard]] core::Result<std::filesystem::path, BrowserStoreError> save_favourite(
    const std::filesystem::path& recipes_root,
    const core::Recipe& recipe);

[[nodiscard]] core::Result<void, BrowserStoreError> remove_favourite(
    const std::filesystem::path& recipes_root,
    std::string_view fingerprint);

[[nodiscard]] core::Result<std::vector<StoredRecipe>, BrowserStoreError> load_favourites(
    const std::filesystem::path& recipes_root);

}  // namespace artminer::platform::windows
