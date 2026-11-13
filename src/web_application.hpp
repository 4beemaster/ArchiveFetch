#pragma once
// The shared ArchiveFetch backend behind the web interface (Windows, Android and the PS5 app).
#include "gui_model.hpp"
#include "credential_store.hpp"
#include "extraction.hpp"
#include "gui_platform.hpp"
#include "remove_tree.hpp"
#include "release_update.hpp"
#ifndef __ANDROID__
#include <httplib.h>
#endif
#include <nlohmann/json.hpp>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <libxml/tree.h>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <algorithm>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <webview/webview.h>
#include "windows_update.hpp"
#endif

using namespace archive_catalog;
using Json = nlohmann::json;

namespace {
constexpr const char* update_platform =
#if defined(__ANDROID__) && defined(__x86_64__)
    "android-x86_64";
#elif defined(__ANDROID__)
    "android";
#elif defined(_WIN32)
    "windows";
#elif defined(PS5_PAYLOAD)
    "ps5";
#else
    "other";
#endif

constexpr bool named_locations_supported = true;

class DiagnosticLog final : public std::streambuf {
public:
    DiagnosticLog() : previous_(std::cerr.rdbuf(this)) {}
    ~DiagnosticLog() override { std::cerr.rdbuf(previous_); }
    std::string text() { std::lock_guard<std::mutex> guard(mutex_); return text_; }
    void clear() { std::lock_guard<std::mutex> guard(mutex_); text_.clear(); }
protected:
    std::streamsize xsputn(const char* data, std::streamsize size) override {
        std::lock_guard<std::mutex> guard(mutex_);
        text_.append(data, static_cast<std::size_t>(size));
        if (text_.size() > 65536) text_.erase(0, text_.size() - 65536);
        return previous_->sputn(data, size);
    }
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        auto character = traits_type::to_char_type(value);
        return xsputn(&character, 1) == 1 ? value : traits_type::eof();
    }
private:
    std::streambuf* previous_;
    std::mutex mutex_;
    std::string text_;
};

std::filesystem::path executable_directory(const char* command) {
#ifdef _WIN32
    std::vector<wchar_t> path(32768);
    auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) throw std::runtime_error("Cannot locate executable assets");
    return std::filesystem::path(path.data()).parent_path();
#else
    return std::filesystem::absolute(std::filesystem::u8path(command)).parent_path();
#endif
}

// Where extraction puts an archive's files: beside it, named after it. Empty for files that are not archives.
std::filesystem::path extracted_folder(const std::filesystem::path& archive, const std::string& name) {
    const auto folder = archive_folder_name(name);
    return folder.empty() ? std::filesystem::path{} : archive.parent_path() / std::filesystem::u8path(folder);
}

bool real_directory(const std::filesystem::path& path) {
    std::error_code failure;
    return !path.empty() && std::filesystem::is_directory(std::filesystem::symlink_status(path, failure)) && !failure;
}

std::string random_token() {
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) throw std::runtime_error("Cannot create local session");
    std::ostringstream result;
    for (auto byte : bytes) result << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    return result.str();
}

Json file_json(const File& file) {
    return {{"item", file.item}, {"name", file.name}, {"size", file.size}, {"restricted", file.restricted}};
}

Json read_download_state(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) > 16 * 1024 * 1024) throw std::runtime_error("Saved download locations exceed 16 MiB");
    std::ifstream input(path, std::ios::binary);
    return Json::parse(input);
}

void write_download_state(const std::filesystem::path& path, const Json& value) {
    const auto text = value.dump();
    if (text.size() > 16 * 1024 * 1024) throw std::runtime_error("Saved download locations exceed 16 MiB");
    auto temporary = path; temporary += ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size())); output.close();
    if (!output) throw std::runtime_error("Cannot save download locations");
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot replace saved download locations");
#else
    std::filesystem::rename(temporary, path);
#endif
}

std::string location_path(const std::string& value) {
    if (value.empty() || value.size() > 32768 || std::any_of(value.begin(), value.end(), [](unsigned char character) { return character < 32; }))
        throw std::runtime_error("Choose a valid download folder");
#ifdef __ANDROID__
    if (value.rfind("content://", 0) == 0) return value;
#endif
    auto path = std::filesystem::u8path(value);
    if (!path.is_absolute()) throw std::runtime_error("Download locations need an absolute path");
    return path.lexically_normal().u8string();
}

const Json& find_location(const Json& state, const std::string& id) {
    for (const auto& location : state.at("locations")) if (location.at("id") == id) return location;
    throw std::runtime_error("Download location no longer exists");
}

std::string location_tag_key(std::string text) {
    for (auto& character : text) if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    return text;
}

void validate_location_rules(Json& state) {
    const auto preference = state.value("preference", "auto");
    if (preference != "auto" && preference != "file" && preference != "source" && preference != "default") throw std::runtime_error("Invalid location preference");
    state["preference"] = preference;
    std::map<std::string, std::string> owners;
    for (auto& location : state.at("locations")) {
        if (location.at("id") == "auto" || location.at("id") == "fallback") throw std::runtime_error("Reserved location identifier");
        location["tags"] = normalize_tags(location.value("tags", std::vector<std::string>{}));
        for (const auto& tag : location.at("tags").get<std::vector<std::string>>()) {
            const auto owner = owners.emplace(location_tag_key(tag), location.at("name").get<std::string>());
            if (!owner.second) throw std::runtime_error("Tag '" + tag + "' already belongs to " + owner.first->second);
        }
    }
}

class WebApplication {
public:
    GuiModel model;
    Config config;
    GuiModel::Download download;
    // Set only where archives can be extracted (the PS5 build); otherwise the extraction settings stay hidden.
    Extractor extract;
    std::mutex extraction_mutex;
    std::string extraction_error;
    std::function<bool(const File&)> destination_exists;
    std::function<Json(const Json&)> platform_storage;
    GuiModel::PartialPath platform_partial_path;
    std::function<void(const File&, const std::filesystem::path&, const TransferHooks&)> platform_record_download;
    std::function<void(const File&, const std::filesystem::path&, const TransferHooks&)> platform_extract_after_download;
    std::function<void()> open_s3_keys;
    std::function<void(const std::string&)> open_update;
    UpdateCheck updates;
    ReleaseUpdate release_update;
    std::function<void()> retry_update_completion;
    std::filesystem::path root;
    std::filesystem::path settings;
    std::filesystem::path sources;
    std::unique_ptr<CredentialStore> credentials;
    std::map<std::pair<std::string, std::string>, File> favorites;
    std::map<std::pair<std::string, std::string>, std::vector<std::string>> file_tags;
    std::uint64_t tag_revision = 0;
    bool tags_writable = true;
    std::uint64_t saved_catalog = 0;
    std::uint64_t saved_sources = 0;
    std::string error;
    bool remember = false;
    bool favorites_writable = true;
    std::mutex download_state_mutex;
    Json download_state;
    bool download_state_writable = true;
    std::uint64_t location_revision = 0;
    struct SourceCollection { std::string id, name; std::vector<std::string> tags, items; };
    std::vector<SourceCollection> collections;
    bool collections_writable = true;

    explicit WebApplication(const std::filesystem::path& isolated = {}) {
#ifdef _WIN32
        release_update.installer = start_windows_update;
        open_update = [](const std::string& url) {
            const std::wstring target(url.begin(), url.end());
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                throw std::runtime_error("Cannot open the browser for the ArchiveFetch release");
        };
        open_s3_keys = [] {
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"https://archive.org/login?referer=%2Faccount%2Fs3.php", nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                throw std::runtime_error("Cannot open the browser for Internet Archive S3 keys");
        };
#endif
        root = isolated.empty() ? CredentialStore::default_path().parent_path() : isolated;
        std::filesystem::create_directories(root);
        settings = isolated.empty() ? std::filesystem::path("gui-config.xml") : root / "settings.xml";
        sources = isolated.empty() ? std::filesystem::path("gui-sources.xml") : root / "sources.xml";
        try {
            if (std::filesystem::exists(settings)) config = read_config(settings, true);
            else if (isolated.empty() && std::filesystem::exists("config.example.xml")) config = read_config("config.example.xml");
            if (std::filesystem::exists(sources)) model.restore_sources(read_source_list(sources));
            else {
                std::vector<SourceEntry> entries;
                for (const auto& url : config.sources) entries.push_back({url, item_id(url), true, {}, {}});
                model.restore_sources(std::move(entries));
            }
            saved_sources = model.snapshot().source_revision;
        } catch (const std::exception& failure) { error = failure.what(); sources.clear(); }
        auto library = root / "library.xml";
        if (!std::filesystem::exists(library) && isolated.empty()) library = config.output_dir / "catalog.xml";
        if (std::filesystem::exists(library)) { model.import_catalog(library); model.wait(); }
        try {
            if (std::filesystem::exists(root / "favorites.xml")) {
                for (const auto& file : read_catalog(root / "favorites.xml").files) favorites[{file.item, file.name}] = file;
            }
        } catch (const std::exception& failure) { error = failure.what(); favorites_writable = false; }
        try {
            const auto path = root / "file-tags.json";
            if (std::filesystem::exists(path)) {
                if (std::filesystem::file_size(path) > 16 * 1024 * 1024) throw std::runtime_error("Saved file tags exceed 16 MiB");
                std::ifstream input(path, std::ios::binary);
                const auto entries = Json::parse(input);
                if (!entries.is_array()) throw std::runtime_error("Saved file tags must be an array");
                decltype(file_tags) loaded;
                for (const auto& entry : entries) {
                    auto key = std::make_pair(entry.at("item").get<std::string>(), entry.at("name").get<std::string>());
                    loaded.emplace(std::move(key), normalize_tags(entry.at("tags").get<std::vector<std::string>>()));
                }
                file_tags = std::move(loaded);
            }
        } catch (const std::exception& failure) { error = failure.what(); tags_writable = false; }
        try {
            const auto path = root / "collections.json";
            if (std::filesystem::exists(path)) {
                if (std::filesystem::file_size(path) > 1024 * 1024) throw std::runtime_error("Saved collections exceed 1 MiB");
                std::ifstream input(path, std::ios::binary);
                const auto saved = Json::parse(input);
                if (saved.at("version") != 1 || !saved.at("collections").is_array()) throw std::runtime_error("Invalid saved collections");
                std::vector<SourceCollection> loaded;
                for (const auto& entry : saved.at("collections"))
                    loaded.push_back({entry.at("id").get<std::string>(), entry.at("name").get<std::string>(),
                        entry.at("tags").get<std::vector<std::string>>(), entry.at("sources").get<std::vector<std::string>>()});
                validate_collections(loaded);
                collections = std::move(loaded);
            }
        } catch (const std::exception& failure) { error = failure.what(); collections_writable = false; }
        download_state = {{"version", 1}, {"preference", "auto"}, {"default", "default"}, {"last", ""}, {"locations", Json::array({
            {{"id", "default"}, {"name", "Default"}, {"tags", Json::array()}, {"path",
#ifdef __ANDROID__
                config.download_dir.u8string()
#else
                std::filesystem::absolute(config.download_dir).lexically_normal().u8string()
#endif
            }}
        })}, {"records", Json::array()}};
        try {
            if (std::filesystem::exists(root / "download-locations.json")) {
                auto loaded = read_download_state(root / "download-locations.json");
                if (loaded.at("version") != 1 || !loaded.at("locations").is_array() || loaded.at("locations").empty() || loaded.at("locations").size() > 128 || !loaded.at("records").is_array())
                    throw std::runtime_error("Invalid saved download locations");
                std::set<std::string> identifiers, names;
                for (auto& location : loaded.at("locations")) {
                    const auto id = location.at("id").get<std::string>();
                    auto labels = normalize_tags({location.at("name").get<std::string>()});
                    if (id.empty() || id.size() > 64 || labels.size() != 1 || !identifiers.insert(id).second) throw std::runtime_error("Invalid saved download location");
                    auto folded = labels.front();
                    for (auto& character : folded) if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
                    if (!names.insert(folded).second || (id == "default" && labels.front() != "Default")) throw std::runtime_error("Duplicate or renamed default download location");
                    location["path"] = location_path(location.at("path").get<std::string>());
                    if (location.contains("extract") && !location.at("extract").is_boolean()) throw std::runtime_error("Invalid saved download location");
                }
                find_location(loaded, "default"); find_location(loaded, loaded.at("default").get<std::string>());
                if (loaded.at("last") != "" && loaded.at("last") != "auto" && loaded.at("last") != "fallback") find_location(loaded, loaded.at("last").get<std::string>());
                validate_location_rules(loaded);
                for (const auto& record : loaded.at("records")) {
                    record.at("item").get<std::string>(); record.at("name").get<std::string>();
                    location_path(record.at("path").get<std::string>());
                    if (record.contains("extracted")) location_path(record.at("extracted").get<std::string>());
                    record.at("size").get<std::uint64_t>(); record.at("modified").get<std::int64_t>();
                }
                download_state = std::move(loaded);
#ifndef __ANDROID__
                config.download_dir = std::filesystem::u8path(find_location(download_state, download_state.at("default")).at("path").get<std::string>());
#endif
            }
        } catch (const std::exception& failure) { error = failure.what(); download_state_writable = false; }
        if (CredentialStore::supported()) {
            credentials = std::make_unique<CredentialStore>(isolated.empty() ? CredentialStore::default_path() : root / "credentials.bin");
            try {
                if (auto saved = credentials->load()) { config.s3 = *saved; remember = true; }
            } catch (const std::exception& failure) { error = failure.what(); }
        }
        release_update.configure(root, ARCHIVEFETCH_VERSION, update_platform);
    }

