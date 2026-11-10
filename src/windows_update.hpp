#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "version.hpp"
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace archive_catalog {

inline std::atomic<bool> windows_update_handoff_started{false};

// The caller must reverify the staged bytes and refuse installation while work is active.
inline void start_windows_update(const std::filesystem::path& path, const std::string& version) {
    if (!canonical_version(version)) throw std::runtime_error("Invalid update version");
    const auto filename = path.native();
    auto extension = path.extension().native();
    for (auto& character : extension) character = static_cast<wchar_t>(std::towlower(character));
    if (!path.is_absolute() || filename.find(L'\0') != std::wstring::npos || extension != L".exe")
        throw std::runtime_error("The verified Windows update must be an absolute installer executable path");

    HANDLE file = CreateFileW(filename.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot open the verified update installer");
    struct CloseHandleOnExit {
        HANDLE value;
        ~CloseHandleOnExit() { CloseHandle(value); }
    } opened{file};
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file, &information))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot inspect the verified update installer");
    if (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("The verified update installer must be a regular file, not a link");

    std::wstring command = L"\"" + filename + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto directory = path.parent_path().native();
    if (!CreateProcessW(filename.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
            directory.c_str(), &startup, &process))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot start the verified update installer");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    windows_update_handoff_started.store(true);
}

}
#endif
