#pragma once

#include "catalog.hpp"

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>

namespace archive_catalog {

struct TransferHooks {
    std::function<bool()> cancelled;
    std::function<void(std::uint64_t, std::uint64_t)> progress;
    std::function<void(const std::string&)> status;
    // A step after the transfer (joining parts, verifying, extracting) and how far it has got.
    std::function<void(const std::string&, std::uint64_t, std::uint64_t)> stage;
};

struct TransferReport {
    std::string name;
    std::string host;
    std::string address;
    std::uint64_t bytes = 0;
    long long total_us = 0;
    long long connect_us = 0;
    long long tls_us = 0;
    long http_version = 0;
    int receive_buffer_default = -1;
    int receive_buffer = -1;
    int connections = 1;
};

// Called after each completed file transfer; set once before downloads start.
void set_transfer_observer(std::function<void(const TransferReport&)> observer);

struct S3Credentials {
    std::string access_key;
    std::string secret_key;
    std::string authorization() const;
};

bool s3_destination_allowed(const std::string& url);
std::string request_error_message(const std::string& detail, long status, bool credentials_provided);

class OperationCancelled : public std::runtime_error {
public:
    OperationCancelled() : std::runtime_error("Operation cancelled") {}
};

void verify_file(const File& file, const std::filesystem::path& path, const TransferHooks& hooks = {});
std::filesystem::path download_target(const std::filesystem::path& directory, const File& file);

// A split download keeps parts 1..n-1 beside the .part file ("name.part.1", ...).
inline constexpr int max_partial_segments = 16;
// Archive.org's storage servers start refusing parallel requests from one client at around 10-12.
inline constexpr int default_download_connections = 4;
inline constexpr int max_download_connections = 8;
std::filesystem::path partial_segment(const std::filesystem::path& partial, int index);

class Http {
public:
    explicit Http(std::string ca_bundle = {}, TransferHooks hooks = {}, S3Credentials s3 = {}, int connections = default_download_connections);
    Http(const Http&) = delete;
    Http& operator=(const Http&) = delete;
    ~Http();
    std::string get(const std::string& url, std::size_t limit = 32 * 1024 * 1024) const;
    void download(const File& file, const std::filesystem::path& directory, bool overwrite = false) const;

private:
    std::string ca_bundle_;
    TransferHooks hooks_;
    S3Credentials s3_;
    int connections_;
};

}