#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/recipe.hpp"
#include "core/result.hpp"

namespace artminer::platform::windows {

inline constexpr std::size_t kMaximumPersistedFavouriteRecipes = 4096U;

enum class BrowserStoreErrorCode {
    directory_creation_failed,
    write_failed,
    replace_failed,
    read_failed,
    invalid_recipe,
    resource_limit,
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

// The recovery snapshot is authoritative only as an explicitly workspace-owned
// recipe snapshot. It is never stored in cache and never changes recipe semantics.
// Malformed/corrupt recovery data is reported to the caller rather than guessed.
[[nodiscard]] core::Result<std::filesystem::path, BrowserStoreError> save_session_recovery(
    const std::filesystem::path& recipes_root,
    const core::Recipe& recipe);

[[nodiscard]] core::Result<std::optional<StoredRecipe>, BrowserStoreError> load_session_recovery(
    const std::filesystem::path& recipes_root);

}  // namespace artminer::platform::windows
