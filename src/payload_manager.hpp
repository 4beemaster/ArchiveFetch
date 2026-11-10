#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace archive_catalog {

struct PayloadManagerSync {
    bool replaced = false;
    std::string filename;
};

namespace payload_manager_detail {

inline std::string reply(const httplib::Result& result, const char* operation) {
    if (!result) throw std::runtime_error(std::string(operation) + ": " + httplib::to_string(result.error()));
    if (result->status != 200)
        throw std::runtime_error(std::string(operation) + ": HTTP " + std::to_string(result->status));
    return result->body;
}

struct Stored {
    std::string path, filename, slot;
};

inline std::vector<Stored> list(httplib::Client& client) {
    const auto value = nlohmann::json::parse(reply(client.Get("/list_payloads"), "Read Payload Manager's list"));
    std::vector<Stored> result;
    const auto add = [&](const std::string& path, const std::string& slot) {
        const auto slash = path.rfind('/');
        if (slash == std::string::npos) throw std::runtime_error("Payload Manager returned an invalid payload path");
        result.push_back({path, path.substr(slash + 1), slot});
    };
    if (value.contains("items")) {
        for (const auto& item : value.at("items").get<nlohmann::json::array_t>())
            add(item.at("path").get<std::string>(), item.value("slot", std::string()));
    } else {
        for (const auto& path : value.at("payloads").get<std::vector<std::string>>()) add(path, {});
    }
    return result;
}

inline bool archivefetch_name(std::string filename) {
    std::transform(filename.begin(), filename.end(), filename.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (filename == "archivefetch.elf") return true;
    const std::string prefix = "archivefetch-";
    if (filename.compare(0, prefix.size(), prefix) != 0 || filename.size() <= prefix.size() + 4 ||
        !std::isdigit(static_cast<unsigned char>(filename[prefix.size()])) || filename.substr(filename.size() - 4) != ".elf") return false;
    return std::all_of(filename.begin(), filename.end(), [](unsigned char ch) { return std::isalnum(ch) || ch == '-' || ch == '.'; });
}

inline std::string query(const std::string& text) {
    const char* digits = "0123456789ABCDEF";
    std::string result;
    for (unsigned char ch : text) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') result += static_cast<char>(ch);
        else { result += '%'; result += digits[ch >> 4]; result += digits[ch & 15]; }
    }
    return result;
}

}

// Only replace an existing, unambiguous local copy. Payload Manager owns the write/rename;
// writing directly into its storage bypasses slot metadata and can cross PS5 filesystems.
inline PayloadManagerSync sync_payload_manager(httplib::Client& client, const std::filesystem::path& elf) {
    using namespace payload_manager_detail;
    client.set_connection_timeout(3);
    client.set_read_timeout(30);
    client.set_write_timeout(30);
    reply(client.Get("/version"), "Contact Payload Manager");
    const auto stored = list(client);
    std::vector<Stored> matches;
    for (const auto& entry : stored)
        if (entry.path.rfind("/data/pldmgr/payloads/", 0) == 0 && archivefetch_name(entry.filename)) matches.push_back(entry);
    if (matches.empty()) return {};
    if (matches.size() != 1) throw std::runtime_error("Payload Manager has multiple ArchiveFetch copies; choose one there before updating");
    const auto& target = matches.front();
    if (target.path.find("/repo/") != std::string::npos || (!target.slot.empty() && target.slot.rfind("local/", 0) != 0))
        throw std::runtime_error("ArchiveFetch is repository-managed in Payload Manager; update that copy there");
    const auto parent = target.path.substr(0, target.path.rfind('/'));
    const auto folder = parent.substr(parent.rfind('/') + 1);
    if (!archivefetch_name(folder + ".elf"))
        throw std::runtime_error("ArchiveFetch is not in its own Payload Manager folder; not replacing it");
    if (target.path.find("/../") != std::string::npos || target.path.find("/./") != std::string::npos ||
        (parent != "/data/pldmgr/payloads/" + folder && parent != "/data/pldmgr/payloads/local/" + folder))
        throw std::runtime_error("ArchiveFetch is not in a recognized Payload Manager storage folder");
    for (const auto& entry : stored)
        if (entry.path != target.path && entry.path.rfind(parent + "/", 0) == 0)
            throw std::runtime_error("ArchiveFetch shares a Payload Manager folder with another payload; not replacing it");
    const auto encoded = query(target.filename);
    const auto check = nlohmann::json::parse(reply(client.Get("/manage:check?filename=" + encoded), "Check Payload Manager's destination"));
    if (check.at("folder_name").get<std::string>() != folder || !check.at("file_exists").get<bool>())
        throw std::runtime_error("Payload Manager's upload destination does not match the existing ArchiveFetch copy");
    std::ifstream input(elf, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read the uploaded ArchiveFetch payload");
    const auto size = std::filesystem::file_size(elf);
    std::vector<char> buffer(65536);
    const httplib::Headers headers{{"X-PLDMGR-Request", "1"}};
    const auto result = client.Post("/manage:upload?filename=" + encoded + "&mode=replace", headers, static_cast<std::size_t>(size),
        [&](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
            input.clear();
            input.seekg(static_cast<std::streamoff>(offset));
            const auto count = std::min(length, buffer.size());
            input.read(buffer.data(), static_cast<std::streamsize>(count));
            return input.gcount() == static_cast<std::streamsize>(count) && sink.write(buffer.data(), count);
        }, "application/octet-stream");
    const auto body = reply(result, "Replace ArchiveFetch in Payload Manager");
    const auto uploaded = nlohmann::json::parse(body, nullptr, false);
    if (uploaded.is_object() && (uploaded.value("success", true) == false || uploaded.value("ok", true) == false ||
        (uploaded.contains("error") && !uploaded.at("error").is_null() && uploaded.at("error") != "")))
        throw std::runtime_error("Payload Manager rejected the replacement: " + body.substr(0, 300));
    const auto after = list(client);
    if (std::none_of(after.begin(), after.end(), [&](const Stored& entry) { return entry.path == target.path && entry.slot == target.slot; }))
        throw std::runtime_error("Payload Manager did not keep ArchiveFetch under its original name and slot");
    return {true, target.filename};
}

}