    ~WebApplication() {
        model.cancel(); model.wait();
        persist();
        OPENSSL_cleanse(config.s3.access_key.data(), config.s3.access_key.size());
        OPENSSL_cleanse(config.s3.secret_key.data(), config.s3.secret_key.size());
    }

    void persist() {
        auto state = model.snapshot();
        try {
            if (state.loaded && saved_catalog != state.catalog_revision) {
                save_catalog(state.catalog, root / "library.xml");
                saved_catalog = state.catalog_revision;
            }
            if (!state.scanning && !sources.empty() && saved_sources != state.source_revision) {
                save_source_list(sources, state.sources);
                saved_sources = state.source_revision;
            }
        } catch (const std::exception& failure) { error = failure.what(); }
    }

    Json snapshot() {
        persist();
        sync_collections();
        auto state = model.snapshot();
        Json result = {{"busy", state.busy}, {"scanning", state.scanning}, {"downloading", state.downloading}, {"loaded", state.loaded},
            {"activity", state.activity}, {"notice", state.notice}, {"error", error}, {"count", std::count_if(state.catalog.files.begin(), state.catalog.files.end(),
                [&](const File& file) { return !excluded_file_type(file.name, config.excluded_extensions); })},
            {"catalogRevision", state.catalog_revision}, {"downloadRevision", state.download_revision},
            {"sourceRevision", state.source_revision}, {"tagRevision", tag_revision}, {"errors", state.catalog.errors},
            {"extractionError", extraction_message()},
            {"sources", Json::array()}, {"collections", Json::array()}, {"queue", Json::array()}, {"settings", settings_json()}};
        for (const auto& source : state.sources) {
            const auto* collection = collection_of(item_id(source.url));
            result["sources"].push_back({{"url", source.url}, {"name", source.name},
                {"item", item_id(source.url)}, {"tags", source.tags}, {"collection", collection ? collection->id : ""},
                {"selected", source.selected}, {"scanned", source.last_scanned}, {"error", source.error}});
        }
        for (const auto& collection : collections)
            result["collections"].push_back({{"id", collection.id}, {"name", collection.name}, {"tags", collection.tags}});
        result["collectionsWritable"] = collections_writable;
        for (const auto& entry : state.queue) {
            auto value = file_json(entry.file);
            value.update({{"id", entry.id}, {"status", queue_status_name(entry.status)}, {"received", entry.received},
                {"detail", entry.detail}, {"removing", entry.removal_requested}, {"pausing", entry.pause_requested && entry.status == QueueStatus::Active}, {"destination", entry.destination.u8string()},
                {"stage", entry.stage}, {"stageDone", entry.stage_done}, {"stageTotal", entry.stage_total}, {"rate", entry.rate}});
            result["queue"].push_back(value);
        }
        return result;
    }

    Json settings_json() {
        std::lock_guard<std::mutex> guard(download_state_mutex);
        return {{"destination", config.download_dir.u8string()}, {"output", config.output_dir.u8string()},
            {"locations", download_state.at("locations")}, {"defaultLocation", download_state.at("default")}, {"lastLocation", download_state.at("last")},
            {"locationRevision", location_revision}, {"locationsWritable", download_state_writable},
            {"locationsSupported", named_locations_supported},
            {"locationPreference", download_state.at("preference")},
            {"ca", config.ca_bundle}, {"automatic", config.automatically_start_downloads},
            {"extractSupported", static_cast<bool>(extract)}, {"extractArchives", config.extract_archives},
            {"deleteExtracted", config.delete_extracted_archives},
            {"connections", config.download_connections}, {"maxConnections", max_download_connections},
            {"excludedExtensions", format_extension_list(config.excluded_extensions)},
            {"authenticated", !config.s3.access_key.empty() && !config.s3.secret_key.empty()}, {"remember", remember},
            {"keysProtected", CredentialStore::protected_storage()}};
    }

