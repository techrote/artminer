#include "platform/windows/lineage_store.hpp"

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

#include "core/graph.hpp"

namespace artminer::platform::windows {
namespace {

[[nodiscard]] LineageStoreError make_error(
    const LineageStoreErrorCode code,
    std::wstring message,
    const std::error_code system_error = {}) {
    return LineageStoreError{code, std::move(message), system_error};
}

[[nodiscard]] core::Result<void, LineageStoreError> write_text_atomic(
    const std::filesystem::path& target,
    const std::string_view text) {
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) {
        return core::Result<void, LineageStoreError>::failure(
            make_error(LineageStoreErrorCode::directory_creation_failed, L"could not create lineage directory", error));
    }

    std::filesystem::path temporary = target;
    temporary += L".artminer-tmp";
    std::filesystem::remove(temporary, error);
    error.clear();
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return core::Result<void, LineageStoreError>::failure(
                make_error(LineageStoreErrorCode::write_failed, L"could not open temporary lineage file"));
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary, error);
            return core::Result<void, LineageStoreError>::failure(
                make_error(LineageStoreErrorCode::write_failed, L"failed while writing temporary lineage file"));
        }
    }

    if (MoveFileExW(
            temporary.c_str(),
            target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        const DWORD code = GetLastError();
        std::filesystem::remove(temporary, error);
        return core::Result<void, LineageStoreError>::failure(make_error(
            LineageStoreErrorCode::replace_failed,
            L"could not atomically replace persisted lineage file",
            std::error_code(static_cast<int>(code), std::system_category())));
    }
    return core::Result<void, LineageStoreError>::success();
}

[[nodiscard]] core::Result<std::string, LineageStoreError> read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return core::Result<std::string, LineageStoreError>::failure(
            make_error(LineageStoreErrorCode::read_failed, L"could not open persisted lineage file"));
    }
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        return core::Result<std::string, LineageStoreError>::failure(
            make_error(LineageStoreErrorCode::read_failed, L"failed while reading persisted lineage file"));
    }
    return core::Result<std::string, LineageStoreError>::success(std::move(text));
}

[[nodiscard]] std::filesystem::path lineage_root(const std::filesystem::path& recipes_root) {
    return recipes_root / L"lineage";
}

[[nodiscard]] std::filesystem::path record_path(
    const std::filesystem::path& recipes_root,
    const std::string_view child_fingerprint) {
    return lineage_root(recipes_root) / L"records" / (std::string(child_fingerprint) + ".aml");
}

[[nodiscard]] std::filesystem::path specimen_path(
    const std::filesystem::path& recipes_root,
    const std::string_view fingerprint) {
    return lineage_root(recipes_root) / L"specimens" / (std::string(fingerprint) + ".amr");
}

[[nodiscard]] core::Result<StoredLineageSpecimen, LineageStoreError> load_specimen_file(
    const std::filesystem::path& path) {
    auto text = read_text(path);
    if (text.is_error()) {
        return core::Result<StoredLineageSpecimen, LineageStoreError>::failure(text.error());
    }
    auto parsed = core::parse_recipe(text.value());
    if (parsed.is_error()) {
        return core::Result<StoredLineageSpecimen, LineageStoreError>::failure(
            make_error(LineageStoreErrorCode::invalid_recipe, L"persisted lineage specimen is malformed"));
    }
    core::Recipe recipe = std::move(parsed).value();
    const auto errors = core::validate_recipe(recipe);
    if (!errors.empty()) {
        return core::Result<StoredLineageSpecimen, LineageStoreError>::failure(
            make_error(LineageStoreErrorCode::invalid_recipe, L"persisted lineage specimen no longer validates"));
    }
    const std::string fingerprint = core::semantic_fingerprint(recipe);
    if (path.stem().string() != fingerprint) {
        return core::Result<StoredLineageSpecimen, LineageStoreError>::failure(
            make_error(LineageStoreErrorCode::invalid_recipe, L"lineage specimen filename does not match its semantic fingerprint"));
    }
    return core::Result<StoredLineageSpecimen, LineageStoreError>::success(
        StoredLineageSpecimen{path, std::move(recipe), fingerprint});
}

[[nodiscard]] core::Result<std::vector<std::filesystem::path>, LineageStoreError> enumerate(
    const std::filesystem::path& directory,
    const std::wstring_view extension) {
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) {
        if (error) {
            return core::Result<std::vector<std::filesystem::path>, LineageStoreError>::failure(
                make_error(LineageStoreErrorCode::read_failed, L"could not inspect lineage directory", error));
        }
        return core::Result<std::vector<std::filesystem::path>, LineageStoreError>::success({});
    }
    std::vector<std::filesystem::path> paths;
    std::filesystem::directory_iterator iterator(directory, error);
    if (error) {
        return core::Result<std::vector<std::filesystem::path>, LineageStoreError>::failure(
            make_error(LineageStoreErrorCode::read_failed, L"could not enumerate lineage directory", error));
    }
    for (const auto& entry : iterator) {
        if (entry.is_regular_file(error) && !error && entry.path().extension() == extension) {
            paths.push_back(entry.path());
        }
        if (error) {
            return core::Result<std::vector<std::filesystem::path>, LineageStoreError>::failure(
                make_error(LineageStoreErrorCode::read_failed, L"could not inspect lineage entry", error));
        }
    }
    std::sort(paths.begin(), paths.end());
    return core::Result<std::vector<std::filesystem::path>, LineageStoreError>::success(std::move(paths));
}

}  // namespace

