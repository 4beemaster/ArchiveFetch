#include "credential_store.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dpapi.h>
#include <shlobj.h>
#elif defined(PS5_PAYLOAD)
#include <fcntl.h>
#include <iterator>
#include <unistd.h>
#endif

namespace archive_catalog {

CredentialStore::CredentialStore(std::filesystem::path path) : path_(std::move(path)) {}

bool CredentialStore::supported() {
#if defined(_WIN32) || defined(PS5_PAYLOAD)
    return true;
#else
    return false;
#endif
}

bool CredentialStore::protected_storage() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

std::filesystem::path CredentialStore::default_path() {
#ifdef _WIN32
    PWSTR directory = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &directory))) {
        throw std::runtime_error("Cannot locate Windows credential storage");
    }
    std::filesystem::path result(directory);
    CoTaskMemFree(directory);
    return result / "ArchiveCatalog" / "credentials.bin";
#else
    throw std::runtime_error("Saved credentials are not supported on this platform");
#endif
}

#ifdef _WIN32
namespace {

struct ProtectedData {
    DATA_BLOB blob{};
    ~ProtectedData() {
        if (blob.pbData) {
            SecureZeroMemory(blob.pbData, blob.cbData);
            LocalFree(blob.pbData);
        }
    }
};

struct Plaintext {
    std::vector<unsigned char> bytes;
    ~Plaintext() {
        if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size());
    }
};

}
#elif defined(PS5_PAYLOAD)
namespace {

const std::string plain_prefix = "ArchiveCatalog-S3-plain-v1\n";

void wipe(std::string& text) {
    volatile char* data = text.data();
    for (std::size_t index = 0; index < text.size(); ++index) data[index] = 0;
    text.clear();
}

}
#endif

std::optional<S3Credentials> CredentialStore::load() const {
#ifdef _WIN32
    if (!std::filesystem::exists(path_)) return std::nullopt;
    std::ifstream input(path_, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot read saved credentials");
    auto size = input.tellg();
    if (size <= 0 || size > 65536) throw std::runtime_error("Invalid saved credentials");
    std::vector<unsigned char> encrypted(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(encrypted.data()), size)) {
        throw std::runtime_error("Cannot read saved credentials");
    }
    DATA_BLOB source{static_cast<DWORD>(encrypted.size()), encrypted.data()};
    ProtectedData decoded;
    if (!CryptUnprotectData(&source, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &decoded.blob)) {
        throw std::runtime_error("Saved credentials cannot be unlocked; enter your keys again or forget them");
    }
    const auto* begin = reinterpret_cast<const char*>(decoded.blob.pbData);
    const auto* end = begin + decoded.blob.cbData;
    const std::string prefix = "ArchiveCatalog-S3-v1\n";
    if (decoded.blob.cbData > 2200 || decoded.blob.cbData < prefix.size() ||
        !std::equal(prefix.begin(), prefix.end(), begin)) throw std::runtime_error("Invalid saved credentials");
    const auto* separator = std::find(begin + prefix.size(), end, '\n');
    if (separator == end) throw std::runtime_error("Invalid saved credentials");
    S3Credentials credentials{std::string(begin + prefix.size(), separator), std::string(separator + 1, end)};
    if (credentials.access_key.empty() || credentials.secret_key.empty()) throw std::runtime_error("Invalid saved credentials");
    credentials.authorization();
    return credentials;
#elif defined(PS5_PAYLOAD)
    if (!std::filesystem::exists(path_)) return std::nullopt;
    std::ifstream input(path_, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read saved credentials");
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto separator = text.size() > 2200 || text.compare(0, plain_prefix.size(), plain_prefix) != 0
        ? std::string::npos : text.find('\n', plain_prefix.size());
    if (separator == std::string::npos) { wipe(text); throw std::runtime_error("Invalid saved credentials"); }
    S3Credentials credentials{text.substr(plain_prefix.size(), separator - plain_prefix.size()), text.substr(separator + 1)};
    wipe(text);
    if (credentials.access_key.empty() || credentials.secret_key.empty()) throw std::runtime_error("Invalid saved credentials");
    credentials.authorization();
    return credentials;
#else
    throw std::runtime_error("Saved credentials are not supported on this platform");
#endif
}

void CredentialStore::save(const S3Credentials& credentials) const {
#ifdef _WIN32
    if (credentials.access_key.empty() || credentials.secret_key.empty()) {
        throw std::runtime_error("Enter both IA-S3 keys before remembering them");
    }
    credentials.authorization();
    const std::string prefix = "ArchiveCatalog-S3-v1\n";
    Plaintext plaintext;
    plaintext.bytes.reserve(prefix.size() + credentials.access_key.size() + 1 + credentials.secret_key.size());
    plaintext.bytes.insert(plaintext.bytes.end(), prefix.begin(), prefix.end());
    plaintext.bytes.insert(plaintext.bytes.end(), credentials.access_key.begin(), credentials.access_key.end());
    plaintext.bytes.push_back('\n');
    plaintext.bytes.insert(plaintext.bytes.end(), credentials.secret_key.begin(), credentials.secret_key.end());
    DATA_BLOB source{static_cast<DWORD>(plaintext.bytes.size()), plaintext.bytes.data()};
    ProtectedData encrypted;
    if (!CryptProtectData(&source, L"ArchiveCatalog IA-S3", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &encrypted.blob)) {
        throw std::runtime_error("Windows could not protect the credentials");
    }
    if (!path_.parent_path().empty()) std::filesystem::create_directories(path_.parent_path());
    auto temporary = path_;
    temporary += L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create saved credentials");
    DWORD written = 0;
    bool saved = WriteFile(file, encrypted.blob.pbData, encrypted.blob.cbData, &written, nullptr) &&
        written == encrypted.blob.cbData && FlushFileBuffers(file);
    CloseHandle(file);
    if (!saved || !MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("Cannot save protected credentials");
    }
#elif defined(PS5_PAYLOAD)
    if (credentials.access_key.empty() || credentials.secret_key.empty()) {
        throw std::runtime_error("Enter both IA-S3 keys before remembering them");
    }
    credentials.authorization();
    if (!path_.parent_path().empty()) std::filesystem::create_directories(path_.parent_path());
    std::string text = plain_prefix + credentials.access_key + "\n" + credentials.secret_key;
    auto temporary = path_;
    temporary += ".tmp";
    ::unlink(temporary.c_str());
    // The console has no secret store, so the keys are plain text readable only by this file's owner.
    const int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    bool saved = file >= 0;
    for (std::size_t offset = 0; saved && offset < text.size();) {
        const auto written = ::write(file, text.data() + offset, text.size() - offset);
        if (written > 0) offset += static_cast<std::size_t>(written); else saved = false;
    }
    if (file >= 0) {
        saved = saved && ::fsync(file) == 0;
        ::close(file);
    }
    wipe(text);
    if (!saved || ::rename(temporary.c_str(), path_.c_str()) != 0) {
        ::unlink(temporary.c_str());
        throw std::runtime_error("Cannot save the keys on this console");
    }
#else
    (void)credentials;
    throw std::runtime_error("Saved credentials are not supported on this platform");
#endif
}

void CredentialStore::forget() const {
    std::error_code error;
    std::filesystem::remove(path_, error);
    if (error) throw std::runtime_error("Cannot remove saved credentials");
}

}