#include "windows_update.hpp"
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Function> void refused(Function function) {
    archive_catalog::windows_update_handoff_started.store(false);
    bool failed = false;
    try { function(); } catch (const std::exception&) { failed = true; }
    require(failed, "Expected an explicit installer launch error");
    require(!archive_catalog::windows_update_handoff_started.load(), "A failed launch must not request app shutdown");
}
}

int main() {
    std::vector<wchar_t> module(32768);
    const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!length || length == module.size()) return 2;
    const std::filesystem::path executable(std::wstring(module.data(), length));
    if (executable.filename().wstring() == L"verified setup & \u03a9.exe") {
        std::ofstream(executable.parent_path() / "handoff.txt") << "started";
        return 0;
    }
    const auto root = std::filesystem::temp_directory_path() /
        ("archivefetch-installer-adapter-" + std::to_string(GetCurrentProcessId()));
    try {
        require(std::filesystem::create_directory(root), "Test directory must be new");
        const auto installer = root / L"verified setup & \u03a9.exe";
        std::filesystem::copy_file(executable, installer);
        using archive_catalog::start_windows_update;
        refused([&] { start_windows_update(installer, "v0.8.0"); });
        refused([&] { start_windows_update("relative.exe", "0.8.0"); });
        refused([&] { start_windows_update(root / "missing.exe", "0.8.0"); });
        refused([&] { start_windows_update(root / "file.cmd", "0.8.0"); });
        const auto invalid = root / "invalid.exe";
        std::ofstream(invalid) << "not a portable executable";
        refused([&] { start_windows_update(invalid, "0.8.0"); });
        start_windows_update(installer, "0.8.0-beta.4");
        require(archive_catalog::windows_update_handoff_started.load(), "Successful handoff must request app shutdown");
        for (int attempt = 0; attempt < 100 && !std::filesystem::exists(root / "handoff.txt"); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        require(std::filesystem::exists(root / "handoff.txt"), "Exact Unicode/spaced installer did not execute");
        for (int attempt = 0; attempt < 100; ++attempt) {
            std::error_code error;
            if (std::filesystem::remove(installer, error)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::filesystem::remove(root / "handoff.txt");
        std::filesystem::remove(invalid);
        require(std::filesystem::remove(root), "Test fixture cleanup failed");
        std::cout << "Windows installer adapter checks passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