    Json library(bool include_hidden = false) {
        auto state = model.snapshot();
        auto status = platform_storage ? std::vector<bool>(state.catalog.files.size(), false) : library_download_status(state.catalog.files, config.download_dir);
        std::lock_guard<std::mutex> guard(download_state_mutex);
        auto updated = download_state;
        bool adopted = false;
        std::map<std::pair<std::string, std::string>, std::vector<Json>> history;
        std::set<std::string> recorded_paths;
        for (const auto& record : updated.at("records")) {
            history[{record.at("item").get<std::string>(), record.at("name").get<std::string>()}].push_back(record);
            recorded_paths.insert(record.at("path").get<std::string>());
        }
        const auto target_directory = platform_storage ? config.download_dir : std::filesystem::absolute(config.download_dir).lexically_normal();
        // Each location folder is listed once, so a download without a record (made outside the app, or whose record
        // went with its library entry) is recognized in any location, not only the default one.
        struct FolderListing { std::filesystem::path folder; std::map<std::string, std::uintmax_t> files; std::set<std::string> folders; };
        std::vector<FolderListing> listings;
        {
            std::vector<std::filesystem::path> folders{target_directory};
            for (const auto& location : updated.at("locations")) {
                if (platform_storage) continue;
                const auto folder = std::filesystem::absolute(std::filesystem::u8path(location.at("path").get<std::string>())).lexically_normal();
                if (std::find(folders.begin(), folders.end(), folder) == folders.end()) folders.push_back(folder);
            }
            for (const auto& folder : folders) {
                if (platform_storage) continue;
                FolderListing listing{folder, {}, {}};
                std::error_code failure;
                for (std::filesystem::directory_iterator entry(folder, failure), end; !failure && entry != end; entry.increment(failure)) {
                    std::error_code kind_failure;
                    const auto kind = entry->symlink_status(kind_failure);
                    if (kind_failure) continue;
                    const auto name = entry->path().filename().u8string();
                    if (std::filesystem::is_regular_file(kind)) listing.files[name] = entry->file_size(kind_failure);
                    else if (std::filesystem::is_directory(kind)) listing.folders.insert(name);
                }
                listings.push_back(std::move(listing));
            }
        }
        std::map<std::string, std::size_t> basenames;
        for (const auto& file : state.catalog.files) ++basenames[std::filesystem::u8path(file.name).filename().u8string()];
        std::map<std::string, std::vector<std::string>> tags;
        std::set<std::string> source_items;
        for (const auto& source : state.sources) { tags[item_id(source.url)] = source.tags; source_items.insert(item_id(source.url)); }
        std::map<std::string, const SourceCollection*> grouped;
        for (const auto& collection : collections) for (const auto& item : collection.items) grouped[item] = &collection;
        Json result = Json::array();
        for (std::size_t index = 0; index < state.catalog.files.size(); ++index) {
            auto value = file_json(state.catalog.files[index]);
            const auto key = std::make_pair(state.catalog.files[index].item, state.catalog.files[index].name);
            const auto custom = file_tags.find(key);
            const auto& file = state.catalog.files[index];
            const auto target = target_directory / std::filesystem::u8path(file.name).filename();
            const bool known_path = recorded_paths.count(target.u8string()) != 0, known_identity = history.count(key) != 0;
#ifndef __ANDROID__
            if (download_state_writable && status[index] && !known_path && !known_identity && basenames[target.filename().u8string()] == 1) {
                std::error_code failure;
                const auto modified = std::filesystem::last_write_time(target, failure);
                if (!failure) {
                    Json record = {{"item", file.item}, {"name", file.name}, {"path", target.u8string()}, {"size", file.size}, {"modified", modified.time_since_epoch().count()}};
                    updated["records"].push_back(record); history[key].push_back(std::move(record)); recorded_paths.insert(target.u8string());
                    adopted = true;
                }
            }
#endif
            std::string downloaded_path, download_status = "Not downloaded";
            bool present = false;
            Json copies = Json::array();
            for (const auto& record : history[key]) {
                if (platform_storage && record.contains("storage")) {
                    auto copy = platform_storage({{"action", "status"}, {"record", record}, {"metadataOnly", true}});
                    copies.push_back(copy);
                    const auto copy_status = copy.at("status").get<std::string>();
                    if (!present) { downloaded_path = record.at("path"); download_status = copy_status; }
                    if (copy_status == "Downloaded" || copy_status == "Extracted") present = true;
                    continue;
                }
                const auto path = std::filesystem::u8path(record.at("path").get<std::string>());
                const auto recorded_folder = record.value("extracted", "");
                const auto folder = !recorded_folder.empty() ? std::filesystem::u8path(recorded_folder) : extract ? extracted_folder(path, file.name) : std::filesystem::path{};
                const bool folder_present = real_directory(folder);
                std::error_code failure;
                const auto disk = std::filesystem::symlink_status(path, failure);
                std::string copy_status;
                if (failure || !std::filesystem::exists(disk)) {
                    failure.clear();
                    copy_status = std::filesystem::is_directory(path.parent_path(), failure) && !failure ? "Missing" : "Location unavailable";
                    // The archive was deleted after extracting; its folder is what was kept.
                    if (folder_present) copy_status = "Extracted";
                } else if (!std::filesystem::is_regular_file(disk)) copy_status = "Changed";
                else {
                    const auto size = std::filesystem::file_size(path, failure);
                    const auto modified = std::filesystem::last_write_time(path, failure);
                    copy_status = !failure && record.at("size") == file.size && size == file.size && record.at("modified") == modified.time_since_epoch().count() ? "Downloaded" : "Changed";
                }
                Json copy = {{"path", path.u8string()}, {"status", copy_status}};
                if (folder_present || !recorded_folder.empty()) copy["extracted"] = folder.u8string();
                copies.push_back(std::move(copy));
                if (!present) { downloaded_path = path.u8string(); download_status = copy_status; }
                if (copy_status == "Downloaded" || copy_status == "Extracted") present = true;
            }
            if (platform_storage && copies.empty() && basenames[target.filename().u8string()] == 1) {
                std::set<std::string> locations;
                for (const auto& location : updated.at("locations")) locations.insert(location.at("path").get<std::string>());
                for (const auto& location : locations) {
                    Json found;
                    try {
                        found = platform_storage({{"action", "discover"}, {"tree", location},
                            {"name", std::filesystem::u8path(file.name).filename().u8string()}, {"size", file.size}, {"sha1", file.sha1}, {"md5", file.md5}, {"metadataOnly", true}});
                    } catch (const std::exception& failure) {
                        std::cerr << "[storage] Cannot inspect download location: " << failure.what() << '\n';
                        continue;
                    }
                    if (!found.at("found").get<bool>()) continue;
                    const auto path = found.at("path").get<std::string>();
                    if (recorded_paths.count(path)) continue;
                    if (found.contains("record") && download_state_writable) {
                        auto record = found.at("record");
                        record["item"] = file.item; record["name"] = file.name; record["size"] = file.size;
                        updated["records"].push_back(record); recorded_paths.insert(path); adopted = true;
                    }
                    copies.push_back({{"path", path}, {"status", found.at("status")}});
                    if (!present) { downloaded_path = path; download_status = found.at("status"); }
                    present = true;
                }
            }
            if (copies.empty() && basenames[target.filename().u8string()] == 1) {
                const auto filename = target.filename().u8string();
                const auto folder_name = extract ? archive_folder_name(file.name) : std::string{};
                for (const auto& listing : listings) {
                    const auto archive = listing.folder / std::filesystem::u8path(filename);
                    if (recorded_paths.count(archive.u8string())) continue;
                    const auto found = listing.files.find(filename);
                    const bool whole = found != listing.files.end() && found->second == file.size;
                    const bool unpacked = !folder_name.empty() && listing.folders.count(folder_name);
                    if (!whole && !unpacked) continue;
                    present = true; downloaded_path = archive.u8string(); download_status = whole ? "Downloaded" : "Extracted";
                    Json copy = {{"path", downloaded_path}, {"status", download_status}};
                    if (unpacked) copy["extracted"] = (listing.folder / std::filesystem::u8path(folder_name)).u8string();
                    copies.push_back(std::move(copy));
                    break;
                }
            }
            if (!include_hidden && !present && excluded_file_type(file.name, config.excluded_extensions)) continue;
            value.update({{"index", index}, {"downloaded", status[index]}, {"sourceTags", tags[state.catalog.files[index].item]},
                {"tags", custom == file_tags.end() ? std::vector<std::string>{} : custom->second},
                {"favorite", favorites.count({state.catalog.files[index].item, state.catalog.files[index].name}) != 0}});
            value.update({{"downloaded", present}, {"downloadPath", downloaded_path}, {"downloadState", download_status}, {"downloadCopies", copies}});
            const auto group = grouped.find(file.item);
            value.update({{"collectionId", group == grouped.end() ? "" : group->second->id}, {"collection", group == grouped.end() ? "" : group->second->name},
                {"sourceRemoved", source_items.count(file.item) == 0}});
            result.push_back(value);
        }
        if (adopted) commit_download_state(std::move(updated));
        return {{"revision", state.catalog_revision}, {"sourceRevision", state.source_revision}, {"tagRevision", tag_revision}, {"files", result}};
    }

    void commit_download_state(Json next) {
        if (!download_state_writable) throw std::runtime_error("Saved download locations are invalid; original file was left unchanged");
        write_download_state(root / "download-locations.json", next);
        download_state = std::move(next); ++location_revision;
    }

    Json location_command(const std::string& action, const Json& value) {
        if (!named_locations_supported) throw std::runtime_error("Named locations are currently supported by the desktop application");
        std::lock_guard<std::mutex> guard(download_state_mutex);
        auto next = download_state;
        auto id = value.value("id", "");
        if (action == "addLocation" || action == "editLocation") {
            const auto labels = normalize_tags({value.at("name").get<std::string>()});
            if (labels.size() != 1) throw std::runtime_error("Give the download location a nickname");
            const auto path = location_path(value.at("path").get<std::string>());
            if (platform_storage) platform_storage({{"action", "validate"}, {"tree", path}});
            if (action == "addLocation") {
                if (next.at("locations").size() >= 128) throw std::runtime_error("Use at most 128 download locations");
                id = random_token().substr(0, 16);
            } else find_location(next, id);
            if (id == "default" && labels.front() != "Default") throw std::runtime_error("The Default location cannot be renamed");
            const auto folded = [](std::string name) { for (auto& character : name) if (character >= 'A' && character <= 'Z') character += 'a' - 'A'; return name; };
            for (const auto& location : next.at("locations")) if (location.at("id") != id && folded(location.at("name")) == folded(labels.front())) throw std::runtime_error("A location already uses that nickname");
            Json entry = {{"id", id}, {"name", labels.front()}, {"path", path}, {"tags", value.value("tags", std::vector<std::string>{})},
                {"extract", value.value("extract", false)}};
            if (action == "addLocation") next["locations"].push_back(std::move(entry));
            else for (auto& location : next["locations"]) if (location.at("id") == id) location = entry;
        } else {
            if (action != "useLocation" || (id != "auto" && id != "fallback")) find_location(next, id);
            if (action == "removeLocation") {
                if (id == "default") throw std::runtime_error("The Default location cannot be removed");
                auto& locations = next["locations"];
                locations.erase(std::remove_if(locations.begin(), locations.end(), [&](const Json& entry) { return entry.at("id") == id; }), locations.end());
                if (next.at("default") == id) next["default"] = "default";
                if (next.at("last") == id) next["last"] = "auto";
            } else if (action == "defaultLocation") next["default"] = id;
            else if (action == "useLocation") next["last"] = id;
            else throw std::runtime_error("Unknown location operation");
        }
        validate_location_rules(next);
        const auto destination = find_location(next, next.at("default")).at("path").get<std::string>();
        commit_download_state(std::move(next)); config.download_dir = std::filesystem::u8path(destination);
        return {{"ok", true}, {"id", id}};
    }