core::Result<std::filesystem::path, LineageStoreError> save_lineage_record(
    const std::filesystem::path& recipes_root,
    const core::LineageRecord& record) {
    if (record.child_fingerprint.empty()) {
        return core::Result<std::filesystem::path, LineageStoreError>::failure(
            make_error(LineageStoreErrorCode::invalid_record, L"lineage record has no child fingerprint"));
    }
    const std::filesystem::path path = record_path(recipes_root, record.child_fingerprint);
    const std::string text = core::serialize_lineage_record(record);
    auto written = write_text_atomic(path, text);
    if (written.is_error()) {
        return core::Result<std::filesystem::path, LineageStoreError>::failure(written.error());
    }
    return core::Result<std::filesystem::path, LineageStoreError>::success(path);
}

core::Result<std::filesystem::path, LineageStoreError> save_lineage_specimen(
    const std::filesystem::path& recipes_root,
    const core::Recipe& recipe) {
    const auto errors = core::validate_recipe(recipe);
    if (!errors.empty()) {
        return core::Result<std::filesystem::path, LineageStoreError>::failure(
            make_error(LineageStoreErrorCode::invalid_recipe, L"cannot persist an invalid lineage specimen"));
    }
    const std::string fingerprint = core::semantic_fingerprint(recipe);
    const std::filesystem::path path = specimen_path(recipes_root, fingerprint);
    const std::string text = core::serialize_recipe_canonical(recipe);
    auto written = write_text_atomic(path, text);
    if (written.is_error()) {
        return core::Result<std::filesystem::path, LineageStoreError>::failure(written.error());
    }
    return core::Result<std::filesystem::path, LineageStoreError>::success(path);
}

core::Result<std::vector<StoredLineageRecord>, LineageStoreError> load_lineage_records(
    const std::filesystem::path& recipes_root) {
    auto paths = enumerate(lineage_root(recipes_root) / L"records", L".aml");
    if (paths.is_error()) {
        return core::Result<std::vector<StoredLineageRecord>, LineageStoreError>::failure(paths.error());
    }
    std::vector<StoredLineageRecord> result;
    result.reserve(paths.value().size());
    for (const auto& path : paths.value()) {
        auto text = read_text(path);
        if (text.is_error()) {
            return core::Result<std::vector<StoredLineageRecord>, LineageStoreError>::failure(text.error());
        }
        auto parsed = core::parse_lineage_record(text.value());
        if (parsed.is_error() || path.stem().string() != (parsed.is_ok() ? parsed.value().child_fingerprint : std::string{})) {
            return core::Result<std::vector<StoredLineageRecord>, LineageStoreError>::failure(
                make_error(LineageStoreErrorCode::invalid_record, L"persisted lineage record is malformed or misnamed"));
        }
        result.push_back(StoredLineageRecord{path, std::move(parsed).value()});
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.record.child_fingerprint < right.record.child_fingerprint;
    });
    return core::Result<std::vector<StoredLineageRecord>, LineageStoreError>::success(std::move(result));
}

core::Result<std::vector<StoredLineageSpecimen>, LineageStoreError> load_lineage_specimens(
    const std::filesystem::path& recipes_root) {
    auto paths = enumerate(lineage_root(recipes_root) / L"specimens", L".amr");
    if (paths.is_error()) {
        return core::Result<std::vector<StoredLineageSpecimen>, LineageStoreError>::failure(paths.error());
    }
    std::vector<StoredLineageSpecimen> result;
    result.reserve(paths.value().size());
    for (const auto& path : paths.value()) {
        auto loaded = load_specimen_file(path);
        if (loaded.is_error()) {
            return core::Result<std::vector<StoredLineageSpecimen>, LineageStoreError>::failure(loaded.error());
        }
        result.push_back(std::move(loaded).value());
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.fingerprint < right.fingerprint;
    });
    return core::Result<std::vector<StoredLineageSpecimen>, LineageStoreError>::success(std::move(result));
}

core::Result<StoredLineageSpecimen, LineageStoreError> load_lineage_specimen(
    const std::filesystem::path& recipes_root,
    const std::string_view fingerprint) {
    const std::filesystem::path path = specimen_path(recipes_root, fingerprint);
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (error) {
            return core::Result<StoredLineageSpecimen, LineageStoreError>::failure(
                make_error(LineageStoreErrorCode::read_failed, L"could not inspect lineage specimen", error));
        }
        return core::Result<StoredLineageSpecimen, LineageStoreError>::failure(
            make_error(LineageStoreErrorCode::not_found, L"lineage parent/specimen snapshot is unavailable"));
    }
    return load_specimen_file(path);
}

}  // namespace artminer::platform::windows
