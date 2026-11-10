#pragma once

#include "transfer.hpp"
#include "version.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <map>
#include <utility>

namespace archive_catalog {

class UpdateCheck {
public:
    using Json = nlohmann::json;
    using Fetch = std::function<std::string(const std::string&, std::size_t)>;
    Fetch fetch;

    static Json select(const Json& releases, const std::string& current, bool include_beta) {
        if (!canonical_version(current)) throw std::runtime_error("The installed ArchiveFetch version is invalid");
        if (!releases.is_array()) throw std::runtime_error("GitHub returned an invalid release list");
        const Json* latest = nullptr;
        std::string version;
        for (const auto& release : releases) {
            if (release.at("draft").get<bool>()) continue;
            const auto tag = release.at("tag_name").get<std::string>();
            if (tag.empty() || tag[0] != 'v' || !canonical_version(tag.substr(1))) continue;
            const auto candidate = tag.substr(1);
            // Published 0.x releases are marked prerelease on GitHub too; the tag distinguishes beta builds.
            if (!include_beta && candidate.find("-beta.") != std::string::npos) continue;
            if (!latest || compare_versions(candidate, version) > 0) {
                latest = &release;
                version = candidate;
            } else if (candidate == version) throw std::runtime_error("GitHub returned duplicate release versions");
        }
        if (!latest) throw std::runtime_error("No matching published ArchiveFetch release was found");
        const auto tag = latest->at("tag_name").get<std::string>();
        const auto url = std::string(repository) + "/releases/tag/" + tag;
        if (latest->at("html_url").get<std::string>() != url)
            throw std::runtime_error("The release page is not in the official ArchiveFetch repository");
        const auto notes = latest->at("body").is_null() ? std::string() : latest->at("body").get<std::string>();
        if (notes.size() > 128 * 1024) throw std::runtime_error("The release notes are too large");
        Json assets = Json::object();
        const std::map<std::string, std::string> names{
            {"ArchiveFetch-" + version + "-windows-x64-setup.exe", "windows"},
            {"ArchiveFetch-" + version + "-android-arm64.apk", "android"},
            {"ArchiveFetch-" + version + "-android-x86_64.apk", "android-x86_64"},
            {"ArchiveFetch-" + version + ".elf", "ps5"},
            {"SHA256SUMS-" + version + ".txt", "checksums"},
            {"SHA256SUMS.txt", "checksums"}
        };
        if (!latest->at("assets").is_array()) throw std::runtime_error("GitHub returned an invalid release asset list");
        for (const auto& asset : latest->at("assets")) {
            const auto name = asset.at("name").get<std::string>();
            const auto known = names.find(name);
            if (known == names.end()) continue;
            const auto download = std::string(repository) + "/releases/download/" + tag + "/" + name;
            if (asset.at("browser_download_url").get<std::string>() != download)
                throw std::runtime_error("A release download is not in the official ArchiveFetch repository");
            if (assets.contains(known->second)) throw std::runtime_error("The release has ambiguous download assets");
            assets[known->second] = {{"name", name}, {"url", download}};
            if (asset.contains("size")) {
                if (!asset.at("size").is_number_integer() || asset.at("size").get<std::int64_t>() <= 0 ||
                    asset.at("size").get<std::uint64_t>() > 256ULL * 1024 * 1024)
                    throw std::runtime_error("The release asset has an invalid size");
                assets[known->second]["size"] = asset.at("size");
            }
            if (asset.contains("digest") && !asset.at("digest").is_null()) {
                const auto digest = asset.at("digest").get<std::string>();
                if (digest.size() != 71 || digest.compare(0, 7, "sha256:") != 0 ||
                    digest.find_first_not_of("0123456789abcdef", 7) != std::string::npos)
                    throw std::runtime_error("The release asset has an invalid SHA256 digest");
                assets[known->second]["sha256"] = digest.substr(7);
            }
        }
        const int comparison = compare_versions(version, current);
        return {{"ok", true}, {"currentVersion", current}, {"latestVersion", version},
            {"updateAvailable", comparison > 0}, {"currentNewer", comparison < 0},
            {"release", {{"url", url}, {"notes", notes}, {"assets", assets}}}};
    }

    Json check(const std::string& current, const std::string& ca_bundle, bool include_beta) {
        checked_ = nullptr;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        TransferHooks hooks;
        hooks.cancelled = [deadline] { return std::chrono::steady_clock::now() >= deadline; };
        Http http(ca_bundle, hooks);
        Json releases = Json::array();
        try {
            for (int page = 1; page <= 5; ++page) {
                const auto url = release_page_url(page);
                const auto body = fetch ? fetch(url, response_limit) : http.get_release_page(page, response_limit);
                if (body.size() > response_limit) throw std::runtime_error("GitHub's release response is too large");
                const auto batch = Json::parse(body);
                if (!batch.is_array() || batch.size() > 20) throw std::runtime_error("GitHub returned an invalid release page");
                for (const auto& release : batch) releases.push_back(release);
                if (batch.size() < 20) {
                    checked_ = select(releases, current, include_beta);
                    return checked_;
                }
            }
            throw std::runtime_error("Too many releases to check reliably; open the ArchiveFetch releases page instead");
        } catch (const OperationCancelled&) {
            throw std::runtime_error("The GitHub update check timed out; try again later");
        }
    }

    std::string link(const std::string& kind, const std::string& version) const {
        if (checked_.is_null()) throw std::runtime_error("Check for updates before opening a release link");
        if (version != checked_.at("latestVersion").get<std::string>())
            throw std::runtime_error("The checked release changed; check for updates again before opening a link");
        if (kind == "release") return checked_.at("release").at("url").get<std::string>();
        const auto& assets = checked_.at("release").at("assets");
        if ((kind == "windows" || kind == "android" || kind == "android-x86_64" || kind == "ps5" || kind == "checksums") && assets.contains(kind))
            return assets.at(kind).at("url").get<std::string>();
        throw std::runtime_error("That download is not available in the checked release");
    }

    Json install_release(const std::string& platform, const std::string& version) const {
        link(platform, version);
        link("checksums", version);
        if (!checked_.at("updateAvailable").get<bool>())
            throw std::runtime_error("Only a newer checked release can be installed");
        return checked_;
    }

private:
    static constexpr const char* repository = "https://github.com/4beemaster/ArchiveFetch";
    static constexpr std::size_t response_limit = 2 * 1024 * 1024;
    Json checked_;
};

}
