#include "platform/windows/browser_store.hpp"

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <utility>

#include "core/graph.hpp"
#include "core/local_text.hpp"

namespace artminer::platform::windows {
namespace {

[[nodiscard]] BrowserStoreError make_error(
    const BrowserStoreErrorCode code,
    std::wstring message,
    const std::error_code system_error = {}) {
    return BrowserStoreError{code, std::move(message), system_error};
}

[[nodiscard]] core::Result<void, BrowserStoreError> write_recipe_atomic(
    const std::filesystem::path& target,
    const core::Recipe& recipe) {
    const auto validation_errors = core::validate_recipe(recipe);
    if (!validation_errors.empty()) {
        return core::Result<void, BrowserStoreError>::failure(
            make_error(BrowserStoreErrorCode::invalid_recipe, L"refusing to persist an invalid recipe"));
    }

    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) {
        return core::Result<void, BrowserStoreError>::failure(
            make_error(BrowserStoreErrorCode::directory_creation_failed, L"could not create recipe directory", error));
    }

    std::filesystem::path temporary = target;
    temporary += L".artminer-tmp";
    std::filesystem::remove(temporary, error);
    error.clear();

    const std::string text = core::serialize_recipe_canonical(recipe);
    if (text.size() > core::kMaximumRecipeFileBytes) {
        return core::Result<void, BrowserStoreError>::failure(
            make_error(BrowserStoreErrorCode::resource_limit, L"canonical recipe exceeds the release file-size limit"));
    }
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return core::Result<void, BrowserStoreError>::failure(
                make_error(BrowserStoreErrorCode::write_failed, L"could not open temporary recipe file for writing"));
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary, error);
            return core::Result<void, BrowserStoreError>::failure(
                make_error(BrowserStoreErrorCode::write_failed, L"failed while writing temporary recipe file"));
        }
    }

    if (MoveFileExW(
            temporary.c_str(),
            target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        const DWORD code = GetLastError();
        std::filesystem::remove(temporary, error);
        return core::Result<void, BrowserStoreError>::failure(
            make_error(
                BrowserStoreErrorCode::replace_failed,
                L"could not atomically replace persisted recipe",
                std::error_code(static_cast<int>(code), std::system_category())));
    }
    return core::Result<void, BrowserStoreError>::success();
}

[[nodiscard]] core::Result<StoredRecipe, BrowserStoreError> load_recipe_file(
    const std::filesystem::path& path) {
    auto text = core::read_local_text_file(path);
    if (text.is_error()) {
        return core::Result<StoredRecipe, BrowserStoreError>::failure(
            make_error(
                BrowserStoreErrorCode::read_failed,
                L"could not read bounded UTF-8 persisted recipe: " + path.wstring()));
    }

    auto parsed = core::parse_recipe(text.value());
    if (parsed.is_error()) {
        return core::Result<StoredRecipe, BrowserStoreError>::failure(
            make_error(BrowserStoreErrorCode::invalid_recipe, L"persisted recipe is malformed"));
    }
    core::Recipe recipe = std::move(parsed).value();
    const auto validation_errors = core::validate_recipe(recipe);
    if (!validation_errors.empty()) {
        return core::Result<StoredRecipe, BrowserStoreError>::failure(
            make_error(BrowserStoreErrorCode::invalid_recipe, L"persisted recipe no longer validates"));
    }
    const std::string fingerprint = core::semantic_fingerprint(recipe);
    return core::Result<StoredRecipe, BrowserStoreError>::success(
        StoredRecipe{path, std::move(recipe), fingerprint});
}

[[nodiscard]] std::filesystem::path recipe_path(
    const std::filesystem::path& recipes_root,
    const std::string_view fingerprint,
    const bool favourite) {
    std::filesystem::path directory = recipes_root;
    if (favourite) {
        directory /= L"favourites";
    } else {
        directory /= L"saved";
    }
    return directory / (std::string(fingerprint) + ".amr");
}

[[nodiscard]] std::filesystem::path recovery_path(const std::filesystem::path& recipes_root) {
    return recipes_root / L"recovery" / L"current.amr";
}

}  // namespace

core::Result<std::filesystem::path, BrowserStoreError> save_recipe_copy(
    const std::filesystem::path& recipes_root,
    const core::Recipe& recipe) {
    const std::string fingerprint = core::semantic_fingerprint(recipe);
    const std::filesystem::path path = recipe_path(recipes_root, fingerprint, false);
    auto written = write_recipe_atomic(path, recipe);
    if (written.is_error()) {
        return core::Result<std::filesystem::path, BrowserStoreError>::failure(written.error());
    }
    return core::Result<std::filesystem::path, BrowserStoreError>::success(path);
}

