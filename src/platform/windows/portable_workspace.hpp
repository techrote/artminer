#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "core/result.hpp"

namespace artminer::platform::windows {

enum class WorkspaceErrorCode {
    invalid_root,
    executable_path_unavailable,
    path_resolution_failed,
    directory_creation_failed,
    not_writable,
};

struct WorkspaceError {
    WorkspaceErrorCode code{};
    std::wstring message;
    std::error_code system_error;
};

struct WorkspaceLayout {
    std::filesystem::path root;
    std::filesystem::path recipes;
    std::filesystem::path palettes;
    std::filesystem::path output;
    std::filesystem::path cache;
};

class PortableWorkspace final {
public:
    [[nodiscard]] static core::Result<PortableWorkspace, WorkspaceError> open(
        const std::filesystem::path& root);

    [[nodiscard]] static core::Result<PortableWorkspace, WorkspaceError> from_executable(
        const std::optional<std::filesystem::path>& override_root = std::nullopt);

    [[nodiscard]] core::Result<void, WorkspaceError> ensure_layout() const;

    [[nodiscard]] const WorkspaceLayout& layout() const noexcept { return layout_; }

private:
    explicit PortableWorkspace(WorkspaceLayout layout) : layout_(std::move(layout)) {}

    WorkspaceLayout layout_;
};

}  // namespace artminer::platform::windows
