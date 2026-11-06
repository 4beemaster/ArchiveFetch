#pragma once

#include "transfer.hpp"

#include <optional>

namespace archive_catalog {

class CredentialStore {
public:
    explicit CredentialStore(std::filesystem::path path);
    static std::filesystem::path default_path();
    static bool supported();
    // False where the keys are saved as plain text because the platform has no secret store.
    static bool protected_storage();
    std::optional<S3Credentials> load() const;
    void save(const S3Credentials& credentials) const;
    void forget() const;

private:
    std::filesystem::path path_;
};

}