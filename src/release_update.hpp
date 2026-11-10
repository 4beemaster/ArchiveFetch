#pragma once

#include "update_check.hpp"
#include <atomic>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#endif

namespace archive_catalog {

inline std::string release_checksum(const std::string& manifest, const std::string& filename) {
    if (manifest.size() > 64 * 1024) throw std::runtime_error("The release checksum manifest is too large");
    std::istringstream input(manifest.compare(0, 3, "\xef\xbb\xbf") == 0 ? manifest.substr(3) : manifest);
    std::string line, result;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.size() < 67 || line.size() > 512 || line[64] != ' ' || (line[65] != ' ' && line[65] != '*'))
            throw std::runtime_error("The release checksum manifest is malformed");
        auto digest = line.substr(0, 64);
        for (auto& ch : digest) {
            if (ch >= 'A' && ch <= 'F') ch = static_cast<char>(ch - 'A' + 'a');
        }
        if (digest.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("The release checksum manifest has an invalid SHA256");
        const auto name = line.substr(66);
        if (name.find_first_of("/\\") != std::string::npos || name == "." || name == "..")
            throw std::runtime_error("The release checksum manifest contains an unsafe filename");
        if (name != filename) continue;
        if (!result.empty()) throw std::runtime_error("The release checksum manifest has duplicate asset entries");
        result = digest;
    }
    if (result.empty()) throw std::runtime_error("The release checksum manifest does not contain this download");
    return result;
}

class ReleaseUpdate {
public:
    using Json = nlohmann::json;
    using Download = std::function<std::uint64_t(const std::string&, const std::filesystem::path&,
        std::uint64_t, const TransferHooks&)>;
    using Install = std::function<void(const std::filesystem::path&, const std::string&)>;
    Download download;
    Install installer;

    ~ReleaseUpdate() { cancel_ = true; if (worker_.joinable()) worker_.join(); }

    void configure(const std::filesystem::path& root, const std::string& current, const std::string& platform) {
        directory_ = root / "updates";
        current_ = current;
        platform_ = platform;
        std::filesystem::create_directories(directory_);
        const auto file = directory_ / "update-state.json";
        if (!std::filesystem::exists(file)) return;
        try {
            if (std::filesystem::is_symlink(file) || std::filesystem::file_size(file) > 64 * 1024)
                throw std::runtime_error("Saved update state is not a bounded regular file");
            {
                std::ifstream input(file);
                state_ = Json::parse(input);
            }
            const auto version = state_.at("version").get<std::string>();
            if (!canonical_version(version) || state_.at("name") != asset_name(version))
                throw std::runtime_error("Saved update state has an invalid release identity");
            const auto digest = state_.value("sha256", "");
            if (!digest.empty() && (digest.size() != 64 || digest.find_first_not_of("0123456789abcdef") != std::string::npos))
                throw std::runtime_error("Saved update state has an invalid SHA256");
            const auto phase = state_.at("phase").get<std::string>();
            if (phase == "ready" && compare_versions(version, current_) <= 0) {
                state_["phase"] = "obsolete";
                state_["error"] = "The staged update is not newer than this app. Check for updates again.";
                save_locked();
            } else if (phase == "downloading" || phase == "verifying") {
                state_["phase"] = "interrupted";
                state_["error"] = "The previous update download was interrupted; download it again.";
                save_locked();
            } else if (phase == "installing" && version != current_) {
                state_["phase"] = "ready";
                state_["error"] = "The installer has not updated this app. If it was cancelled, you can retry the verified update.";
                save_locked();
            } else if (phase != "ready" && phase != "installing" && phase != "installed" &&
                       phase != "failed" && phase != "cancelled" && phase != "interrupted" && phase != "obsolete")
                throw std::runtime_error("Saved update state has an invalid phase");
            if (platform_ != "ps5") complete_pending();
        } catch (const std::exception& failure) {
            state_ = {{"phase", "failed"}, {"error", std::string("Cannot restore update state: ") + failure.what()}};
            std::cerr << state_.at("error").get<std::string>() << '\n';
        }
    }