    void record_download(const File& file, const std::filesystem::path& directory) {
        const auto path = std::filesystem::weakly_canonical(std::filesystem::absolute(directory)) / std::filesystem::u8path(file.name).filename();
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)) || std::filesystem::file_size(path) != file.size) throw std::runtime_error("Completed download is not present at its recorded path");
        const auto modified = std::filesystem::last_write_time(path).time_since_epoch().count();
        std::lock_guard<std::mutex> guard(download_state_mutex);
        auto next = download_state;
        auto& records = next["records"];
        records.erase(std::remove_if(records.begin(), records.end(), [&](const Json& record) { return record.at("path") == path.u8string(); }), records.end());
        records.push_back({{"item", file.item}, {"name", file.name}, {"path", path.u8string()}, {"size", file.size}, {"modified", modified}});
        commit_download_state(std::move(next));
    }

    std::string extraction_message() {
        std::lock_guard<std::mutex> guard(extraction_mutex);
        return extraction_error;
    }

    // Runs on the download worker after the file is recorded; a failed or stopped extraction keeps the download.
    void extract_after_download(const File& file, const std::filesystem::path& directory, const TransferHooks& hooks) {
        if (platform_extract_after_download) { platform_extract_after_download(file, directory, hooks); return; }
        const auto folder_name = archive_folder_name(file.name);
        if (!extract || folder_name.empty()) return;
        const auto base = std::filesystem::weakly_canonical(std::filesystem::absolute(directory));
        bool wanted = config.extract_archives;
        {
            std::lock_guard<std::mutex> guard(download_state_mutex);
            for (const auto& location : download_state.at("locations"))
                if (location.value("extract", false) && std::filesystem::u8path(location.at("path").get<std::string>()).lexically_normal() == base.lexically_normal()) wanted = true;
        }
        if (!wanted) return;
        const auto archive = base / std::filesystem::u8path(file.name).filename();
        const auto folder = base / std::filesystem::u8path(folder_name);
        const auto status = [&hooks](const std::string& text) { if (hooks.status) hooks.status(text); };
        std::error_code size_error;
        const auto archive_size = std::filesystem::file_size(archive, size_error);
        const auto progress = [&hooks](std::uint64_t done, std::uint64_t total) { if (hooks.stage) hooks.stage("Extracting", done, total); };
        const std::function<bool()> cancelled = hooks.cancelled ? hooks.cancelled : [] { return false; };
        status("Extracting");
        progress(0, size_error ? 0 : archive_size);
        const auto outcome = extract(archive, folder, cancelled, status, progress);
        std::string problem;
        if (outcome.result == ExtractOutcome::Result::Cancelled) status("Extraction stopped; the archive was kept");
        else if (outcome.result == ExtractOutcome::Result::Failed) problem = "Could not extract " + file.name + ": " + outcome.error + ". The archive was kept.";
        else {
            std::error_code removal;
            const bool deleted = config.delete_extracted_archives && std::filesystem::remove(archive, removal) && !removal;
            {
                std::lock_guard<std::mutex> guard(download_state_mutex);
                auto next = download_state;
                for (auto& record : next["records"]) if (record.at("path") == archive.u8string()) record["extracted"] = folder.u8string();
                commit_download_state(std::move(next));
            }
            status("Extracted " + std::to_string(outcome.files) + " files to " + folder_name + (deleted ? "; archive deleted" : ""));
            if (outcome.unsafe || outcome.links)
                problem = "Extracted " + file.name + ", but skipped " + std::to_string(outcome.unsafe) + " unsafe names and " +
                    std::to_string(outcome.links) + " links or special files.";
        }
        if (!problem.empty()) {
            std::lock_guard<std::mutex> guard(extraction_mutex);
            extraction_error = problem;
        }
    }

    // Only a path the library reports for this file: a recorded copy, or an unrecorded copy in a download location.
    // An archive's extracted folder is deleted with it, also when the archive itself was already deleted.
    // Returns the bytes freed, extracted folders included.
    std::uint64_t delete_download(const File& file, const std::string& requested) {
        if (!named_locations_supported) throw std::runtime_error("Deleting downloads is not supported on this platform");
        std::lock_guard<std::mutex> guard(download_state_mutex);
        if (platform_storage) {
            auto next = download_state;
            auto& records = next["records"];
            auto found = std::find_if(records.begin(), records.end(), [&](const Json& record) {
                return record.at("item") == file.item && record.at("name") == file.name && record.at("path") == requested && record.contains("storage");
            });
            if (found == records.end()) throw std::runtime_error("That copy is not owned by ArchiveFetch");
            if (!download_state_writable) throw std::runtime_error("Repair saved download records before deleting");
            const auto status = platform_storage({{"action", "status"}, {"record", *found}}).at("status").get<std::string>();
            const auto freed = status == "Missing" || status == "Location unavailable" ? std::uint64_t{0}
                : platform_storage({{"action", "deleteCopy"}, {"record", *found}}).at("bytes").get<std::uint64_t>();
            records.erase(found);
            commit_download_state(std::move(next));
            return freed;
        }
        bool recorded = false;
        std::string recorded_folder;
        for (const auto& record : download_state.at("records"))
            if (record.at("item") == file.item && record.at("name") == file.name && record.at("path") == requested) { recorded = true; recorded_folder = record.value("extracted", ""); }
        const auto path = std::filesystem::u8path(requested);
        bool in_location = false;
        if (!recorded && path.filename() == std::filesystem::u8path(file.name).filename()) {
            const auto parent = path.parent_path().lexically_normal();
            in_location = parent == std::filesystem::absolute(config.download_dir).lexically_normal();
            for (const auto& location : download_state.at("locations"))
                in_location = in_location || parent == std::filesystem::absolute(std::filesystem::u8path(location.at("path").get<std::string>())).lexically_normal();
        }
        if (!recorded && !in_location) throw std::runtime_error("That path is not a download of this file");
        if (recorded && !download_state_writable) throw std::runtime_error("Saved download locations are invalid; original file was left unchanged");
        const auto folder = !recorded_folder.empty() ? std::filesystem::u8path(recorded_folder) : extract ? extracted_folder(path, file.name) : std::filesystem::path{};
        // Only a folder beside the archive, never one a damaged record points elsewhere.
        const bool folder_present = real_directory(folder) && folder.parent_path().lexically_normal() == path.parent_path().lexically_normal();
        std::error_code failure;
        const auto disk = std::filesystem::symlink_status(path, failure);
        const bool gone = failure || !std::filesystem::exists(disk);
        if (!gone && !std::filesystem::is_regular_file(disk)) throw std::runtime_error("That path is no longer a regular file; it was left unchanged");
        if (gone && !recorded && !folder_present) throw std::runtime_error("The downloaded file is no longer there");
        std::uintmax_t freed = 0;
        if (!gone) {
            failure.clear();
            const auto size = std::filesystem::file_size(path, failure);
            std::filesystem::remove(path);
            if (!failure) freed += size;
        }
        if (folder_present) {
            failure.clear();
            if (!remove_tree(folder, failure, &freed)) throw std::runtime_error("Could not delete the extracted folder " + folder.filename().u8string() + ": " + failure.message());
        }
        if (!recorded) return freed;
        auto next = download_state;
        auto& records = next["records"];
        records.erase(std::remove_if(records.begin(), records.end(), [&](const Json& record) { return record.at("path") == requested; }), records.end());
        commit_download_state(std::move(next));
        return freed;
    }

    // Unavailable locations (such as an unplugged drive) are only removed when asked for.
    std::size_t clean_downloads(bool include_unavailable) {
        if (!named_locations_supported) throw std::runtime_error("Cleaning downloads is not supported on this platform");
        std::lock_guard<std::mutex> guard(download_state_mutex);
        auto next = download_state;
        auto& records = next["records"];
        const auto before = records.size();
        records.erase(std::remove_if(records.begin(), records.end(), [this, include_unavailable](const Json& record) {
            if (platform_storage && record.contains("storage")) {
                const auto status = platform_storage({{"action", "status"}, {"record", record}}).at("status").get<std::string>();
                return status == "Missing" || (include_unavailable && status == "Location unavailable");
            }
            const auto path = std::filesystem::u8path(record.at("path").get<std::string>());
            std::error_code failure;
            const auto disk = std::filesystem::symlink_status(path, failure);
            if (!failure && std::filesystem::exists(disk)) return false;
            failure.clear();
            return include_unavailable || (std::filesystem::is_directory(path.parent_path(), failure) && !failure);
        }), records.end());
        const auto removed = before - records.size();
        if (removed) commit_download_state(std::move(next));
        return removed;
    }

    // A file is "owned" while any copy is downloaded, changed or on an unavailable location; owned and queued
    // files are never removed except by "all"/"delete". "delete" and "deleteDownloads" only delete copies verified as
    // downloaded or extracted (with their extracted folders); "deleteDownloads" keeps the library entries.
    Json clean_library(const Json& value) {
        using Key = std::pair<std::string, std::string>;
        const auto state = model.snapshot();
        if (state.busy) throw std::runtime_error("Stop the current operation before cleaning the library");
        std::vector<std::size_t> scope;
        if (value.contains("indices")) {
            if (value.at("revision").get<std::uint64_t>() != state.catalog_revision) throw std::runtime_error("Library changed; refresh the selection");
            for (const auto index : value.at("indices").get<std::vector<std::size_t>>()) {
                if (index >= state.catalog.files.size()) throw std::runtime_error("Invalid library selection");
                scope.push_back(index);
            }
        } else for (std::size_t index = 0; index < state.catalog.files.size(); ++index) scope.push_back(index);
        // Hidden file types are listed too, so the entries line up with catalog indices.
        const auto listing = library(true);
        if (listing.at("revision") != state.catalog_revision) throw std::runtime_error("Library changed; try again");
        const auto& entries = listing.at("files");
        std::set<std::string> sources;
        for (const auto& source : state.sources) sources.insert(item_id(source.url));
        std::set<Key> queued;
        for (const auto& entry : state.queue) queued.emplace(entry.file.item, entry.file.name);
        std::vector<std::filesystem::path> folders{config.download_dir};
        {
            std::lock_guard<std::mutex> guard(download_state_mutex);
            for (const auto& location : download_state.at("locations")) folders.push_back(std::filesystem::u8path(location.at("path").get<std::string>()));
        }
        std::set<Key> orphans, undownloaded, all;
        std::map<Key, std::vector<std::string>> stale;
        std::vector<std::string> kept, deleting;
        std::vector<std::pair<std::size_t, std::string>> removable;
        std::uint64_t present_bytes = 0;
        std::size_t queued_count = 0, extracted_folders = 0;
        for (const auto index : scope) {
            const auto& file = state.catalog.files[index];
            const auto& entry = entries.at(index);
            const Key key{file.item, file.name};
            if (queued.count(key)) { ++queued_count; continue; }
            all.insert(key);
            bool owned = entry.at("downloaded").get<bool>();
            for (const auto& copy : entry.at("downloadCopies")) {
                const auto status = copy.at("status").get<std::string>();
                if (status == "Missing") { stale[key].push_back(copy.at("path")); continue; }
                owned = true;
                if (status == "Downloaded") present_bytes += file.size;
                if (status == "Downloaded" || status == "Extracted") {
                    removable.emplace_back(index, copy.at("path").get<std::string>());
                    if (status == "Downloaded") deleting.push_back(copy.at("path"));
                    if (copy.contains("extracted") && (platform_storage || real_directory(std::filesystem::u8path(copy.at("extracted").get<std::string>())))) {
                        ++extracted_folders; deleting.push_back(copy.at("extracted"));
                        // Only counted for the preview's size; walking every folder for the other choices is not needed.
                        if (value.value("preview", false)) present_bytes += platform_storage ? copy.value("extractedBytes", std::uint64_t{0}) : tree_size(std::filesystem::u8path(copy.at("extracted").get<std::string>()));
                    }
                }
                else kept.push_back(copy.at("path"));
            }
            if (entry.at("downloadCopies").empty() && owned) {
                present_bytes += file.size;
                removable.emplace_back(index, entry.at("downloadPath").get<std::string>());
                deleting.push_back(entry.at("downloadPath"));
            }
            if (owned) continue;
            if (!sources.count(file.item)) orphans.insert(key);
            bool partial = false;
            for (const auto& folder : folders) {
                if (platform_storage) continue;
                auto part = folder / std::filesystem::u8path(file.name).filename();
                part += ".part";
                std::error_code failure;
                partial = partial || std::filesystem::is_regular_file(std::filesystem::symlink_status(part, failure));
            }
            if (!partial) undownloaded.insert(key);
        }
        if (value.value("preview", false)) {
            Json shown = Json::array(), paths = Json::array();
            for (std::size_t index = 0; index < kept.size() && index < 50; ++index) shown.push_back(kept[index]);
            for (std::size_t index = 0; index < deleting.size() && index < 50; ++index) paths.push_back(deleting[index]);
            return {{"total", scope.size()}, {"queued", queued_count}, {"orphans", orphans.size()}, {"undownloaded", undownloaded.size()},
                {"all", all.size()}, {"deleteDownloads", removable.size()}, {"downloading", state.downloading},
                {"delete", {{"supported", named_locations_supported}, {"files", removable.size()}, {"folders", extracted_folders},
                {"bytes", present_bytes}, {"paths", paths}, {"pathCount", deleting.size()}, {"kept", shown}, {"keptCount", kept.size()}}}};
        }
        // "delete" and "deleteDownloads" are kept for older pages; newer ones send a mode plus "deleteDownloads".
        const auto mode = value.at("mode").get<std::string>();
        const std::set<Key> none;
        const std::set<Key>* chosen = mode == "orphans" ? &orphans : mode == "undownloaded" ? &undownloaded : mode == "all" || mode == "delete" ? &all
            : mode == "deleteDownloads" || mode == "none" ? &none : nullptr;
        if (!chosen) throw std::runtime_error("Choose how to clean the library");
        const bool delete_files = mode == "delete" || mode == "deleteDownloads" || value.value("deleteDownloads", false);
        std::set<std::string> forget;
        std::size_t deleted = 0, failed = 0;
        std::uint64_t freed = 0;
        std::string problem;
        if (delete_files) {
            if (!named_locations_supported) throw std::runtime_error("Deleting downloads is not supported on this platform");
            if (state.downloading) throw std::runtime_error("Wait for downloads to finish before deleting files");
            // The page asks a second time before sending this; it guards against requests that skip that question.
            if (value.value("confirm", "") != "DELETE") throw std::runtime_error("Confirm deleting the downloads first");
            for (const auto& [index, text] : removable) {
                try { freed += delete_download(state.catalog.files[index], text); ++deleted; }
                catch (const std::exception& error) { ++failed; if (problem.empty()) problem = error.what(); }
            }
            // Entries are only removed when every download was deleted, so nothing is left on disk without its entry.
            if (failed && chosen == &all)
                throw std::runtime_error("Deleted " + std::to_string(deleted) + " downloads, but " + std::to_string(failed) + " could not be deleted (" + problem + "); the Library was left unchanged");
        }
        // Records of copies that are no longer on disk go with their entries, or with the downloads being deleted.
        for (const auto& key : delete_files ? all : *chosen)
            if (mode != "all") if (const auto found = stale.find(key); found != stale.end()) forget.insert(found->second.begin(), found->second.end());
        if (!forget.empty() && download_state_writable) {
            std::lock_guard<std::mutex> guard(download_state_mutex);
            auto next = download_state;
            auto& records = next["records"];
            records.erase(std::remove_if(records.begin(), records.end(), [&](const Json& record) { return forget.count(record.at("path").get<std::string>()) != 0; }), records.end());
            commit_download_state(std::move(next));
        }
        const auto removed = model.remove_files(*chosen);
        Json result = {{"ok", true}, {"removed", removed}, {"deleted", deleted}, {"bytes", freed}};
        if (failed) { result["failed"] = failed; result["problem"] = problem; }
        return result;
    }

    void start_downloads(std::uint64_t retry_id = 0) {
        if (!download_state_writable) throw std::runtime_error("Repair saved download locations before downloading");
        const auto transfer = download;
        model.start_downloads(config, [this, transfer, ca = config.ca_bundle, keys = config.s3, connections = config.download_connections](const File& file, const std::filesystem::path& directory, bool overwrite, const TransferHooks& hooks) {
            if (transfer) transfer(file, directory, overwrite, hooks);
            else { Http http(ca, hooks, keys, connections); http.download(file, directory, overwrite); }
            if (platform_record_download) platform_record_download(file, directory, hooks);
            else record_download(file, directory);
            extract_after_download(file, directory, hooks);
        }, retry_id, platform_partial_path);
    }

        void save_file_tags(const File& file, const std::vector<std::string>& tags) {
        if (!tags_writable) throw std::runtime_error("Saved file tags are invalid; original file was left unchanged");
        auto updated = file_tags;
        const auto key = std::make_pair(file.item, file.name);
        auto normalized = normalize_tags(tags);
        if (normalized.empty()) updated.erase(key); else updated[key] = std::move(normalized);
        write_file_tags(std::move(updated));
        }

    // Appends tags not already present, ignoring ASCII case.
    static void merge_tags(std::vector<std::string>& current, const std::vector<std::string>& added, const std::string& owner) {
        const auto same = [](const std::string& left, const std::string& right) {
            return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](unsigned char a, unsigned char b) {
                return (a >= 'A' && a <= 'Z' ? a + 32 : a) == (b >= 'A' && b <= 'Z' ? b + 32 : b);
            });
        };
        for (const auto& tag : added)
            if (std::none_of(current.begin(), current.end(), [&](const std::string& existing) { return same(existing, tag); })) current.push_back(tag);
        if (current.size() > 32) throw std::runtime_error("More than 32 tags would be on " + owner);
    }

    // Adds to each file's existing tags; one write, so a failure leaves every file unchanged.
    void add_file_tags(const std::vector<File>& files, const std::vector<std::string>& tags) {
        if (!tags_writable) throw std::runtime_error("Saved file tags are invalid; original file was left unchanged");
        const auto added = normalize_tags(tags);
        if (added.empty()) throw std::runtime_error("Enter at least one tag");
        auto updated = file_tags;
        for (const auto& file : files) merge_tags(updated[std::make_pair(file.item, file.name)], added, file.name);
        write_file_tags(std::move(updated));
    }

    // Every source is checked before any is changed.
    void add_source_tags(const std::vector<std::string>& urls, const std::vector<std::string>& tags) {
        const auto added = normalize_tags(tags);
        if (added.empty()) throw std::runtime_error("Enter at least one tag");
        if (urls.empty()) throw std::runtime_error("Select sources to tag");
        const auto state = model.snapshot();
        std::vector<std::pair<std::string, std::vector<std::string>>> updates;
        for (const auto& url : urls) {
            const auto identifier = item_id(url);
            const auto source = std::find_if(state.sources.begin(), state.sources.end(), [&](const SourceEntry& entry) { return item_id(entry.url) == identifier; });
            if (source == state.sources.end()) throw std::runtime_error("Source no longer exists: " + url);
            auto merged = source->tags;
            merge_tags(merged, added, source->name.empty() ? identifier : source->name);
            updates.emplace_back(source->url, std::move(merged));
        }
        for (const auto& update : updates) model.set_source_tags(update.first, update.second);
    }

    static std::string folded(std::string text) {
        for (auto& character : text) if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
        return text;
    }

    // Normalizes names and tags in place; a source may belong to only one collection.
    static void validate_collections(std::vector<SourceCollection>& values) {
        if (values.size() > 128) throw std::runtime_error("At most 128 collections are supported");
        std::set<std::string> ids, names, items;
        for (auto& collection : values) {
            const auto labels = normalize_tags({collection.name});
            if (labels.size() != 1) throw std::runtime_error("Name the collection");
            collection.name = labels.front();
            collection.tags = normalize_tags(collection.tags);
            if (collection.tags.size() > 32) throw std::runtime_error("More than 32 tags would be on collection " + collection.name);
            if (collection.id.empty() || collection.id.size() > 16 || !ids.insert(collection.id).second) throw std::runtime_error("Invalid saved collection");
            if (!names.insert(folded(collection.name)).second) throw std::runtime_error("A collection named " + collection.name + " already exists");
            for (const auto& item : collection.items)
                if (item.empty() || !items.insert(item).second) throw std::runtime_error("A source can only be in one collection");
        }
    }

    void write_collections(std::vector<SourceCollection> updated) {
        if (!collections_writable) throw std::runtime_error("Saved collections are invalid; original file was left unchanged");
        validate_collections(updated);
        Json entries = Json::array();
        for (const auto& collection : updated)
            entries.push_back({{"id", collection.id}, {"name", collection.name}, {"tags", collection.tags}, {"sources", collection.items}});
        const auto serialized = Json({{"version", 1}, {"collections", entries}}).dump();
        if (serialized.size() > 1024 * 1024) throw std::runtime_error("Saved collections exceed 1 MiB");
        const auto path = root / "collections.json";
        auto temporary = path; temporary += ".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(serialized.data(), static_cast<std::streamsize>(serialized.size())); output.close();
        if (!output) throw std::runtime_error("Cannot save collections");
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot replace saved collections");
#else
        std::filesystem::rename(temporary, path);
#endif
        collections = std::move(updated); ++tag_revision;
    }

    const SourceCollection* collection_of(const std::string& item) const {
        for (const auto& collection : collections)
            if (std::find(collection.items.begin(), collection.items.end(), item) != collection.items.end()) return &collection;
        return nullptr;
    }

    void reject_collection_members(const std::vector<std::string>& urls) const {
        for (const auto& url : urls)
            if (const auto* collection = collection_of(item_id(url)))
                throw std::runtime_error(item_id(url) + " is in collection " + collection->name + "; edit the collection's tags instead");
    }

    // Tags offered by a join or leave prompt: "add"/"copy" keeps all, "discard"/"remove" none, "choose" the listed ones.
    static std::vector<std::string> chosen_tags(const Json& value, const std::vector<std::string>& offered) {
        const auto mode = value.at("tags").get<std::string>();
        if (mode == "add" || mode == "copy") return offered;
        if (mode == "discard" || mode == "remove") return {};
        if (mode != "choose") throw std::runtime_error("Choose what happens to the tags");
        std::set<std::string> keep;
        for (const auto& tag : value.at("keep").get<std::vector<std::string>>()) keep.insert(folded(tag));
        std::vector<std::string> result;
        for (const auto& tag : offered) if (keep.count(folded(tag))) result.push_back(tag);
        return result;
    }

    std::map<std::string, std::string> source_urls(const GuiSnapshot& state, const std::vector<std::string>& urls) const {
        std::map<std::string, std::string> result;
        for (const auto& url : urls) {
            const auto item = item_id(url);
            const auto source = std::find_if(state.sources.begin(), state.sources.end(), [&](const SourceEntry& entry) { return item_id(entry.url) == item; });
            if (source == state.sources.end()) throw std::runtime_error("Source no longer exists: " + url);
            result[item] = source->url;
        }
        if (result.empty()) throw std::runtime_error("Select sources first");
        return result;
    }

    // Members always carry their collection's tags, so routing, filters and older versions see them as source tags.
    void apply_collection_tags(const GuiSnapshot& state, const std::map<std::string, std::vector<std::string>>& leaving = {}) {
        for (const auto& source : state.sources) {
            const auto item = item_id(source.url);
            if (const auto* collection = collection_of(item)) {
                if (source.tags != collection->tags) model.set_source_tags(source.url, collection->tags);
            } else if (const auto left = leaving.find(item); left != leaving.end()) model.set_source_tags(source.url, left->second);
        }
    }

    Json collection_add(const Json& value) {
        const auto state = model.snapshot();
        const auto selected = source_urls(state, value.at("urls").get<std::vector<std::string>>());
        auto next = collections;
        const auto id = value.value("id", "");
        std::size_t target = next.size();
        for (std::size_t index = 0; index < next.size(); ++index) if (next[index].id == id) target = index;
        if (id.empty()) next.push_back({next_collection_id(next), value.at("name").get<std::string>(), {}, {}});
        else if (target == next.size()) throw std::runtime_error("That collection no longer exists");
        auto& collection = next[target];
        std::vector<std::string> offered;
        for (const auto& source : state.sources) {
            const auto item = item_id(source.url);
            if (!selected.count(item) || std::find(collection.items.begin(), collection.items.end(), item) != collection.items.end()) continue;
            for (const auto& tag : source.tags)
                if (std::none_of(offered.begin(), offered.end(), [&](const std::string& existing) { return folded(existing) == folded(tag); })) offered.push_back(tag);
        }
        merge_tags(collection.tags, chosen_tags(value, offered), "collection " + collection.name);
        for (auto& other : next)
            other.items.erase(std::remove_if(other.items.begin(), other.items.end(), [&](const std::string& item) { return selected.count(item) != 0; }), other.items.end());
        for (const auto& entry : selected) collection.items.push_back(entry.first);
        const auto result_id = collection.id;
        write_collections(std::move(next));
        apply_collection_tags(state);
        return {{"ok", true}, {"id", result_id}};
    }

    Json collection_edit(const Json& value) {
        const auto state = model.snapshot();
        auto next = collections;
        const auto found = std::find_if(next.begin(), next.end(), [&](const SourceCollection& entry) { return entry.id == value.at("id"); });
        if (found == next.end()) throw std::runtime_error("That collection no longer exists");
        found->name = value.at("name").get<std::string>();
        found->tags = value.at("tags").get<std::vector<std::string>>();
        write_collections(std::move(next));
        apply_collection_tags(state);
        return {{"ok", true}};
    }

    Json collection_leave(const Json& value, bool whole) {
        const auto state = model.snapshot();
        auto next = collections;
        std::map<std::string, std::vector<std::string>> leaving;
        if (whole) {
            const auto found = std::find_if(next.begin(), next.end(), [&](const SourceCollection& entry) { return entry.id == value.at("id"); });
            if (found == next.end()) throw std::runtime_error("That collection no longer exists");
            const auto kept = chosen_tags(value, found->tags);
            for (const auto& item : found->items) leaving[item] = kept;
            next.erase(found);
        } else {
            for (const auto& entry : source_urls(state, value.at("urls").get<std::vector<std::string>>())) {
                const auto* collection = collection_of(entry.first);
                if (!collection) throw std::runtime_error(entry.first + " is not in a collection");
                leaving[entry.first] = chosen_tags(value, collection->tags);
            }
            for (auto& collection : next)
                collection.items.erase(std::remove_if(collection.items.begin(), collection.items.end(), [&](const std::string& item) { return leaving.count(item) != 0; }), collection.items.end());
        }
        write_collections(std::move(next));
        apply_collection_tags(state, leaving);
        return {{"ok", true}};
    }

    static std::string next_collection_id(const std::vector<SourceCollection>& values) {
        std::size_t number = 1;
        for (const auto& collection : values) if (collection.id.size() > 1) number = std::max<std::size_t>(number, std::strtoull(collection.id.c_str() + 1, nullptr, 10) + 1);
        return "c" + std::to_string(number);
    }

    static Json file_entry(const File& file) {
        return {{"item", file.item}, {"name", file.name}, {"size", file.size}, {"md5", file.md5}, {"sha1", file.sha1}, {"restricted", file.restricted}};
    }

    // Incoming files pass the same checks as a saved catalog by being written and read back.
    std::vector<File> merge_files_from(const Json& entries) {
        Scan scan;
        for (const auto& entry : entries)
            scan.files.push_back({entry.at("item").get<std::string>(), entry.at("name").get<std::string>(), entry.at("size").get<std::uint64_t>(),
                entry.value("md5", ""), entry.value("sha1", ""), entry.value("restricted", false)});
        if (scan.files.empty()) return {};
        const auto path = root / "merge-incoming.xml";
        save_catalog(scan, path);
        std::vector<File> files;
        try { files = read_catalog(path).files; } catch (...) { std::error_code ignored; std::filesystem::remove(path, ignored); throw; }
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return files;
    }

    // What another device may add from here: never settings, keys, locations, the queue or download status.
    Json merge_export(const Json& value) {
        persist();
        const auto state = model.snapshot();
        const bool selected_only = value.value("selected", false);
        std::set<std::string> items, collection_names;
        Json sources = Json::array(), groups = Json::array(), files = Json::array(), tags = Json::array(), favorite_files = Json::array();
        for (const auto& source : state.sources) {
            if (selected_only && !source.selected) continue;
            const auto item = item_id(source.url);
            items.insert(item);
            const auto* collection = collection_of(item);
            sources.push_back({{"url", source.url}, {"item", item}, {"name", source.name}, {"tags", source.tags}, {"collection", collection ? collection->name : ""}});
            if (collection && collection_names.insert(collection->name).second) groups.push_back({{"name", collection->name}, {"tags", collection->tags}});
        }
        const auto included = [&](const std::string& item) { return !selected_only || items.count(item) != 0; };
        for (const auto& file : state.catalog.files) if (included(file.item)) files.push_back(file_entry(file));
        for (const auto& entry : file_tags) if (included(entry.first.first)) tags.push_back({{"item", entry.first.first}, {"name", entry.first.second}, {"tags", entry.second}});
        for (const auto& entry : favorites) if (included(entry.first.first)) favorite_files.push_back(file_entry(entry.second));
        return {{"version", ARCHIVEFETCH_VERSION}, {"sources", sources}, {"collections", groups}, {"files", files}, {"fileTags", tags}, {"favorites", favorite_files}};
    }

    // Add-only: new sources (unscanned), their collections, new files, tag unions and favorites. Nothing is removed or replaced.
    Json merge_import(const Json& value) {
        std::size_t added_sources = 0, added_files = 0, tagged = 0, added_favorites = 0;
        if (value.contains("sources")) {
            auto current = model.snapshot().sources;
            std::set<std::string> present;
            for (const auto& source : current) present.insert(item_id(source.url));
            std::vector<std::pair<std::string, std::string>> joining;
            for (const auto& source : value.at("sources")) {
                const auto item = item_id(source.at("url").get<std::string>());
                if (!present.insert(item).second) continue;
                if (current.size() >= 128) throw std::runtime_error("Source list exceeds the 128-item limit");
                SourceEntry entry;
                entry.url = "https://archive.org/details/" + item;
                entry.name = source.value("name", item).substr(0, 4096);
                entry.tags = source.value("tags", std::vector<std::string>{});
                current.push_back(std::move(entry));
                ++added_sources;
                const auto collection = source.value("collection", "");
                if (!collection.empty()) joining.emplace_back(item, collection);
            }
            if (added_sources) model.restore_sources(std::move(current));
            if (!joining.empty()) {
                auto next = collections;
                for (const auto& join : joining) {
                    auto found = std::find_if(next.begin(), next.end(), [&](const SourceCollection& entry) { return folded(entry.name) == folded(join.second); });
                    if (found == next.end()) {
                        std::vector<std::string> group_tags;
                        for (const auto& group : value.value("collections", Json::array()))
                            if (folded(group.at("name").get<std::string>()) == folded(join.second)) group_tags = group.at("tags").get<std::vector<std::string>>();
                        next.push_back({next_collection_id(next), join.second, group_tags, {}});
                        found = next.end() - 1;
                    }
                    found->items.push_back(join.first);
                }
                write_collections(std::move(next));
                apply_collection_tags(model.snapshot());
            }
        }
        if (value.contains("sourceTags")) {
            const auto state = model.snapshot();
            for (const auto& entry : value.at("sourceTags")) {
                const auto item = item_id(entry.at("url").get<std::string>());
                const auto source = std::find_if(state.sources.begin(), state.sources.end(), [&](const SourceEntry& candidate) { return item_id(candidate.url) == item; });
                // Collection members carry the collection's tags; those are not changed one source at a time.
                if (source == state.sources.end() || collection_of(item)) continue;
                auto merged = source->tags;
                merge_tags(merged, normalize_tags(entry.at("tags").get<std::vector<std::string>>()), source->name);
                if (merged != source->tags) { model.set_source_tags(source->url, merged); ++tagged; }
            }
        }
        if (value.contains("files")) added_files = model.add_files(merge_files_from(value.at("files")));
        if (value.contains("fileTags")) {
            if (!tags_writable) throw std::runtime_error("Saved file tags are invalid; original file was left unchanged");
            auto updated = file_tags;
            for (const auto& entry : value.at("fileTags")) {
                auto& current = updated[std::make_pair(entry.at("item").get<std::string>(), entry.at("name").get<std::string>())];
                const auto before = current.size();
                merge_tags(current, normalize_tags(entry.at("tags").get<std::vector<std::string>>()), entry.at("name").get<std::string>());
                if (current.size() != before) ++tagged;
            }
            if (updated != file_tags) write_file_tags(std::move(updated));
        }
        if (value.contains("favorites")) {
            if (!favorites_writable) throw std::runtime_error("Saved favorites are invalid; original file was left unchanged");
            auto updated = favorites;
            for (const auto& file : merge_files_from(value.at("favorites")))
                if (updated.emplace(std::make_pair(file.item, file.name), file).second) ++added_favorites;
            if (added_favorites) {
                Scan saved;
                for (const auto& entry : updated) saved.files.push_back(entry.second);
                save_catalog(saved, root / "favorites.xml");
                favorites = std::move(updated);
            }
        }
        persist();
        return {{"ok", true}, {"sources", added_sources}, {"files", added_files}, {"tags", tagged}, {"favorites", added_favorites}};
    }

    // Forgets removed sources and repairs member tags edited outside a collection (for example by an older version).
    void sync_collections() {
        const auto state = model.snapshot();
        if (state.scanning || collections.empty() || !collections_writable) return;
        std::set<std::string> present;
        for (const auto& source : state.sources) present.insert(item_id(source.url));
        auto next = collections;
        bool pruned = false;
        for (auto& collection : next) {
            const auto before = collection.items.size();
            collection.items.erase(std::remove_if(collection.items.begin(), collection.items.end(), [&](const std::string& item) { return !present.count(item); }), collection.items.end());
            pruned = pruned || before != collection.items.size();
        }
        try {
            if (pruned) write_collections(std::move(next));
            apply_collection_tags(state);
        } catch (const std::exception& failure) { error = failure.what(); }
    }

    void write_file_tags(std::map<std::pair<std::string, std::string>, std::vector<std::string>> updated) {
        Json entries = Json::array();
        for (const auto& entry : updated) entries.push_back({{"item", entry.first.first}, {"name", entry.first.second}, {"tags", entry.second}});
        const auto serialized = entries.dump();
        if (serialized.size() > 16 * 1024 * 1024) throw std::runtime_error("Saved file tags exceed 16 MiB");
        const auto path = root / "file-tags.json";
        auto temporary = path; temporary += ".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(serialized.data(), static_cast<std::streamsize>(serialized.size())); output.close();
        if (!output) throw std::runtime_error("Cannot save file tags");
    #ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot replace saved file tags");
    #else
        std::filesystem::rename(temporary, path);
    #endif
        file_tags = std::move(updated); ++tag_revision;
        }

    void save_settings(const Json& value) {
        if (model.snapshot().busy) throw std::runtime_error("Stop the current operation before changing settings");
        auto next = config;
        next.download_dir = std::filesystem::u8path(value.at("destination").get<std::string>());
        Json next_locations;
        {
            std::lock_guard<std::mutex> guard(download_state_mutex);
            if (!download_state_writable) throw std::runtime_error("Saved download locations are invalid; original file was left unchanged");
            next_locations = download_state;
            next_locations["preference"] = value.value("locationPreference", next_locations.at("preference").get<std::string>());
            validate_location_rules(next_locations);
            if (value.contains("defaultLocation")) {
                const auto id = value.at("defaultLocation").get<std::string>();
                next.download_dir = std::filesystem::u8path(find_location(next_locations, id).at("path").get<std::string>());
                next_locations["default"] = id;
            } else {
                if (next.download_dir.empty()) throw std::runtime_error("Choose a download directory");
                next.download_dir = platform_storage ? std::filesystem::u8path(location_path(next.download_dir.u8string())) : std::filesystem::absolute(next.download_dir).lexically_normal();
                if (platform_storage) platform_storage({{"action", "validate"}, {"tree", next.download_dir.u8string()}});
                next_locations["default"] = "default";
                for (auto& location : next_locations["locations"]) if (location.at("id") == "default") location["path"] = next.download_dir.u8string();
            }
        }
        next.output_dir = std::filesystem::u8path(value.at("output").get<std::string>());
        next.ca_bundle = value.value("ca", config.ca_bundle);
        next.automatically_start_downloads = value.at("automatic").get<bool>();
        next.extract_archives = value.value("extractArchives", config.extract_archives);
        next.delete_extracted_archives = value.value("deleteExtracted", config.delete_extracted_archives);
        if (value.contains("connections")) {
            const auto& connections = value.at("connections");
            if (!connections.is_number_integer() || connections.get<int>() < 1 || connections.get<int>() > max_download_connections)
                throw std::runtime_error("Choose 1 to " + std::to_string(max_download_connections) + " connections per download");
            next.download_connections = connections.get<int>();
        }
        if (value.contains("excludedExtensions")) next.excluded_extensions = parse_extension_list(value.at("excludedExtensions").get<std::string>());
        if (next.download_dir.empty() || next.output_dir.empty()) throw std::runtime_error("Choose download and export directories");
        auto access = value.value("access", "");
        auto secret = value.value("secret", "");
        if (!access.empty() || !secret.empty()) {
            if (access.empty() || secret.empty()) throw std::runtime_error("Supply both S3 keys");
            next.s3 = {access, secret};
        }
        auto keep = value.value("remember", false);
        if (keep && (!credentials || next.s3.access_key.empty() || next.s3.secret_key.empty())) throw std::runtime_error("Supply both keys before remembering them");
        write_settings_file(next);
        if (credentials) {
            if (keep) {
                if (next.s3.access_key.empty() || next.s3.secret_key.empty()) throw std::runtime_error("Supply keys before remembering them");
                credentials->save(next.s3);
            } else credentials->forget();
        }
        {
            std::lock_guard<std::mutex> guard(download_state_mutex);
            commit_download_state(std::move(next_locations));
        }
        config = std::move(next); remember = keep;
    }

    // Keys are never written here; they live in the credential store.
    void write_settings_file(const Config& next) {
        auto* document = xmlNewDoc(BAD_CAST "1.0");
        if (!document) throw std::runtime_error("Cannot save settings");
        std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> owned(document, xmlFreeDoc);
        auto* node = xmlNewNode(nullptr, BAD_CAST "archive_catalog");
        xmlDocSetRootElement(document, node);
        auto property = [node](const char* name, const std::string& text) { xmlNewProp(node, BAD_CAST name, BAD_CAST text.c_str()); };
        property("download_dir", next.download_dir.u8string()); property("output_dir", next.output_dir.u8string());
        property("ca_bundle", next.ca_bundle); property("automatically_start_downloads", next.automatically_start_downloads ? "true" : "false");
        property("extract_archives", next.extract_archives ? "true" : "false");
        property("delete_extracted_archives", next.delete_extracted_archives ? "true" : "false");
        property("download_connections", std::to_string(next.download_connections));
        property("excluded_extensions", format_extension_list(next.excluded_extensions));
        property("follow_parts", next.follow_parts ? "true" : "false"); property("debug_window", next.debug_window ? "true" : "false");
        for (const auto& source : model.snapshot().sources) {
            auto* child = xmlNewChild(node, nullptr, BAD_CAST "source", nullptr);
            xmlNewProp(child, BAD_CAST "url", BAD_CAST source.url.c_str());
        }
        auto temporary = settings; temporary += ".tmp";
        if (xmlSaveFormatFileEnc(temporary.u8string().c_str(), document, "UTF-8", 1) < 0) throw std::runtime_error("Cannot write settings");
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), settings.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot replace settings");
#else
        std::filesystem::rename(temporary, settings);
