#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/breeding.hpp"
#include "core/recipe.hpp"
#include "core/result.hpp"

namespace artminer::platform::windows {

enum class LineageStoreErrorCode {
    directory_creation_failed,
    write_failed,
    replace_failed,
    read_failed,
    invalid_record,
    invalid_recipe,
    not_found,
};

struct LineageStoreError {
    LineageStoreErrorCode code{LineageStoreErrorCode::write_failed};
    std::wstring message;
    std::error_code system_error;
};

struct StoredLineageRecord {
    std::filesystem::path path;
    core::LineageRecord record;
};

struct StoredLineageSpecimen {
    std::filesystem::path path;
    core::Recipe recipe;
    std::string fingerprint;
};

[[nodiscard]] core::Result<std::filesystem::path, LineageStoreError> save_lineage_record(
    const std::filesystem::path& recipes_root,
    const core::LineageRecord& record);

[[nodiscard]] core::Result<std::filesystem::path, LineageStoreError> save_lineage_specimen(
    const std::filesystem::path& recipes_root,
    const core::Recipe& recipe);

[[nodiscard]] core::Result<std::vector<StoredLineageRecord>, LineageStoreError> load_lineage_records(
    const std::filesystem::path& recipes_root);

[[nodiscard]] core::Result<std::vector<StoredLineageSpecimen>, LineageStoreError> load_lineage_specimens(
    const std::filesystem::path& recipes_root);

[[nodiscard]] core::Result<StoredLineageSpecimen, LineageStoreError> load_lineage_specimen(
    const std::filesystem::path& recipes_root,
    std::string_view fingerprint);

}  // namespace artminer::platform::windows
