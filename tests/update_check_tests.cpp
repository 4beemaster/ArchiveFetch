#include "release_update.hpp"
#include <iostream>

using archive_catalog::UpdateCheck;
using Json = UpdateCheck::Json;

namespace {
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class Action>
void rejects(Action action, const std::string& detail) {
    bool rejected = false;
    try { action(); }
    catch (const std::exception& failure) { rejected = std::string(failure.what()).find(detail) != std::string::npos; }
    require(rejected, ("Expected explicit failure: " + detail).c_str());
}
Json release(const std::string& version) {
    const auto base = std::string("https://github.com/4beemaster/ArchiveFetch/releases/");
    Json assets = Json::array();
    for (const auto& name : {"ArchiveFetch-" + version + "-windows-x64-setup.exe",
            "ArchiveFetch-" + version + "-android-arm64.apk", "ArchiveFetch-" + version + ".elf",
            "SHA256SUMS-" + version + ".txt"}) {
        assets.push_back({{"name", name}, {"browser_download_url", base + "download/v" + version + "/" + name}});
    }
    return {{"draft", false}, {"prerelease", true}, {"tag_name", "v" + version},
        {"html_url", base + "tag/v" + version}, {"body", "# Release\n<script>not HTML</script>"}, {"assets", assets}};
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--live-download") {
            const auto directory = std::filesystem::temp_directory_path() /
                ("archivefetch-release-live-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(directory);
            {
                UpdateCheck checker;
                const auto checked = checker.check("0.7.2", "", false);
                archive_catalog::ReleaseUpdate update;
                update.configure(directory, "0.7.2", "ps5");
                update.start(checked, "");
                while (update.status().at("running").get<bool>()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                const auto status = update.status();
                require(status.at("phase") == "ready", ("Real verified download failed: " + status.dump()).c_str());
                std::cout << status.dump() << '\n';
            }
            std::filesystem::remove_all(directory);
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--live") {
            UpdateCheck checker;
            const auto result = checker.check("0.7.2", "", false);
            require(result.at("updateAvailable") == true, "The real published release must update the 0.7.2 test baseline");
            std::cout << Json({{"baseline", "0.7.2"}, {"latestVersion", result.at("latestVersion")},
                {"installer", checker.link("windows", result.at("latestVersion").get<std::string>())},
                {"checksums", checker.link("checksums", result.at("latestVersion").get<std::string>())}}).dump() << '\n';
            return 0;
        }
        if (argc != 1) throw std::runtime_error("Usage: update_check_tests [--live|--live-download]");
        using archive_catalog::canonical_version;
        using archive_catalog::compare_versions;
        const std::string abc_digest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        const std::string asset_name = "ArchiveFetch-0.8.0-windows-x64-setup.exe";
        require(archive_catalog::release_checksum(abc_digest + "  " + asset_name + "\r\n", asset_name) == abc_digest,
            "Exact SHA256 manifest entry parsed");
        rejects([&] { archive_catalog::release_checksum(abc_digest + "  ../" + asset_name, asset_name); }, "unsafe");
        rejects([&] { archive_catalog::release_checksum(abc_digest + "  " + asset_name + "\n" +
            abc_digest + "  " + asset_name, asset_name); }, "duplicate");
        rejects([&] { archive_catalog::release_checksum("not a manifest", asset_name); }, "malformed");
        rejects([&] { archive_catalog::release_checksum(abc_digest + "  other.exe", asset_name); }, "does not contain");
        require(!archive_catalog::release_asset_url_allowed("https://example.invalid/setup.exe"), "Foreign release origin refused");
        require(!archive_catalog::release_asset_url_allowed("https://release-assets.githubusercontent.com/file"),
            "Asset CDN is not a permitted initial URL");
        require(archive_catalog::release_asset_url_allowed("https://release-assets.githubusercontent.com/file?signature=fixture", true),
            "Exact HTTPS asset CDN redirect accepted");
        for (const auto* url : {"http://release-assets.githubusercontent.com/file",
                "https://release-assets.githubusercontent.com.evil.invalid/file",
                "https://user@release-assets.githubusercontent.com/file", "https://release-assets.githubusercontent.com:443/file",
                "https://release-assets.githubusercontent.com/file#fragment", "https://127.0.0.1/file"})
            require(!archive_catalog::release_asset_url_allowed(url, true), "Unsafe asset redirect refused");
        const auto update_root = std::filesystem::temp_directory_path() /
            ("archivefetch-release-unit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(update_root);
        {
            archive_catalog::ReleaseUpdate update;
            update.configure(update_root, "0.7.3", "windows");
            bool corrupt = false;
            update.download = [&](const std::string& url, const std::filesystem::path& path, std::uint64_t limit,
                    const archive_catalog::TransferHooks&) {
                const std::string text = url.find("SHA256SUMS") != std::string::npos
                    ? abc_digest + "  " + asset_name + "\n" : corrupt ? "bad" : "abc";
                require(text.size() <= limit, "Fixture respects size cap");
                std::ofstream output(path, std::ios::binary);
                output << text;
                return static_cast<std::uint64_t>(text.size());
            };
            const auto checked = UpdateCheck::select(Json::array({release("0.8.0")}), "0.7.3", false);
            auto await = [&] {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (update.status().at("running").get<bool>() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                require(!update.status().at("running").get<bool>(), "Bounded update worker completion");
            };
            corrupt = true;
            update.start(checked, "");
            await();
            require(update.status().at("phase") == "failed", "Corrupt release fails verification");
            rejects([&] { update.install("0.8.0"); }, "Download and verify");
            corrupt = false;
            update.start(checked, "");
            await();
            require(update.status().at("phase") == "ready" && update.status().at("sha256") == abc_digest,
                "Verified release becomes ready");
            rejects([&] { update.install("0.8.1"); }, "Download and verify");
            const auto staged = update_root / "updates" / asset_name;
            { std::ofstream changed(staged, std::ios::binary); changed << "tampered"; }
            rejects([&] { update.install("0.8.0"); }, "staged update changed");
            update.start(checked, "");
            await();
            update.installer = [](const std::filesystem::path&, const std::string&) { throw std::runtime_error("fixture launch failure"); };
            rejects([&] { update.install("0.8.0"); }, "fixture launch failure");
            require(update.status().at("phase") == "ready", "Failed installer launch remains retryable");
            int installs = 0;
            update.installer = [&](const std::filesystem::path& path, const std::string& version) {
                require(path == staged && version == "0.8.0", "Only checked verified file handed to installer");
                ++installs;
            };
            update.install("0.8.0");
            require(installs == 1 && update.status().at("phase") == "installing", "Successful installer handoff persisted");
        }
        {
            archive_catalog::ReleaseUpdate restored;
            restored.configure(update_root, "0.8.0", "windows");
            require(restored.status().at("phase") == "installed", "New matching runtime completes persisted update");
        }
        {
            Json saved;
            {
                std::ifstream input(update_root / "updates" / "update-state.json");
                saved = Json::parse(input);
            }
            saved["phase"] = "ready";
            {
                std::ofstream output(update_root / "updates" / "update-state.json");
                output << saved.dump();
            }
            archive_catalog::ReleaseUpdate newer;
            newer.configure(update_root, "0.9.0", "windows");
            require(newer.status().at("phase") == "obsolete", "A manual app upgrade invalidates older staged installers");
            rejects([&] { newer.install("0.8.0"); }, "Download and verify");
        }
        {
            archive_catalog::ReleaseUpdate cancelled;
            cancelled.configure(update_root / "cancelled", "0.7.3", "windows");
            cancelled.download = [](const std::string&, const std::filesystem::path&, std::uint64_t,
                    const archive_catalog::TransferHooks& hooks) -> std::uint64_t {
                while (!hooks.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                throw archive_catalog::OperationCancelled();
            };
            cancelled.start(UpdateCheck::select(Json::array({release("0.8.0")}), "0.7.3", false), "");
            cancelled.cancel();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (cancelled.status().at("running").get<bool>() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            require(cancelled.status().at("phase") == "cancelled" && !cancelled.status().at("running").get<bool>(),
                "Cancellation terminates staging without enabling installation");
            rejects([&] { cancelled.install("0.8.0"); }, "Download and verify");
        }
        const auto ps5_root = update_root / "ps5";
        {
            archive_catalog::ReleaseUpdate ps5;
            ps5.configure(ps5_root, "0.7.3", "ps5");
            ps5.download = [&](const std::string& url, const std::filesystem::path& path, std::uint64_t,
                    const archive_catalog::TransferHooks&) {
                const std::string text = url.find("SHA256SUMS") != std::string::npos
                    ? abc_digest + "  ArchiveFetch-0.8.0.elf\n" : "abc";
                std::ofstream output(path, std::ios::binary);
                output << text;
                return static_cast<std::uint64_t>(text.size());
            };
            ps5.start(UpdateCheck::select(Json::array({release("0.8.0")}), "0.7.3", false), "");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (ps5.status().at("running").get<bool>() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            require(ps5.status().at("phase") == "ready", "PS5 payload verified before handoff");
            ps5.installer = [](const std::filesystem::path&, const std::string&) {};
            ps5.install("0.8.0");
        }
        {
            archive_catalog::ReleaseUpdate successor;
            successor.configure(ps5_root, "0.8.0", "ps5");
            require(successor.status().at("phase") == "installing", "PS5 completion waits for owner API synchronization");
            successor.complete_pending([](const std::filesystem::path&) { throw std::runtime_error("fixture PM unavailable"); });
            require(successor.status().at("phase") == "installed" &&
                successor.status().at("warning").get<std::string>().find("fixture PM unavailable") != std::string::npos,
                "PM failure preserves running update and explicit warning");
            int retries = 0;
            successor.retry_completion([&](const std::filesystem::path&) { ++retries; });
            require(retries == 1 && successor.status().at("warning") == "", "Explicit PM retry clears warning after success");
        }
        std::filesystem::remove_all(update_root);
        for (const auto* valid : {"0.7.3", "0.8.0-beta.1", "12.34.56-beta.12"})
            require(canonical_version(valid), "Canonical versions accepted");
        for (const auto* invalid : {"", "v0.8.0", "0.8", "00.8.0", "-1.0.0", "+1.0.0", " 1.0.0",
                "0.8.0-beta.0", "0.8.0-beta.01", "0.8.0-beta.1x", "0.8.0-beta.1.2",
                "0.8.0-beta.1099511627776", "0.8.0-beta.1099511627777", "0.8.0-rc.1",
                "0.8.0\n", "999999999999999999999999.0.0"})
            require(!canonical_version(invalid), "Noncanonical or overflowing versions rejected");
        require(compare_versions("0.8.0-beta.2", "0.8.0-beta.10") < 0, "Beta numbers compare numerically");
        require(compare_versions("0.8.0-beta.10", "0.8.0") < 0, "Final follows its beta");
        require(compare_versions("0.8.0-beta.1", "0.7.3") > 0, "Newer development version follows older final");
        require(compare_versions("0.10.0", "0.9.9") > 0, "Minor versions compare numerically");
        require(compare_versions("unknown", "0.7.3") < 0, "Console unknown-version behavior preserved");
        {
            archive_catalog::Http http;
            rejects([&] { http.get_release_page(0); }, "Invalid GitHub release page");
            rejects([&] { http.get_release_page(6); }, "Invalid GitHub release page");
            rejects([&] { http.get(archive_catalog::release_page_url(1)); }, "URL must belong to archive.org");
            archive_catalog::Http authenticated("", {}, {"fixture-access", "fixture-secret"});
            rejects([&] { authenticated.get_release_page(1); }, "credentials cannot be used");
        }
        const auto final = release("0.8.0");
        auto draft = release("9.0.0"); draft["draft"] = true;
        auto unknown = release("8.0.0"); unknown["tag_name"] = "v8.0.0-beta.1extra";
        const Json feed = Json::array({release("0.8.1-beta.2"), release("0.7.3"), draft, final, unknown});
        auto result = UpdateCheck::select(feed, "0.8.0-beta.1", false);
        require(result.at("latestVersion") == "0.8.0" && result.at("updateAvailable") == true,
            "Final 0.x releases are included despite GitHub prerelease flag, drafts/betas excluded");
        require(result.at("release").at("notes") == final.at("body"), "Notes remain plain text");
        require(result.at("release").at("assets").size() == 4, "All platform assets and checksums selected");
        result = UpdateCheck::select(feed, "0.8.0-beta.1", true);
        require(result.at("latestVersion") == "0.8.1-beta.2", "Explicit beta selection includes newer beta");
        result = UpdateCheck::select(Json::array({final, release("0.8.0-beta.10")}), "0.8.0", true);
        require(result.at("latestVersion") == "0.8.0" && result.at("updateAvailable") == false &&
            result.at("currentNewer") == false, "Equal versions are current, final outranks beta");
        result = UpdateCheck::select(Json::array({release("0.7.3")}), "0.8.0-beta.1", false);
        require(result.at("currentNewer") == true && result.at("updateAvailable") == false, "Development-ahead state is distinct");
        rejects([&] { UpdateCheck::select(Json::object(), "0.8.0", false); }, "invalid release list");
        rejects([&] { UpdateCheck::select(feed, "broken", false); }, "installed ArchiveFetch version");
        rejects([&] { UpdateCheck::select(Json::array(), "0.8.0", false); }, "No matching");
        rejects([&] { UpdateCheck::select(Json::array({draft, unknown}), "0.8.0", true); }, "No matching");
        rejects([&] { UpdateCheck::select(Json::array({final, final}), "0.8.0", false); }, "duplicate");
        for (const auto* url : {"http://github.com/4beemaster/ArchiveFetch/releases/tag/v0.8.0",
                "https://github.com.evil.invalid/4beemaster/ArchiveFetch/releases/tag/v0.8.0",
                "https://github.com/another/repo/releases/tag/v0.8.0",
                "https://github.com/4beemaster/ArchiveFetch/releases/tag/v0.8.0?redirect=1"}) {
            auto bad = final; bad["html_url"] = url;
            rejects([&] { UpdateCheck::select(Json::array({bad}), "0.7.3", false); }, "official ArchiveFetch repository");
        }
        auto bad = final;
        bad["assets"][0]["browser_download_url"] = "https://example.invalid/setup.exe";
        rejects([&] { UpdateCheck::select(Json::array({bad}), "0.7.3", false); }, "official ArchiveFetch repository");
        bad = final; bad["assets"].push_back(bad["assets"][0]);
        rejects([&] { UpdateCheck::select(Json::array({bad}), "0.7.3", false); }, "ambiguous");
        bad = final; bad["assets"] = Json::object();
        rejects([&] { UpdateCheck::select(Json::array({bad}), "0.7.3", false); }, "invalid release asset list");
        bad = final; bad["body"] = std::string(128 * 1024 + 1, 'x');
        rejects([&] { UpdateCheck::select(Json::array({bad}), "0.7.3", false); }, "notes are too large");
        auto missing = final; missing["assets"] = Json::array(); missing["body"] = nullptr;
        result = UpdateCheck::select(Json::array({missing}), "0.7.3", false);
        require(result.at("updateAvailable") == true && result.at("release").at("assets").empty() &&
            result.at("release").at("notes") == "", "Missing downloads do not hide an available release");
        auto legacy = release("0.7.2"); legacy["assets"].back() = {
            {"name", "SHA256SUMS.txt"}, {"browser_download_url",
            "https://github.com/4beemaster/ArchiveFetch/releases/download/v0.7.2/SHA256SUMS.txt"}};
        require(UpdateCheck::select(Json::array({legacy}), "0.7.1", false).at("release").at("assets").contains("checksums"),
            "Legacy checksum filename supported");
        auto unrelated = final; unrelated["assets"].push_back({{"name", "other.exe"}, {"browser_download_url", "https://example.invalid"}});
        require(UpdateCheck::select(Json::array({unrelated}), "0.7.3", false).at("release").at("assets").size() == 4,
            "Unrecognized assets are not offered");
        UpdateCheck checker;
        rejects([&] { checker.link("release", "0.8.0"); }, "Check for updates");
        int requests = 0;
        checker.fetch = [&](const std::string& url, std::size_t limit) {
            ++requests;
            require(url == "https://api.github.com/repos/4beemaster/ArchiveFetch/releases?per_page=20&page=1",
                "Only the fixed public GitHub endpoint is fetched");
            require(limit == 2 * 1024 * 1024, "Response size is bounded");
            return Json::array({final}).dump();
        };
        require(requests == 0, "No automatic fetch at construction");
        result = checker.check("0.7.3", "", false);
        require(requests == 1 && result.at("latestVersion") == "0.8.0", "Manual check fetches the release list");
        require(checker.link("windows", "0.8.0").find("windows-x64-setup.exe") != std::string::npos, "Matching Windows installer link");
        require(checker.link("release", "0.8.0") == final.at("html_url"), "Release link comes from checked data");
        rejects([&] { checker.link("https://example.invalid", "0.8.0"); }, "not available");
        rejects([&] { checker.link("release", "0.7.3"); }, "checked release changed");
        checker.fetch = [](const std::string&, std::size_t) -> std::string { throw std::runtime_error("HTTP 403: rate limit"); };
        rejects([&] { checker.check("0.7.3", "", false); }, "HTTP 403: rate limit");
        rejects([&] { checker.link("release", "0.8.0"); }, "Check for updates");
        checker.fetch = [](const std::string&, std::size_t) -> std::string { throw archive_catalog::OperationCancelled(); };
        rejects([&] { checker.check("0.7.3", "", false); }, "timed out");
        checker.fetch = [](const std::string&, std::size_t) { return std::string("{"); };
        rejects([&] { checker.check("0.7.3", "", false); }, "parse_error");
        checker.fetch = [](const std::string&, std::size_t) { return std::string("{\"message\":\"rate limited\"}"); };
        rejects([&] { checker.check("0.7.3", "", false); }, "invalid release page");
        checker.fetch = [](const std::string&, std::size_t limit) { return std::string(limit + 1, 'x'); };
        rejects([&] { checker.check("0.7.3", "", false); }, "response is too large");
        Json page = Json::array();
        for (int index = 0; index < 20; ++index) page.push_back(release("0.7." + std::to_string(index)));
        requests = 0;
        checker.fetch = [&](const std::string& url, std::size_t) {
            ++requests;
            require(url.find("page=" + std::to_string(requests)) != std::string::npos, "Release pages fetched in order");
            return (requests == 1 ? page : Json::array({final})).dump();
        };
        require(checker.check("0.7.3", "", false).at("latestVersion") == "0.8.0" && requests == 2,
            "Selection spans pages rather than trusting GitHub's release order");
        requests = 0;
        checker.fetch = [&](const std::string&, std::size_t) { ++requests; return page.dump(); };
        rejects([&] { checker.check("0.7.3", "", false); }, "Too many releases");
        require(requests == 5, "Pagination is bounded instead of reporting a truncated list as current");
        std::cout << "Update checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Update check failure: " << failure.what() << '\n';
        return 1;
    }
}
