#include "platform/windows/portable_workspace.hpp"

#include <Windows.h>

#include <limits>
#include <utility>
#include <vector>

namespace artminer::platform::windows {
namespace {

[[nodiscard]] WorkspaceError make_error(
    const WorkspaceErrorCode code,
    std::wstring message,
    const std::error_code system_error = {}) {
    return WorkspaceError{code, std::move(message), system_error};
}

[[nodiscard]] std::error_code last_system_error() {
    return std::error_code(static_cast<int>(GetLastError()), std::system_category());
}

[[nodiscard]] core::Result<std::filesystem::path, WorkspaceError> executable_directory() {
    std::vector<wchar_t> buffer(512U, L'\0');

    for (;;) {
        if (buffer.size() > 32768U || buffer.size() > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())) {
            return core::Result<std::filesystem::path, WorkspaceError>::failure(make_error(
                WorkspaceErrorCode::executable_path_unavailable,
                L"Executable path exceeded the supported Windows path buffer."));
        }

        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U) {
            return core::Result<std::filesystem::path, WorkspaceError>::failure(make_error(
                WorkspaceErrorCode::executable_path_unavailable,
                L"Windows could not resolve the ArtMiner executable path.",
                last_system_error()));
        }

        if (static_cast<std::size_t>(length) < buffer.size() - 1U) {
            const std::filesystem::path executable_path(
                std::wstring(buffer.data(), static_cast<std::size_t>(length)));
            return core::Result<std::filesystem::path, WorkspaceError>::success(executable_path.parent_path());
        }

        buffer.resize(buffer.size() * 2U, L'\0');
    }
}

[[nodiscard]] core::Result<void, WorkspaceError> create_directory_checked(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) {
        return core::Result<void, WorkspaceError>::failure(make_error(
            WorkspaceErrorCode::directory_creation_failed,
            L"Could not create workspace directory: " + path.wstring(),
            error));
    }

    if (!std::filesystem::is_directory(path, error) || error) {
        return core::Result<void, WorkspaceError>::failure(make_error(
            WorkspaceErrorCode::directory_creation_failed,
            L"Workspace path is not a directory: " + path.wstring(),
            error));
    }

    return core::Result<void, WorkspaceError>::success();
}

[[nodiscard]] core::Result<void, WorkspaceError> verify_writable(const std::filesystem::path& root) {
    const std::filesystem::path probe_path = root / L".artminer-write-probe";
    const HANDLE handle = CreateFileW(
        probe_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE) {
        return core::Result<void, WorkspaceError>::failure(make_error(
            WorkspaceErrorCode::not_writable,
            L"Workspace is not writable: " + root.wstring(),
            last_system_error()));
    }

    if (CloseHandle(handle) == 0) {
        return core::Result<void, WorkspaceError>::failure(make_error(
            WorkspaceErrorCode::not_writable,
            L"Workspace write probe could not be closed cleanly: " + root.wstring(),
            last_system_error()));
    }

    return core::Result<void, WorkspaceError>::success();
}

}  // namespace

core::Result<PortableWorkspace, WorkspaceError> PortableWorkspace::open(const std::filesystem::path& root) {
    if (root.empty()) {
        return core::Result<PortableWorkspace, WorkspaceError>::failure(make_error(
            WorkspaceErrorCode::invalid_root,
            L"Workspace root must not be empty."));
    }

    std::error_code error;
    auto absolute_root = std::filesystem::absolute(root, error);
    if (error) {
        return core::Result<PortableWorkspace, WorkspaceError>::failure(make_error(
            WorkspaceErrorCode::path_resolution_failed,
            L"Could not resolve workspace root: " + root.wstring(),
            error));
    }

    absolute_root = absolute_root.lexically_normal();
    WorkspaceLayout layout{
        absolute_root,
        absolute_root / L"recipes",
        absolute_root / L"palettes",
        absolute_root / L"output",
        absolute_root / L"cache",
    };

    return core::Result<PortableWorkspace, WorkspaceError>::success(PortableWorkspace(std::move(layout)));
}

core::Result<PortableWorkspace, WorkspaceError> PortableWorkspace::from_executable(
    const std::optional<std::filesystem::path>& override_root) {
    if (override_root.has_value()) {
        return open(*override_root);
    }

    auto executable_root = executable_directory();
    if (executable_root.is_error()) {
        return core::Result<PortableWorkspace, WorkspaceError>::failure(executable_root.error());
    }
    return open(executable_root.value());
}

core::Result<void, WorkspaceError> PortableWorkspace::ensure_layout() const {
    const std::filesystem::path directories[] = {
        layout_.root,
        layout_.recipes,
        layout_.palettes,
        layout_.output,
        layout_.cache,
    };

    for (const auto& directory : directories) {
        auto created = create_directory_checked(directory);
        if (created.is_error()) {
            return created;
        }
    }

    return verify_writable(layout_.root);
}

}  // namespace artminer::platform::windows