    Json status() const {
        std::lock_guard<std::mutex> guard(mutex_);
        auto result = state_;
        result["running"] = running_;
        return result;
    }

    void start(const Json& checked, const std::string& ca_bundle) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (running_ || state_.value("phase", "") == "installing") throw std::runtime_error("An app update is already running");
        }
        if (worker_.joinable()) worker_.join();
        const auto version = checked.at("latestVersion").get<std::string>();
        if (!canonical_version(version) || compare_versions(version, current_) <= 0)
            throw std::runtime_error("Only a newer checked release can be downloaded");
        const auto& assets = checked.at("release").at("assets");
        const auto asset = assets.at(platform_);
        const auto checksums = assets.at("checksums");
        if (asset.at("name") != asset_name(version) || !release_asset_url_allowed(asset.at("url")) ||
            !release_asset_url_allowed(checksums.at("url")))
            throw std::runtime_error("The checked update has an invalid asset identity");
        const auto size = asset.value("size", 256ULL * 1024 * 1024);
        if (!size || size > 256ULL * 1024 * 1024) throw std::runtime_error("Invalid release download size");
        if (std::filesystem::space(directory_).available < size + 1024 * 1024)
            throw std::runtime_error("Not enough free space to stage the app update");
        {
            std::lock_guard<std::mutex> guard(mutex_);
            state_ = {{"phase", "downloading"}, {"version", version}, {"name", asset_name(version)},
                {"received", 0}, {"total", size}, {"sha256", ""}, {"error", ""}, {"warning", ""}};
            save_locked();
            running_ = true;
            cancel_ = false;
        }
        try {
            worker_ = std::thread([this, asset, checksums, ca_bundle, size] {
                try {
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
                    TransferHooks hooks;
                    hooks.cancelled = [this, deadline] { return cancel_ || std::chrono::steady_clock::now() >= deadline; };
                    hooks.progress = [this](std::uint64_t received, std::uint64_t) {
                        std::lock_guard<std::mutex> guard(mutex_);
                        state_["received"] = received;
                    };
                    Http http(ca_bundle, hooks);
                    auto get = [&](const std::string& url, const std::filesystem::path& path, std::uint64_t limit) {
                        return download ? download(url, path, limit, hooks) : http.download_release_asset(url, path, limit);
                    };
                    const auto manifest_path = directory_ / "checksums.part";
                    get(checksums.at("url"), manifest_path, 64 * 1024);
                    std::ifstream manifest_file(manifest_path, std::ios::binary);
                    if (!manifest_file) throw std::runtime_error("Cannot read the downloaded checksum manifest");
                    const std::string manifest((std::istreambuf_iterator<char>(manifest_file)), {});
                    const auto digest = release_checksum(manifest, asset.at("name"));
                    if (asset.contains("sha256") && asset.at("sha256") != digest)
                        throw std::runtime_error("The checksum manifest disagrees with GitHub's asset SHA256");
                    const auto part = directory_ / (asset.at("name").get<std::string>() + ".part");
                    const auto received = get(asset.at("url"), part, size);
                    if (asset.contains("size") && received != size) throw std::runtime_error("Release download size mismatch");
                    {
                        std::lock_guard<std::mutex> guard(mutex_);
                        state_["phase"] = "verifying";
                        state_["received"] = received;
                        save_locked();
                    }
                    if (sha256_file(part, hooks) != digest) throw std::runtime_error("Release download SHA256 mismatch; nothing was installed");
                    replace_file(part, directory_ / asset.at("name").get<std::string>());
                    {
                        std::lock_guard<std::mutex> guard(mutex_);
                        state_["sha256"] = digest;
                        state_["phase"] = "ready";
                        state_["total"] = received;
                        save_locked();
                    }
                } catch (const std::exception& failure) {
                    std::lock_guard<std::mutex> guard(mutex_);
                    state_["phase"] = cancel_ ? "cancelled" : "failed";
                    state_["error"] = failure.what();
                    std::cerr << "App update failed: " << failure.what() << '\n';
                    try { save_locked(); }
                    catch (const std::exception& persistence) {
                        state_["error"] = state_.at("error").get<std::string>() + "; cannot save update status: " + persistence.what();
                        std::cerr << state_.at("error").get<std::string>() << '\n';
                    }
                }
                std::lock_guard<std::mutex> guard(mutex_);
                running_ = false;
            });
        } catch (const std::exception& failure) {
            std::lock_guard<std::mutex> guard(mutex_);
            running_ = false;
            state_["phase"] = "failed";
            state_["error"] = failure.what();
            save_locked();
            throw;
        }
    }

    void cancel() { cancel_ = true; }

    Json install(const std::string& version) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto path = verified_locked(version);
        if (!installer) {
            if (platform_ != "android" && platform_ != "android-x86_64")
                throw std::runtime_error("The native app installer is not available on this host");
            return {{"ok", true}, {"installUpdate", {
                {"path", path.u8string()}, {"version", version}, {"sha256", state_.at("sha256")}}}};
        }
        state_["phase"] = "installing";
        state_["error"] = "";
        save_locked();
        try { installer(path, version); }
        catch (const std::exception& failure) {
            state_["phase"] = "ready";
            state_["error"] = failure.what();
            save_locked();
            throw;
        }
        return {{"ok", true}};
    }

    void install_started(const std::string& version) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (platform_ != "android" && platform_ != "android-x86_64")
            throw std::runtime_error("Only the Android host can acknowledge a package-installer handoff");
        verified_locked(version);
        state_["phase"] = "installing";
        save_locked();
    }

    void complete_pending(const std::function<void(const std::filesystem::path&)>& finish = {}) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (state_.value("phase", "") != "installing" || state_.value("version", "") != current_) return;
        try {
            const auto path = verified_locked(current_, "installing");
            if (finish) finish(path);
            state_["warning"] = "";
        } catch (const std::exception& failure) {
            state_["warning"] = std::string("The new version is running, but update completion needs attention: ") + failure.what();
            std::cerr << state_.at("warning").get<std::string>() << '\n';
        }
        state_["phase"] = "installed";
        save_locked();
    }

    void retry_completion(const std::function<void(const std::filesystem::path&)>& finish) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (state_.value("phase", "") != "installed" || state_.value("version", "") != current_ ||
                state_.value("warning", "").empty())
                throw std::runtime_error("There is no unfinished update completion to retry");
            state_["phase"] = "installing";
            save_locked();
        }
        complete_pending(finish);
    }

