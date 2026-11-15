#include "gui_platform.hpp"
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>

#include <array>

namespace archive_catalog {
namespace {

std::string utf8(const wchar_t* text) {
    auto length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    result.pop_back();
    return result;
}

}

std::string choose_file(bool xml) {
    std::array<wchar_t, 32768> path{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrFilter = xml ? L"XML catalogs\0*.xml\0All files\0*.*\0" : L"All files\0*.*\0";
    dialog.lpstrTitle = xml ? L"Open catalog" : L"Choose local file";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? utf8(path.data()) : std::string{};
}

std::string choose_directory(const std::string& initial) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) throw std::runtime_error("Cannot initialize the folder picker");
    struct Apartment { bool owned; ~Apartment() { if (owned) CoUninitialize(); } } apartment{SUCCEEDED(initialized)};
    IFileDialog* dialog = nullptr;
    std::string result;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        throw std::runtime_error("Cannot create the folder picker");
    struct Dialog { IFileDialog* value; ~Dialog() { value->Release(); } } owned{dialog};
    DWORD options = 0;
    if (FAILED(dialog->GetOptions(&options)) || FAILED(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM)))
        throw std::runtime_error("Cannot configure the folder picker");
    IShellItem* folder = nullptr;
    HRESULT resolved;
    if (initial.empty()) resolved = SHCreateItemInKnownFolder(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder));
    else {
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, initial.c_str(), -1, nullptr, 0);
        if (!length) throw std::runtime_error("The folder path is not valid UTF-8");
        std::wstring path(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, initial.c_str(), -1, path.data(), length);
        resolved = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&folder));
    }
    if (FAILED(resolved)) throw std::runtime_error("Cannot open that folder in the picker; check the full path");
    const auto positioned = dialog->SetFolder(folder);
    folder->Release();
    if (FAILED(positioned)) throw std::runtime_error("Cannot set the folder picker's starting location");
    const auto shown = dialog->Show(GetActiveWindow());
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    if (FAILED(shown)) throw std::runtime_error("Cannot show the folder picker");
    IShellItem* item = nullptr;
    if (FAILED(dialog->GetResult(&item))) throw std::runtime_error("Cannot read the selected folder");
    PWSTR path = nullptr;
    const auto selected = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    item->Release();
    if (FAILED(selected)) throw std::runtime_error("Cannot read the selected folder's full path");
    result = utf8(path);
    CoTaskMemFree(path);
    return result;
}

}
#else
namespace archive_catalog {
std::string choose_file(bool) { return {}; }
std::string choose_directory(const std::string&) { throw std::runtime_error("Use the storage browser to choose a folder on this platform"); }
}
#endif