core::Result<std::filesystem::path, BrowserStoreError> save_favourite(
    const std::filesystem::path& recipes_root,
    const core::Recipe& recipe) {
    const std::string fingerprint = core::semantic_fingerprint(recipe);
    const std::filesystem::path path = recipe_path(recipes_root, fingerprint, true);
    auto written = write_recipe_atomic(path, recipe);
    if (written.is_error()) {
        return core::Result<std::filesystem::path, BrowserStoreError>::failure(written.error());
    }
    return core::Result<std::filesystem::path, BrowserStoreError>::success(path);
}

core::Result<void, BrowserStoreError> remove_favourite(
    const std::filesystem::path& recipes_root,
    const std::string_view fingerprint) {
    const std::filesystem::path path = recipe_path(recipes_root, fingerprint, true);
    std::error_code error;
    (void)std::filesystem::remove(path, error);
    if (error) {
        return core::Result<void, BrowserStoreError>::failure(
            make_error(BrowserStoreErrorCode::remove_failed, L"could not remove persisted favourite recipe", error));
    }
    return core::Result<void, BrowserStoreError>::success();
}

core::Result<std::vector<StoredRecipe>, BrowserStoreError> load_favourites(
    const std::filesystem::path& recipes_root) {
    const std::filesystem::path directory = recipes_root / L"favourites";
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) {
        if (error) {
            return core::Result<std::vector<StoredRecipe>, BrowserStoreError>::failure(
                make_error(BrowserStoreErrorCode::read_failed, L"could not inspect favourites directory", error));
        }
        return core::Result<std::vector<StoredRecipe>, BrowserStoreError>::success({});
    }

    std::vector<std::filesystem::path> paths;
    std::filesystem::directory_iterator iterator(directory, error);
    if (error) {
        return core::Result<std::vector<StoredRecipe>, BrowserStoreError>::failure(
            make_error(BrowserStoreErrorCode::read_failed, L"could not enumerate favourites directory", error));
    }
    for (const auto& entry : iterator) {
        if (entry.is_regular_file(error) && !error && entry.path().extension() == L".amr") {
            if (paths.size() >= kMaximumPersistedFavouriteRecipes) {
                return core::Result<std::vector<StoredRecipe>, BrowserStoreError>::failure(
                    make_error(
                        BrowserStoreErrorCode::resource_limit,
                        L"favourites directory exceeds the 4096-recipe release safety limit"));
            }
            paths.push_back(entry.path());
        }
        if (error) {
            return core::Result<std::vector<StoredRecipe>, BrowserStoreError>::failure(
                make_error(BrowserStoreErrorCode::read_failed, L"could not inspect favourite recipe entry", error));
        }
    }
    std::sort(paths.begin(), paths.end());

    std::vector<StoredRecipe> recipes;
    recipes.reserve(paths.size());
    for (const auto& path : paths) {
        auto loaded = load_recipe_file(path);
        if (loaded.is_error()) {
            return core::Result<std::vector<StoredRecipe>, BrowserStoreError>::failure(loaded.error());
        }
        recipes.push_back(std::move(loaded).value());
    }
    std::sort(recipes.begin(), recipes.end(), [](const StoredRecipe& left, const StoredRecipe& right) {
        return left.fingerprint < right.fingerprint;
    });
    return core::Result<std::vector<StoredRecipe>, BrowserStoreError>::success(std::move(recipes));
}

core::Result<std::filesystem::path, BrowserStoreError> save_session_recovery(
    const std::filesystem::path& recipes_root,
    const core::Recipe& recipe) {
    const std::filesystem::path path = recovery_path(recipes_root);
    auto written = write_recipe_atomic(path, recipe);
    if (written.is_error()) {
        return core::Result<std::filesystem::path, BrowserStoreError>::failure(written.error());
    }
    return core::Result<std::filesystem::path, BrowserStoreError>::success(path);
}

core::Result<std::optional<StoredRecipe>, BrowserStoreError> load_session_recovery(
    const std::filesystem::path& recipes_root) {
    const std::filesystem::path path = recovery_path(recipes_root);
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        return core::Result<std::optional<StoredRecipe>, BrowserStoreError>::failure(
            make_error(BrowserStoreErrorCode::read_failed, L"could not inspect session recovery snapshot", error));
    }
    if (!exists) {
        return core::Result<std::optional<StoredRecipe>, BrowserStoreError>::success(std::nullopt);
    }
    auto loaded = load_recipe_file(path);
    if (loaded.is_error()) {
        return core::Result<std::optional<StoredRecipe>, BrowserStoreError>::failure(loaded.error());
    }
    return core::Result<std::optional<StoredRecipe>, BrowserStoreError>::success(
        std::optional<StoredRecipe>{std::move(loaded).value()});
}

}  // namespace artminer::platform::windows