#endif
    }

    // Only the hidden-type list changes, so this is safe while downloads and scans run.
    Json hide_extension(const std::string& text, bool hide) {
        const auto parsed = parse_extension_list(text);
        if (parsed.size() != 1) throw std::runtime_error("Choose one file type");
        auto next = config;
        auto& list = next.excluded_extensions;
        const auto found = std::find(list.begin(), list.end(), parsed.front());
        if (hide && found == list.end()) list.push_back(parsed.front());
        if (!hide && found != list.end()) list.erase(found);
        if (list.size() > 64) throw std::runtime_error("Hide at most 64 file types");
        write_settings_file(next);
        config.excluded_extensions = list;
        return {{"ok", true}, {"extension", parsed.front()}, {"excludedExtensions", format_extension_list(list)}};
    }

    Json command(const Json& value) {
        auto action = value.at("action").get<std::string>();
        if (action == "checkUpdates") {
            auto result = updates.check(ARCHIVEFETCH_VERSION, config.ca_bundle, value.value("includeBeta", false));
#if defined(__ANDROID__)
            result["platform"] = "android";
#elif defined(_WIN32)
            result["platform"] = "windows";
#elif defined(PS5_PAYLOAD)
            result["platform"] = "ps5";
#else
            result["platform"] = "other";
#endif
            result["updatePlatform"] = update_platform;
            return result;
        }
        if (action == "updateStatus") return release_update.status();
        if (action == "cancelUpdate") { release_update.cancel(); return {{"ok", true}}; }
        if (action == "prepareUpdate") {
            if (model.snapshot().busy) throw std::runtime_error("Finish or stop current operations before downloading an app update");
            release_update.start(updates.install_release(update_platform, value.at("version")), config.ca_bundle);
            return release_update.status();
        }
        if (action == "installUpdate") {
            if (model.snapshot().busy) throw std::runtime_error("Finish or stop current operations before installing an app update");
            return release_update.install(value.at("version"));
        }
        if (action == "updateInstallStarted") {
            release_update.install_started(value.at("version"));
            auto result = release_update.status();
            result["ok"] = true;
            return result;
        }
        if (action == "retryUpdateCompletion") {
            if (model.snapshot().busy) throw std::runtime_error("Finish current operations before retrying update completion");
            if (!retry_update_completion) throw std::runtime_error("Update completion retry is not supported on this platform");
            retry_update_completion();
            return release_update.status();
        }
        if (action == "openUpdate") {
            const auto url = updates.link(value.at("kind").get<std::string>(), value.at("version").get<std::string>());
            if (open_update) { open_update(url); return {{"ok", true}}; }
            return {{"ok", true}, {"openUpdate", url}};
        }
        if (action == "openS3Keys") {
    #ifdef __ANDROID__
            return {{"ok", true}, {"openS3Keys", true}};
    #else
            if (!open_s3_keys) throw std::runtime_error("Opening the browser is not supported on this platform");
            open_s3_keys();
            return {{"ok", true}};
    #endif
        }
        auto state = model.snapshot();
        // Source changes only wait for a scan, so they also work while downloads run.
        static const std::set<std::string> source_actions{"addSource", "selectSource", "selectSources", "sourceTags", "addSourceTags",
            "collectionAdd", "collectionEdit", "collectionRemove", "collectionDelete", "removeSource", "clearSources", "scan", "mergeImport"};
        // The running download queue picks these up itself, so they never wait for it.
        static const std::set<std::string> queue_actions{"enqueue", "start", "retryQueue", "resumeQueue", "useLocation"};
        if (action == "stop") {
            const auto lane = value.value("lane", "");
            if (lane == "scan") model.cancel_scan(); else if (lane == "downloads") model.cancel_downloads(); else model.cancel();
        }
        else if (action == "removeQueue") model.remove_entry(value.at("id").get<std::uint64_t>());
        else if (action == "pauseQueue") model.pause_entry(value.at("id").get<std::uint64_t>());
        else if (action == "clearVerified") model.clear_finished();
        else if (action == "hideExtension" || action == "showExtension") return hide_extension(value.at("extension").get<std::string>(), action == "hideExtension");
        else if (action == "mergeExport") return merge_export(value);
        else {
            if (source_actions.count(action) ? state.scanning : !queue_actions.count(action) && state.busy) throw std::runtime_error("Another operation is still running");
            if (action == "addSource") model.add_source(value.at("url"), value.value("related", false), config.ca_bundle);
            else if (action == "selectSource") model.select_source(value.at("index"), value.at("selected"));
            else if (action == "sourceTags") {
                reject_collection_members({value.at("url").get<std::string>()});
                model.set_source_tags(value.at("url"), value.at("tags").get<std::vector<std::string>>());
            }
            else if (action == "addSourceTags") {
                reject_collection_members(value.at("urls").get<std::vector<std::string>>());
                add_source_tags(value.at("urls").get<std::vector<std::string>>(), value.at("tags").get<std::vector<std::string>>());
            }
            else if (action == "collectionAdd") return collection_add(value);
            else if (action == "mergeImport") return merge_import(value);
            else if (action == "collectionEdit") return collection_edit(value);
            else if (action == "collectionRemove") return collection_leave(value, false);
            else if (action == "collectionDelete") return collection_leave(value, true);
            else if (action == "fileTags") {
                if (value.at("revision").get<std::uint64_t>() != state.catalog_revision) throw std::runtime_error("Library changed; refresh the selection");
                save_file_tags(state.catalog.files.at(value.at("index").get<std::size_t>()), value.at("tags").get<std::vector<std::string>>());
            }
            else if (action == "addFileTags") {
                if (value.at("revision").get<std::uint64_t>() != state.catalog_revision) throw std::runtime_error("Library changed; refresh the selection");
                std::vector<File> files;
                for (auto index : value.at("indices").get<std::vector<std::size_t>>()) files.push_back(state.catalog.files.at(index));
                if (files.empty()) throw std::runtime_error("Select files to tag");
                add_file_tags(files, value.at("tags").get<std::vector<std::string>>());
            }
            else if (action == "selectSources") {
                if (value.contains("urls")) {
                    auto urls = value.at("urls").get<std::vector<std::string>>();
                    for (std::size_t index = 0; index < state.sources.size(); ++index) {
                        if (std::find(urls.begin(), urls.end(), state.sources[index].url) != urls.end()) model.select_source(index, value.at("selected"));
                    }
                } else model.select_all_sources(value.at("selected"));
            }
            else if (action == "removeSource") model.remove_source(value.at("index"));
            else if (action == "deleteDownload") {
                if (value.at("revision").get<std::uint64_t>() != state.catalog_revision) throw std::runtime_error("Library changed; refresh the selection");
                const auto file = state.catalog.files.at(value.at("index").get<std::size_t>());
                delete_download(file, value.at("path").get<std::string>());
                const bool source_gone = std::none_of(state.sources.begin(), state.sources.end(), [&](const SourceEntry& source) { return item_id(source.url) == file.item; });
                bool other_copies = false;
                {
                    std::lock_guard<std::mutex> guard(download_state_mutex);
                    for (const auto& record : download_state.at("records")) other_copies = other_copies || (record.at("item") == file.item && record.at("name") == file.name);
                }
                if (value.value("removeEntry", false) && source_gone && !other_copies) model.remove_files({{file.item, file.name}});
            }
            else if (action == "cleanLibrary") return clean_library(value);
            else if (action == "cleanDownloads") return {{"removed", clean_downloads(value.value("unavailable", false))}};
            else if (action == "clearSources") model.restore_sources({});
            else if (action == "scan") model.scan_selected_sources(config);
            else if (action == "start") start_downloads();
            else if (action == "retryQueue" || action == "resumeQueue") {
                const auto id = value.at("id").get<std::uint64_t>();
                const auto expected = action == "resumeQueue" ? QueueStatus::Paused : QueueStatus::Failed;
                if (!id || std::none_of(state.queue.begin(), state.queue.end(), [&](const QueueEntry& entry) { return entry.id == id && entry.status == expected; }))
                    throw std::runtime_error(action == "resumeQueue" ? "Choose a paused download to resume" : "Choose a failed download to retry");
                start_downloads(id);
            }
            else if (action == "export") model.export_files(config.output_dir);
            else if (action == "chooseDirectory") return {{"path", choose_directory()}};
            else if (action == "chooseCatalog") return {{"path", choose_file(true)}};
            else if (action == "import") model.import_catalog(std::filesystem::u8path(value.at("path").get<std::string>()));
            else if (action == "addLocation" || action == "editLocation" || action == "removeLocation" || action == "defaultLocation" || action == "useLocation") return location_command(action, value);
            else if (action == "settings") save_settings(value);
            else if (action == "forget") { if (credentials) credentials->forget(); config.s3 = {}; remember = false; }
            else if (action == "favorite") {
                if (!favorites_writable) throw std::runtime_error("Saved favorites are invalid; original file was left unchanged");
                if (value.at("revision").get<std::uint64_t>() != state.catalog_revision) throw std::runtime_error("Library changed; refresh the selection");
                auto file = state.catalog.files.at(value.at("index").get<std::size_t>());
                auto updated = favorites;
                auto key = std::make_pair(file.item, file.name);
                if (updated.count(key)) updated.erase(key); else updated[key] = file;
                Scan saved;
                for (const auto& entry : updated) saved.files.push_back(entry.second);
                save_catalog(saved, root / "favorites.xml"); favorites = std::move(updated);
            } else if (action == "enqueue") {
                if (value.at("revision").get<std::uint64_t>() != state.catalog_revision) throw std::runtime_error("Library changed; refresh the selection");
                auto indices = value.at("indices").get<std::vector<std::size_t>>();
                bool overwrite = value.value("overwrite", false);
                std::vector<std::filesystem::path> destinations(indices.size(), config.download_dir);
                std::string selection;
                Json conflicts = Json::array();
                if (named_locations_supported) {
                    std::lock_guard<std::mutex> guard(download_state_mutex);
                    if (!download_state_writable) throw std::runtime_error("Repair saved download locations before queueing");
                    if ((value.contains("locationRevision") && value.at("locationRevision") != location_revision) ||
                        (value.contains("tagRevision") && value.at("tagRevision") != tag_revision) ||
                        (value.contains("sourceRevision") && value.at("sourceRevision") != state.source_revision)) throw std::runtime_error("Locations or tags changed; refresh and queue again");
                    selection = value.value("location", download_state.at("last") == "" ? "auto" : download_state.at("last").get<std::string>());
                    auto matches = [&](const std::vector<std::string>& tags) {
                        std::set<std::string> candidates;
                        for (const auto& tag : tags) for (const auto& location : download_state.at("locations"))
                            for (const auto& rule : location.at("tags")) if (location_tag_key(rule.get<std::string>()) == location_tag_key(tag)) candidates.insert(location.at("id").get<std::string>());
                        return candidates;
                    };
                    const auto choices = value.value("locationChoices", Json::object());
                    const auto preference = download_state.at("preference").get<std::string>();
                    for (std::size_t offset = 0; offset < indices.size(); ++offset) {
                        const auto& file = state.catalog.files.at(indices[offset]);
                        std::set<std::string> candidates;
                        if (selection != "auto") candidates.insert(selection == "fallback" ? download_state.at("default").get<std::string>() : selection);
                        else if (preference != "default") {
                            const auto custom = file_tags.find({file.item, file.name});
                            const auto file_matches = custom == file_tags.end() ? std::set<std::string>{} : matches(custom->second);
                            std::vector<std::string> source_tags;
                            for (const auto& source : state.sources) if (item_id(source.url) == file.item) source_tags.insert(source_tags.end(), source.tags.begin(), source.tags.end());
                            const auto source_matches = matches(source_tags);
                            candidates = preference == "source" ? source_matches : file_matches;
                            if (candidates.empty()) candidates = preference == "source" ? file_matches : source_matches;
                        }
                        if (candidates.empty()) candidates.insert(download_state.at("default").get<std::string>());
                        auto id = *candidates.begin();
                        if (candidates.size() > 1) {
                            const auto key = std::to_string(indices[offset]);
                            if (choices.contains(key)) {
                                id = choices.at(key).get<std::string>();
                                if (!candidates.count(id)) throw std::runtime_error("Choose one of the matching locations");
                            } else {
                                Json options = Json::array();
                                for (const auto& candidate : candidates) options.push_back(find_location(download_state, candidate));
                                conflicts.push_back({{"index", indices[offset]}, {"name", file.name}, {"locations", options}});
                            }
                        }
                        const auto path = find_location(download_state, id).at("path").get<std::string>();
                        if (platform_storage) platform_storage({{"action", "validate"}, {"tree", path}});
                        destinations[offset] = platform_storage ? std::filesystem::u8path(path) : std::filesystem::weakly_canonical(std::filesystem::u8path(path));
                    }
                }
                if (!conflicts.empty()) return {{"locationConflicts", conflicts}};
                Json existing = Json::array();
                for (std::size_t offset = 0; offset < indices.size(); ++offset) {
                    const auto& file = state.catalog.files.at(indices[offset]);
                    const auto target = destinations[offset] / std::filesystem::u8path(file.name).filename();
                    const auto folder = extract ? extracted_folder(target, file.name) : std::filesystem::path{};
                    if (overwrite) continue;
                    if (platform_storage ? platform_storage({{"action", "exists"}, {"tree", destinations[offset].u8string()}, {"name", std::filesystem::u8path(file.name).filename().u8string()}, {"folder", archive_folder_name(file.name)}}).at("exists").get<bool>() : destination_exists ? destination_exists(file) : std::filesystem::exists(target)) existing.push_back(target.u8string());
                    // Extraction refuses an existing folder, so an extracted copy counts as downloaded too.
                    else if (!platform_storage && real_directory(folder)) existing.push_back(folder.u8string());
                }
                if (!existing.empty()) return {{"confirmOverwrite", existing}};
                if (!selection.empty()) location_command("useLocation", {{"id", selection}});
                for (std::size_t offset = 0; offset < indices.size(); ++offset) model.enqueue({indices[offset]}, overwrite, named_locations_supported ? destinations[offset] : std::filesystem::path{});
                if (config.automatically_start_downloads) start_downloads();
            } else throw std::runtime_error("Unknown operation");
        }
        persist();
        return {{"ok", true}};
    }
};

}