private:
    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    bool running_ = false;
    std::filesystem::path directory_;
    std::string current_, platform_;
    Json state_ = {{"phase", "idle"}, {"error", ""}, {"warning", ""}};

    std::string asset_name(const std::string& version) const {
        if (platform_ == "windows") return "ArchiveFetch-" + version + "-windows-x64-setup.exe";
        if (platform_ == "android" || platform_ == "android-x86_64")
            return "ArchiveFetch-" + version + (platform_ == "android" ? "-android-arm64.apk" : "-android-x86_64.apk");
        if (platform_ == "ps5") return "ArchiveFetch-" + version + ".elf";
        throw std::runtime_error("App installation is not supported on this platform");
    }

    static void replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
        if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot save the verified app update");
#else
        std::filesystem::rename(from, to);
#endif
    }

    void save_locked() const {
        const auto temporary = directory_ / "update-state.json.tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << state_.dump(2);
        output.close();
        if (!output) throw std::runtime_error("Cannot write update status");
        replace_file(temporary, directory_ / "update-state.json");
    }

    std::filesystem::path verified_locked(const std::string& version, const std::string& phase = "ready") const {
        if (running_ || state_.value("phase", "") != phase || state_.value("version", "") != version ||
            !canonical_version(version) || state_.value("name", "") != asset_name(version) ||
            (phase == "ready" && compare_versions(version, current_) <= 0))
            throw std::runtime_error("Download and verify this release before installing it");
        const auto path = directory_ / asset_name(version);
        if (sha256_file(path) != state_.at("sha256").get<std::string>())
            throw std::runtime_error("The staged update changed; download it again before installing");
        return path;
    }
};

}
