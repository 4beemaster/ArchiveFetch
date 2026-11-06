#include "gui_model.hpp"
#include "transfer.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace archive_catalog;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Action>
void rejects(Action action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "Expected rejection");
}

int main() {
    auto directory = std::filesystem::temp_directory_path() /
        ("archive-gui-partial-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        GuiModel model;
        Config config;
        config.sources = {"https://archive.org/details/example"};
        config.follow_parts = false;
        model.start_scan(config, [](const std::string&) {
            return "<files><file name='Alpha.chd'><size>3</size><sha1>a9993e364706816aba3e25717850c26c9cd0d89d</sha1></file>"
                   "<file name='Beta.chd' private='true'><size>3</size><md5>900150983cd24fb0d6963f7d28e17f72</md5></file></files>";
        });
        model.wait();
        auto state = model.snapshot();
        require(state.loaded && !state.busy && state.catalog.files.size() == 2, "Background scan completion");
        require(state.catalog_revision == 1, "Completed scan marks the library for automatic saving");
        {
            auto first = directory / "status-first";
            auto second = directory / "status-second";
            require(library_download_status(state.catalog.files, first) == std::vector<bool>({false, false}),
                "Missing download directory leaves the entire library not downloaded");
            require(!std::filesystem::exists(first), "Checking status does not create download directories");
            std::filesystem::create_directories(first);
            std::filesystem::create_directories(second);
            { std::ofstream output(first / "Alpha.chd", std::ios::binary); output << "abc"; }
            { std::ofstream output(first / "Beta.chd.part", std::ios::binary); output << "abc"; }
            require(library_download_status(state.catalog.files, first) == std::vector<bool>({true, false}),
                "Check every library file and exclude partial downloads");
            require(library_download_status(state.catalog.files, second) == std::vector<bool>({false, false}),
                "Changing destination recalculates the entire library");
            { std::ofstream output(first / "Beta.chd", std::ios::binary); output << "ab"; }
            require(library_download_status(state.catalog.files, first) == std::vector<bool>({true, false}),
                "Wrong-sized final files are not downloaded");
            { std::ofstream output(first / "Beta.chd", std::ios::binary); output << "abc"; }
            require(library_download_status(state.catalog.files, first) == std::vector<bool>({true, true}),
                "Completing a download updates its status");
            std::filesystem::remove(first / "Alpha.chd");
            require(library_download_status(state.catalog.files, first) == std::vector<bool>({false, true}),
                "A full refresh detects externally removed files");
            auto nested = state.catalog.files.front();
            nested.name = "remote/Beta.chd";
            require(library_download_status({nested}, first) == std::vector<bool>({true}),
                "Status uses the same flat filename layout as downloads");
        }
        {
            GuiModel routed;
            const auto catalog_path = directory / "routed.xml";
            save_catalog(state.catalog, catalog_path);
            routed.import_catalog(catalog_path); routed.wait();
            const auto first = std::filesystem::weakly_canonical(directory / "location-first");
            const auto second = std::filesystem::weakly_canonical(directory / "location-second");
            routed.enqueue({0}, false, first); routed.enqueue({1}, false, second);
            Config routing_config; routing_config.download_dir = directory / "changed-default";
            std::vector<std::filesystem::path> targets;
            routed.start_downloads(routing_config, [&](const File& file, const std::filesystem::path& target, bool, const TransferHooks&) {
                targets.push_back(target);
                if (file.name == "Beta.chd") throw std::runtime_error("Retry destination fixture");
            });
            routed.wait();
            require(targets == std::vector<std::filesystem::path>({first, second}), "Each queued file retains its selected location");
            const auto failed = routed.snapshot().queue.front();
            require(failed.destination == second && failed.partial_path.parent_path() == second, "Failed entries retain their destination and partial path");
            routing_config.download_dir = directory / "another-default";
            routed.start_downloads(routing_config, [&](const File&, const std::filesystem::path& target, bool, const TransferHooks&) {
                require(target == second, "Retries use the original destination after changing the default");
            }, failed.id);
            routed.wait(); require(routed.snapshot().queue.empty(), "Location-bound retry completed");
        }
        model.enqueue({0, 0, 1});
        model.enqueue({1});
        require(model.snapshot().queue.size() == 2, "Deduplicate queued files");
        rejects([&] { model.enqueue({100}); });
        config.s3.access_key = "test-access";
        rejects([&] { model.start_downloads(config); });
        config.s3.access_key.clear();
        auto anonymous_config = config;
        anonymous_config.download_dir = directory / "anonymous";
        bool restricted_attempted = false;
        model.start_downloads(anonymous_config, [&](const File& file, const std::filesystem::path&, bool, const TransferHooks&) {
            if (file.restricted) restricted_attempted = true;
            throw std::runtime_error("HTTP 403");
        });
        model.wait();
        state = model.snapshot();
        require(restricted_attempted && state.queue.size() == 2 &&
            state.queue.back().status == QueueStatus::Failed && state.queue.back().detail == "HTTP 403",
            "Restricted downloads without S3 keys reach the transfer and retain server permission failures");
        model.remove(1);
        require(model.snapshot().queue.size() == 1, "Remove queue entry");
        model.start_scan(config, [](const std::string&) -> std::string { throw OperationCancelled(); });
        model.wait();
        state = model.snapshot();
        require(state.loaded && !state.busy && state.catalog.files.size() == 2, "Cancelled scan retains existing catalog");
        model.start_scan(config, [](const std::string&) -> std::string { throw std::runtime_error("HTTP 403"); });
        model.wait();
        state = model.snapshot();
        require(!state.catalog.errors.empty() && state.queue.empty(), "Incomplete scan clears stale queue");
        model.enqueue({});
        require(model.snapshot().queue.empty(), "Empty partial catalog has no files to queue");
        config.sources.push_back("https://archive.org/details/missing");
        model.start_scan(config, [](const std::string& url) {
            if (url.find("/missing/") != std::string::npos) throw std::runtime_error("HTTP 403");
            return "<files><file name='Alpha.chd'><size>3</size><sha1>a9993e364706816aba3e25717850c26c9cd0d89d</sha1></file>"
                   "<file name='Beta.chd' private='true'><size>3</size><md5>900150983cd24fb0d6963f7d28e17f72</md5></file>"
                   "<file name='NoChecksum.chd'><size>3</size></file></files>";
        });
        model.wait();
        state = model.snapshot();
        require(state.catalog.errors.size() == 1 && state.catalog.files.size() == 3, "Partial scan retains available files");
        model.enqueue({0, 0, 1});
        require(model.snapshot().queue.size() == 2, "Partial catalog selections are queued and deduplicated");
        rejects([&] { model.enqueue({100}); });
        rejects([&] { model.enqueue({2}); });
        config.s3.secret_key = "test-secret";
        rejects([&] { model.start_downloads(config); });
        config.s3.secret_key.clear();
        config.download_dir = directory;
        config.s3 = {"test-access", "test-secret"};
        std::filesystem::create_directories(directory);
        { std::ofstream output(directory / "Alpha.chd", std::ios::binary); output << "abc"; }
        { std::ofstream output(directory / "Beta.chd", std::ios::binary); output << "abc"; }
        model.start_downloads(config);
        model.wait();
        state = model.snapshot();
        require(state.queue.empty() && state.download_revision == 2, "Successful verified downloads are automatically removed when the queue finishes");
        require(state.catalog.errors.size() == 1, "Downloading never marks a partial catalog complete");
        model.enqueue({0, 1});
        model.start_downloads(config, [](const File& file, const std::filesystem::path& folder, bool, const TransferHooks& hooks) {
            if (file.name == "Beta.chd") throw std::runtime_error("Simulated transfer failure");
            verify_file(file, folder / std::filesystem::u8path(file.name).filename(), hooks);
        });
        model.wait();
        state = model.snapshot();
        require(state.queue.size() == 1 && state.queue.front().file.name == "Beta.chd" &&
            state.queue.front().status == QueueStatus::Failed && state.queue.front().detail == "Simulated transfer failure",
            "A finished mixed queue retains only failed downloads and their error details");
        const auto failed_id = state.queue.front().id;
        model.enqueue({0});
        const auto pending_id = model.snapshot().queue.back().id;
        int retries = 0;
        model.start_downloads(config, [&](const File& file, const std::filesystem::path& folder, bool, const TransferHooks& hooks) {
            require(file.name == "Beta.chd", "Per-entry retry must not start another queued file");
            ++retries;
            verify_file(file, folder / std::filesystem::u8path(file.name).filename(), hooks);
        }, failed_id);
        model.wait();
        state = model.snapshot();
        require(retries == 1 && state.queue.size() == 1 && state.queue.front().id == pending_id && state.queue.front().status == QueueStatus::Pending,
            "Per-entry retry removes only the successfully retried file and leaves pending work untouched");
        rejects([&] { model.start_downloads(config, {}, failed_id); });
        rejects([&] { model.start_downloads(config, {}, pending_id); });
        model.start_downloads(config);
        model.wait();
        require(model.snapshot().queue.empty(), "Successful retry removes the last failed item");
        {
            GuiModel live;
            Config live_config;
            live_config.sources = {"https://archive.org/details/example"};
            live_config.follow_parts = false;
            live_config.download_dir = directory / "live-removal";
            std::filesystem::create_directories(live_config.download_dir);
            live.start_scan(live_config, [](const std::string&) {
                return "<files><file name='Alpha.chd'><size>3</size><md5>900150983cd24fb0d6963f7d28e17f72</md5></file>"
                       "<file name='Beta.chd'><size>3</size><md5>900150983cd24fb0d6963f7d28e17f72</md5></file>"
                       "<file name='Gamma.chd'><size>3</size><md5>900150983cd24fb0d6963f7d28e17f72</md5></file></files>";
            });
            live.wait();
            std::mutex gate;
            std::condition_variable changed;
            bool started = false, release = false;
            auto transfer = [&](const File& file, const std::filesystem::path& folder, bool, const TransferHooks& hooks) {
                if (file.name == "Alpha.chd") {
                    { std::ofstream output(folder / "Alpha.chd.part", std::ios::binary); output << "a"; }
                    hooks.progress(1, 3);
                    std::unique_lock<std::mutex> lock(gate);
                    started = true;
                    changed.notify_all();
                    if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return release; })) throw std::runtime_error("Transfer gate timed out");
                    lock.unlock();
                    if (hooks.cancelled()) throw OperationCancelled();
                }
                { std::ofstream output(folder / file.name, std::ios::binary); output << "abc"; }
                hooks.progress(3, 3);
            };
            auto await_started = [&] {
                std::unique_lock<std::mutex> lock(gate);
                require(changed.wait_for(lock, std::chrono::seconds(5), [&] { return started; }), "Active transfer started");
            };
            auto unblock = [&] {
                std::lock_guard<std::mutex> lock(gate);
                release = true;
                changed.notify_all();
            };
            live.enqueue({0, 1, 2});
            { std::ofstream output(live_config.download_dir / "Beta.chd.part"); output << "b"; }
            live.start_downloads(live_config, transfer);
            await_started();
            require(live.snapshot().downloading && live.snapshot().queue[0].received == 1, "Live progress is published during transfer");
            live.remove(1);
            require(live.snapshot().queue.size() == 2 && !std::filesystem::exists(live_config.download_dir / "Beta.chd.part"), "Pending entry removal works while downloading and deletes its partial");
            live.remove(0);
            require(live.snapshot().queue[0].removal_requested, "Active removal cancels only that transfer");
            auto removed_id = live.snapshot().queue[0].id;
            unblock();
            live.wait();
            live.remove_entry(removed_id);
            auto result = live.snapshot();
            require(result.queue.empty() && result.download_revision == 1 && std::filesystem::exists(live_config.download_dir / "Gamma.chd"),
                    "Removing active and pending entries preserves callbacks and continues remaining downloads");
            require(!std::filesystem::exists(live_config.download_dir / "Alpha.chd.part") && !std::filesystem::exists(live_config.download_dir / "Beta.chd"), "Removed transfers leave no partial and pending removal prevents download");
            live.clear_finished();
            live.enqueue({0, 1});
            { std::lock_guard<std::mutex> lock(gate); started = false; release = false; }
            live.start_downloads(live_config, transfer);
            await_started();
            live.cancel();
            unblock();
            live.wait();
            require(live.snapshot().queue[0].status == QueueStatus::Paused && std::filesystem::exists(live_config.download_dir / "Alpha.chd.part"), "Stop alone retains resumable partial download");
            auto pending_partial = live.snapshot().queue[1].partial_path;
            live.enqueue({1}, true);
            require(live.snapshot().queue[1].partial_path == pending_partial, "Requeueing overwrite approval preserves the partial cleanup location");
            { std::ofstream output(live_config.download_dir / "Alpha.chd"); output << "old"; }
            live.remove(0);
            require(!std::filesystem::exists(live_config.download_dir / "Alpha.chd.part") && std::filesystem::exists(live_config.download_dir / "Alpha.chd"), "Removing stopped entry deletes only its partial, never final output");
                std::filesystem::create_directory(live_config.download_dir / "Beta.chd.part");
                rejects([&] { live.remove(0); });
                require(live.snapshot().queue.size() == 1 && std::filesystem::is_directory(live_config.download_dir / "Beta.chd.part"),
                    "Unsafe partial paths are preserved and failed removal keeps the queue entry");
                std::filesystem::remove(live_config.download_dir / "Beta.chd.part");
                live.remove(0);
        }
        model.enqueue({0}, true);
        require(model.snapshot().queue[0].overwrite && model.snapshot().queue[0].status == QueueStatus::Pending,
            "Explicit overwrite requeues an already verified entry");
        { std::ofstream output(directory / "Alpha.chd.part", std::ios::binary); output << "abc"; }
        model.start_downloads(config);
        model.wait();
        require(model.snapshot().queue.empty() && !std::filesystem::exists(directory / "Alpha.chd.part"),
            "Per-entry overwrite reaches the downloader");
        model.export_files(directory / "catalog");
        model.wait();
        model.import_catalog(directory / "catalog" / "catalog.xml");
        model.wait();
        model.enqueue({0});
        state = model.snapshot();
        require(state.queue.size() == 1 && state.catalog.errors.size() == 1, "Imported partial catalog permits queueing and retains warnings");
        {
            GuiModel concurrent;
            auto seed = config;
            seed.sources = {"https://archive.org/details/concurrent-3", "https://archive.org/details/concurrent-7"};
            const std::string files = "<files><file name='Concurrent.chd'><size>3</size><sha1>a9993e364706816aba3e25717850c26c9cd0d89d</sha1></file></files>";
            concurrent.start_scan(seed, [&](const std::string&) { return files; });
            concurrent.wait();
            std::vector<SourceEntry> links;
            for (int index = 0; index < 8; ++index) {
                auto identifier = "concurrent-" + std::to_string(index);
                links.push_back({"https://archive.org/details/" + identifier, identifier, index != 7, "Previous scan", {}});
            }
            concurrent.restore_sources(links);
            std::mutex gate;
            std::condition_variable changed;
            int entered = 0;
            int active = 0;
            int peak = 0;
            int finished = 0;
            bool release_fast = false;
            bool release_slow = false;
            concurrent.scan_selected_sources(config, [&](const std::string& url) {
                std::unique_lock<std::mutex> lock(gate);
                ++entered;
                ++active;
                if (active > peak) peak = active;
                changed.notify_all();
                bool slow = url.find("concurrent-0/") != std::string::npos;
                bool released = changed.wait_for(lock, std::chrono::seconds(5), [&] { return slow ? release_slow : release_fast; });
                --active;
                ++finished;
                changed.notify_all();
                if (!released) throw std::runtime_error("Concurrent scan test timed out");
                if (url.find("concurrent-3/") != std::string::npos) throw std::runtime_error("HTTP 503");
                if (url.find("_files.xml") == std::string::npos) throw std::runtime_error("Unexpected related discovery");
                return files;
            });
            bool overlapped = false;
            bool refilled = false;
            bool live_visible = false;
            {
                std::unique_lock<std::mutex> lock(gate);
                overlapped = changed.wait_for(lock, std::chrono::seconds(3), [&] { return entered == 3; });
                release_fast = true;
                changed.notify_all();
                refilled = changed.wait_for(lock, std::chrono::seconds(3), [&] { return finished >= 6; });
                live_visible = changed.wait_for(lock, std::chrono::seconds(3), [&] {
                    auto live = concurrent.snapshot();
                    return live.busy && live.catalog.files.size() == 7 && !live.sources[3].error.empty() &&
                        live.sources[0].last_scanned == "Previous scan" && live.sources[1].last_scanned != "Previous scan";
                });
                release_slow = true;
                changed.notify_all();
            }
            concurrent.wait();
            require(overlapped && peak == 3 && entered == 7, "Scan queue runs exactly three concurrent requests and skips unchecked links");
            require(refilled, "Free scan slots immediately take waiting links without waiting for a slow first source");
            require(live_visible, "Completed source files, timestamps and errors appear while a slow source is still scanning");
            auto completed = concurrent.snapshot();
            require(completed.catalog.files.size() == 8 && completed.catalog.errors.size() == 1 &&
                completed.sources[3].last_scanned == "Previous scan" && !completed.sources[3].error.empty() &&
                completed.sources[7].last_scanned == "Previous scan" && completed.sources[0].last_scanned != "Previous scan",
                "Parallel scans merge successes while preserving failed and unchecked source results");
            concurrent.enqueue({0});
            auto before_cancel = concurrent.snapshot();
            entered = 0;
            bool release_cancelled = false;
            concurrent.scan_selected_sources(config, [&](const std::string& url) {
                std::unique_lock<std::mutex> lock(gate);
                ++entered;
                changed.notify_all();
                if (url.find("concurrent-0/") != std::string::npos) return std::string("<files/>");
                changed.wait_for(lock, std::chrono::seconds(5), [&] { return release_cancelled; });
                return files;
            });
            bool cancellation_overlap = false;
            {
                std::unique_lock<std::mutex> lock(gate);
                cancellation_overlap = changed.wait_for(lock, std::chrono::seconds(3), [&] {
                    return entered >= 3 && concurrent.snapshot().catalog.files.size() == 7;
                });
                concurrent.cancel();
                release_cancelled = true;
                changed.notify_all();
            }
            concurrent.wait();
            auto cancelled = concurrent.snapshot();
            require(cancellation_overlap && entered <= 4 && !cancelled.busy &&
                cancelled.source_revision > before_cancel.source_revision && cancelled.queue.size() == 1 &&
                cancelled.catalog.files.size() == 7 && cancelled.sources[1].last_scanned == before_cancel.sources[1].last_scanned,
                "Cancellation joins active scans, skips waiting links, keeps the queue and retains already published source updates");
            concurrent.restore_sources({});
            require(concurrent.snapshot().sources.empty() && concurrent.snapshot().catalog.files.size() == 7,
                "Deleting all source links preserves the existing library");
        }
        {
            GuiModel sources;
            rejects([&] { sources.add_source("https://example.com/details/example", false); });
            int metadata_calls = 0;
            auto metadata = [&](const std::string& url) {
            ++metadata_calls;
            if (url.find("example-part-0_meta.xml") == std::string::npos) {
                throw std::runtime_error("Add must not fetch discovered pages or recurse");
            }
            if (url.find("example-part-0") != std::string::npos) return std::string(
                "<metadata><title>Collection &amp; One</title><description>&lt;a href='/details/example-part-1'&gt;Part 1&lt;/a&gt;"
                "&lt;a href='/details/example-part-1'&gt;Duplicate&lt;/a&gt;</description></metadata>");
            return std::string("<metadata><title>Collection Two</title></metadata>");
            };
            sources.add_source("https://archive.org/details/example-part-0", true, {}, metadata);
            sources.wait();
            auto listed = sources.snapshot();
            require(listed.sources.size() == 2 && listed.sources[0].name == "Collection & One" && listed.catalog.files.empty(),
                "Add validates metadata, resolves names and related links without scanning manifests");
            require(metadata_calls == 1 && listed.sources[1].name == "example-part-1",
                "Related links use item IDs without additional metadata requests or recursive discovery");
            GuiModel case_sensitive;
            int case_requests = 0;
            case_sensitive.add_source("https://archive.org/details/ps2-redump-usa-chd-part-0", true, {},
                [&](const std::string& url) {
                    ++case_requests;
                    require(url == "https://archive.org/download/ps2-redump-usa-chd-part-0/ps2-redump-usa-chd-part-0_meta.xml",
                        "Case-preserving discovery still requests only primary metadata");
                    return std::string("<metadata><description>K - &lt;a href=\"https://archive.org/details/ps2-redump-usa-chd-part-K\" "
                        "rel=\"ugc nofollow\"&gt;https://archive.org/details/ps2-redump-usa-chd-part-K&lt;/a&gt;&lt;br /&gt;"
                        "</description></metadata>");
                });
            case_sensitive.wait();
            auto case_sources = case_sensitive.snapshot().sources;
            require(case_requests == 1 && case_sources.size() == 2 &&
                case_sources[1].url == "https://archive.org/details/ps2-redump-usa-chd-part-K",
                "GUI discovery preserves uppercase K from part-0");
            auto case_path = directory / "case-sources.xml";
            save_source_list(case_path, case_sources);
            case_sensitive.restore_sources(read_source_list(case_path));
            case_sensitive.scan_selected_sources(config, [](const std::string& url) {
                if (url.find("ps2-redump-usa-chd-part-0") == std::string::npos &&
                    url != "https://archive.org/download/ps2-redump-usa-chd-part-K/ps2-redump-usa-chd-part-K_files.xml") {
                    throw std::runtime_error("Saved discovered identifiers must retain case in scan requests");
                }
                return std::string("<files><file name='Example.chd'><size>3</size></file></files>");
            });
            case_sensitive.wait();
            auto case_scan = case_sensitive.snapshot();
            require(case_scan.catalog.errors.empty() && case_scan.catalog.files.size() == 2 &&
                case_scan.sources[1].url == case_sources[1].url,
                "Source persistence and scanning preserve uppercase K");
            GuiModel primary_only;
            primary_only.add_source("https://archive.org/details/example-part-0", false, {}, metadata);
            primary_only.wait();
            require(metadata_calls == 2 && primary_only.snapshot().sources.size() == 1,
                "Disabling related links adds only the entered item");
            sources.add_source("https://archive.org/download/example-part-0/example-part-0_files.xml", true, {}, metadata);
            sources.wait();
            require(sources.snapshot().sources.size() == 2, "Duplicate identifiers are not added twice");
            sources.add_source("https://archive.org/details/bad", false, {}, [](const std::string&) { return "<html/>"; });
            sources.wait();
            require(sources.snapshot().sources.size() == 2, "Invalid remote metadata is not added");
            sources.select_source(1, false);
            auto tagged_url = sources.snapshot().sources[0].url;
            auto tag_revision = sources.snapshot().source_revision;
            sources.set_source_tags(tagged_url, {" PS2 ", "USA", "ps2", "A & B", ""});
            require(sources.snapshot().sources[0].tags == std::vector<std::string>({"PS2", "USA", "A & B"}) &&
                sources.snapshot().source_revision > tag_revision, "Tags are trimmed, deduplicated and publish a source revision");
            rejects([&] { sources.set_source_tags(tagged_url, {std::string(129, 'x')}); });
            rejects([&] { sources.set_source_tags(tagged_url, {"bad\ntag"}); });
            rejects([&] { sources.set_source_tags(tagged_url, std::vector<std::string>(33, "tag")); });
            rejects([&] { sources.set_source_tags("https://archive.org/details/missing", {"PS2"}); });
            int calls = 0;
            auto manifest = [&](const std::string&) {
            ++calls;
            return std::string("<files><file name='First.chd'><size>3</size><sha1>a9993e364706816aba3e25717850c26c9cd0d89d</sha1></file></files>");
            };
            sources.scan_selected_sources(config, manifest);
            sources.wait();
            listed = sources.snapshot();
            require(calls == 1 && listed.catalog.files.size() == 1 && !listed.sources[0].last_scanned.empty() && listed.sources[1].last_scanned.empty(),
                "Scan only selected links and timestamp successful scans");
            sources.select_source(0, false);
            sources.select_source(1, true);
            sources.scan_selected_sources(config, manifest);
            sources.wait();
            require(sources.snapshot().catalog.files.size() == 2, "Selected scan retains unselected source results");
            auto previous_time = sources.snapshot().sources[1].last_scanned;
            sources.scan_selected_sources(config, [](const std::string&) -> std::string { throw std::runtime_error("HTTP 503"); });
            sources.wait();
            listed = sources.snapshot();
            require(listed.catalog.files.size() == 2 && !listed.sources[1].error.empty() && listed.sources[1].last_scanned == previous_time,
                "Failed rescans retain prior files and successful scan time");
            sources.scan_selected_sources(config, [](const std::string&) { return std::string("<files/>"); });
            sources.wait();
            listed = sources.snapshot();
            require(listed.catalog.files.size() == 1 && listed.sources[1].error.empty() && listed.catalog.errors.empty(),
                "Successful empty rescan removes stale files and prior source errors");
            auto saved_sources = directory / "sources.xml";
            save_source_list(saved_sources, listed.sources);
            auto restored = read_source_list(saved_sources);
            require(restored[0].tags == std::vector<std::string>({"PS2", "USA", "A & B"}) && restored[1].tags.empty(),
                "Source tags survive scans and XML round-trip without leaking to other sources");
            require(restored.size() == 2 && !restored[0].selected && restored[1].selected &&
                restored[0].name == "Collection & One" && restored[1].last_scanned == listed.sources[1].last_scanned,
                "Source list round-trips names, checkboxes and timestamps");
            sources.select_all_sources(false);
            rejects([&] { sources.scan_selected_sources(config, manifest); });
            sources.select_all_sources(true);
            sources.remove_source(1);
            save_source_list(saved_sources, sources.snapshot().sources);
            require(read_source_list(saved_sources).size() == 1, "Source list updates replace the prior saved file");
            sources.restore_sources({});
            save_source_list(saved_sources, sources.snapshot().sources);
            require(read_source_list(saved_sources).empty(), "An empty saved list stays empty");
            rejects([&] { sources.restore_sources({{"https://evil.test/details/item", "Invalid"}}); });
            {
            std::ofstream invalid(saved_sources);
            invalid << "<!DOCTYPE sources [<!ENTITY x 'test'>]><sources/>";
            }
            rejects([&] { read_source_list(saved_sources); });
        }
        std::filesystem::remove_all(directory);
        std::cout << "All GUI model tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(directory);
        std::cerr << error.what() << '\n';
        return 1;
    }
}