#pragma once

#include "application.hpp"

#include <atomic>
#include <mutex>
#include <set>
#include <thread>

namespace archive_catalog {

enum class QueueStatus { Pending, Active, Verified, Failed, Paused };

struct QueueEntry {
    File file;
    QueueStatus status = QueueStatus::Pending;
    std::uint64_t received = 0;
    std::string detail;
    bool overwrite = false;
    std::uint64_t id = 0;
    std::filesystem::path partial_path;
    bool removal_requested = false;
    std::filesystem::path destination;
    std::string stage;
    std::uint64_t stage_done = 0, stage_total = 0;
    // Bytes per second while transferring, 0 otherwise.
    std::uint64_t rate = 0;
    bool pause_requested = false;
};

struct SourceEntry {
    std::string url;
    std::string name;
    bool selected = true;
    std::string last_scanned;
    std::string error;
    std::vector<std::string> tags;
};

std::vector<std::string> normalize_tags(const std::vector<std::string>& tags);
std::vector<SourceEntry> read_source_list(const std::filesystem::path& path);
void save_source_list(const std::filesystem::path& path, const std::vector<SourceEntry>& sources);

struct GuiSnapshot {
    Scan catalog;
    std::vector<SourceEntry> sources;
    std::uint64_t source_revision = 0;
    std::uint64_t catalog_revision = 0;
    std::uint64_t download_revision = 0;
    std::vector<QueueEntry> queue;
    bool loaded = false;
    bool busy = false; // scanning || downloading
    bool scanning = false;
    bool downloading = false;
    std::string activity = "Ready";
    std::string notice;
};

std::vector<bool> library_download_status(const std::vector<File>& files, const std::filesystem::path& directory);
const char* queue_status_name(QueueStatus status);

class GuiModel {
public:
    ~GuiModel();
    GuiSnapshot snapshot() const;
    void start_scan(Config config, Fetch fetch = {});
    void restore_sources(std::vector<SourceEntry> sources);
    void add_source(std::string url, bool related, std::string ca_bundle = {}, Fetch fetch = {});
    void select_source(std::size_t index, bool selected);
    void set_source_tags(const std::string& url, std::vector<std::string> tags);
    void select_all_sources(bool selected);
    void remove_source(std::size_t index);
    void scan_selected_sources(Config config, Fetch fetch = {});
    void import_catalog(std::filesystem::path path);
    std::size_t remove_files(const std::set<std::pair<std::string, std::string>>& files);
    // Adds files not already in the catalog (by item and name); returns how many were added.
    std::size_t add_files(const std::vector<File>& files);
    void export_files(std::filesystem::path directory);
    void enqueue(const std::vector<std::size_t>& indices, bool overwrite = false, std::filesystem::path destination = {});
    void remove(std::size_t index);
    void remove_entry(std::uint64_t id);
    void pause_entry(std::uint64_t id);
    void clear_finished();
    using Download = std::function<void(const File&, const std::filesystem::path&, bool, const TransferHooks&)>;
    using PartialPath = std::function<std::filesystem::path(const File&, const std::filesystem::path&)>;
    void start_downloads(Config config, Download download = {}, std::uint64_t retry_id = 0, PartialPath partial_path = {});
    void cancel();
    void cancel_scan();
    void cancel_downloads();
    void wait();

private:
    // Source work (adding, scanning, import, export) and downloads run on separate workers.
    enum class Lane { Library, Downloads };
    bool launch(Lane lane, const std::string& activity, std::function<void()> operation, bool exclusive = false);
    void activity(const std::string& message);
    mutable std::mutex mutex_;
    GuiSnapshot state_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> scan_cancelled_{false};
    std::thread worker_;
    std::thread scan_worker_;
    std::uint64_t next_queue_id_ = 1;
};

}