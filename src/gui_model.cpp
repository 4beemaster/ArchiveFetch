#include "gui_model.hpp"
#include "transfer.hpp"
#include "ps5_identity.hpp"

#include <libxml/parser.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <set>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace archive_catalog {
namespace {

std::string lowercase(std::string value) {
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    }
    return value;
}

std::string timestamp() {
    auto now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream result;
    result << std::put_time(&utc, "%Y-%m-%d %H:%M UTC");
    return result.str();
}

std::string source_attribute(xmlNode* node, const char* name) {
    auto* value = xmlGetProp(node, BAD_CAST name);
    std::string result = value ? reinterpret_cast<const char*>(value) : "";
    xmlFree(value);
    return result;
}

}

std::vector<std::string> normalize_tags(const std::vector<std::string>& tags) {
    if (tags.size() > 32) throw std::runtime_error("Use at most 32 tags per entry");
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (auto tag : tags) {
        auto first = tag.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        tag = tag.substr(first, tag.find_last_not_of(" \t\r\n") - first + 1);
        if (tag.size() > 128 || std::any_of(tag.begin(), tag.end(), [](unsigned char character) { return character < 32 || character == 127; })) {
            throw std::runtime_error("Tags must be at most 128 bytes and contain no control characters");
        }
        if (seen.insert(lowercase(tag)).second) result.push_back(std::move(tag));
    }
    return result;
}

namespace {

void validate_sources(const std::vector<SourceEntry>& sources) {
    if (sources.size() > 128) throw std::runtime_error("Source list exceeds the 128-item limit");
    std::set<std::string> seen;
    for (const auto& source : sources) {
        if (!seen.insert(item_id(source.url)).second) throw std::runtime_error("Duplicate source in saved list");
        if (source.name.size() > 4096 || source.last_scanned.size() > 64 || source.error.size() > 4096) {
            throw std::runtime_error("Source details are too large");
        }
        normalize_tags(source.tags);
    }
}

}

std::vector<SourceEntry> read_source_list(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) > 1024 * 1024) throw std::runtime_error("Source list exceeds 1 MiB");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open source list");
    const std::string xml{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> document(xmlReadMemory(xml.data(), static_cast<int>(xml.size()), nullptr,
        "UTF-8", XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING), xmlFreeDoc);
    if (!document || document->intSubset || document->extSubset) throw std::runtime_error("Invalid source list XML or forbidden DTD");
    auto* root = xmlDocGetRootElement(document.get());
    if (!root || !xmlStrEqual(root->name, BAD_CAST "sources")) throw std::runtime_error("Expected a source list");
    std::vector<SourceEntry> result;
    for (auto* node = root->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE) continue;
        if (!xmlStrEqual(node->name, BAD_CAST "source")) throw std::runtime_error("Unknown source list entry");
        auto checked = source_attribute(node, "selected");
        if (checked != "true" && checked != "false") throw std::runtime_error("Invalid source selection");
        result.push_back({source_attribute(node, "url"), source_attribute(node, "name"), checked == "true",
                          source_attribute(node, "last_scanned"), source_attribute(node, "error")});
        for (auto* child = node->children; child; child = child->next) {
            if (child->type != XML_ELEMENT_NODE) continue;
            if (!xmlStrEqual(child->name, BAD_CAST "tag")) throw std::runtime_error("Unknown source detail");
            auto* text = xmlNodeGetContent(child);
            result.back().tags.emplace_back(text ? reinterpret_cast<const char*>(text) : "");
            xmlFree(text);
        }
        result.back().tags = normalize_tags(result.back().tags);
    }
    validate_sources(result);
    return result;
}

void save_source_list(const std::filesystem::path& path, const std::vector<SourceEntry>& sources) {
    validate_sources(sources);
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> document(xmlNewDoc(BAD_CAST "1.0"), xmlFreeDoc);
    if (!document) throw std::runtime_error("Cannot create source list");
    auto* root = xmlNewNode(nullptr, BAD_CAST "sources");
    if (!root) throw std::runtime_error("Cannot create source list root");
    xmlDocSetRootElement(document.get(), root);
    for (const auto& source : sources) {
        auto* node = xmlNewChild(root, nullptr, BAD_CAST "source", nullptr);
        if (!node) throw std::runtime_error("Cannot create source entry");
        auto property = [&](const char* name, const std::string& value) {
            if (!xmlNewProp(node, BAD_CAST name, BAD_CAST value.c_str())) throw std::runtime_error("Cannot write source attribute");
        };
        property("url", source.url);
        property("name", source.name);
        property("selected", source.selected ? "true" : "false");
        property("last_scanned", source.last_scanned);
        property("error", source.error);
        for (const auto& tag : normalize_tags(source.tags)) {
            auto* child = xmlNewChild(node, nullptr, BAD_CAST "tag", nullptr);
            if (!child || !xmlAddChild(child, xmlNewText(BAD_CAST tag.c_str()))) throw std::runtime_error("Cannot write source tag");
        }
    }
    xmlChar* buffer = nullptr;
    int size = 0;
    xmlDocDumpFormatMemoryEnc(document.get(), &buffer, &size, "UTF-8", 1);
    std::unique_ptr<xmlChar, void (*)(void*)> owned(buffer, xmlFree);
    if (!buffer || size > 1024 * 1024) throw std::runtime_error("Cannot serialize source list within 1 MiB");
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(buffer), size);
    output.close();
    if (!output) throw std::runtime_error("Cannot save source list");
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Cannot replace saved source list");
    }
#else
    std::filesystem::rename(temporary, path);
#endif
}

std::vector<bool> library_download_status(const std::vector<File>& files, const std::filesystem::path& directory) {
    std::vector<bool> downloaded(files.size(), false);
    if (directory.empty()) return downloaded;
    for (std::size_t index = 0; index < files.size(); ++index) {
        try {
            const auto& file = files[index];
            download_url(file);
            auto path = directory / std::filesystem::u8path(file.name).filename();
            std::error_code error;
            auto status = std::filesystem::symlink_status(path, error);
            if (error || !std::filesystem::is_regular_file(status)) continue;
            auto size = std::filesystem::file_size(path, error);
            downloaded[index] = !error && size == file.size;
        } catch (const std::exception&) {
            downloaded[index] = false;
        }
    }
    return downloaded;
}

const char* queue_status_name(QueueStatus status) {
    switch (status) {
    case QueueStatus::Pending: return "Queued";
    case QueueStatus::Active: return "Transferring / verifying";
    case QueueStatus::Verified: return "Verified";
    case QueueStatus::Failed: return "Failed";
    case QueueStatus::Paused: return "Paused";
    }
    return "Unknown";
}

GuiModel::~GuiModel() { cancel(); wait(); }

GuiSnapshot GuiModel::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void GuiModel::wait() {
    if (worker_.joinable()) worker_.join();
    if (scan_worker_.joinable()) scan_worker_.join();
}
void GuiModel::cancel() { cancelled_ = true; scan_cancelled_ = true; }
void GuiModel::cancel_scan() { scan_cancelled_ = true; }
void GuiModel::cancel_downloads() { cancelled_ = true; }

void GuiModel::activity(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.activity = message;
}

// An exclusive operation needs both lanes idle; otherwise only its own lane must be free.
bool GuiModel::launch(Lane lane, const std::string& message, std::function<void()> operation, bool exclusive) {
    const bool downloads = lane == Lane::Downloads;
    auto& running = downloads ? state_.downloading : state_.scanning;
    auto& worker = downloads ? worker_ : scan_worker_;
    auto& cancelled = downloads ? cancelled_ : scan_cancelled_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running || (exclusive && state_.busy)) return false;
    }
    if (worker.joinable()) worker.join();
    cancelled = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running = true;
        state_.busy = true;
        state_.activity = message;
        state_.notice.clear();
    }
    auto finish = [this, downloads, &running] {
        std::lock_guard<std::mutex> lock(mutex_);
        running = false;
        state_.busy = state_.scanning || state_.downloading;
        if (!state_.busy) state_.activity = "Ready";
        else if (!downloads)
            for (const auto& entry : state_.queue) if (entry.status == QueueStatus::Active) state_.activity = entry.file.name;
    };
    try {
        worker = std::thread([this, downloads, finish, operation = std::move(operation)] {
            name_archivefetch_thread();
            try { operation(); }
            catch (const OperationCancelled&) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!downloads) state_.notice = "Stopped. Sources scanned so far were kept.";
                else {
                    state_.notice = "Stopped. Completed results were kept; partial downloads can resume.";
                    for (auto& entry : state_.queue) {
                        if (entry.status == QueueStatus::Active) entry.status = QueueStatus::Paused;
                    }
                }
            } catch (const std::exception& error) {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.notice = error.what();
            }
            finish();
        });
    } catch (...) {
        finish();
        throw;
    }
    return true;
}

void GuiModel::start_scan(Config config, Fetch fetch) {
    launch(Lane::Library, "Connecting to Archive.org", [this, config = std::move(config), fetch = std::move(fetch)] {
        TransferHooks hooks;
        hooks.cancelled = [this] { return scan_cancelled_.load(); };
        Http http(config.ca_bundle, hooks);
        auto result = scan_catalog(config, [&](const std::string& url) {
            if (scan_cancelled_) throw OperationCancelled();
            activity("Scanning " + item_id(url));
            return fetch ? fetch(url) : http.get(url);
        });
        if (scan_cancelled_) throw OperationCancelled();
        std::lock_guard<std::mutex> lock(mutex_);
        state_.catalog = std::move(result);
        ++state_.catalog_revision;
        state_.queue.clear();
        state_.loaded = true;
        state_.notice = state_.catalog.errors.empty() ? "Scan complete." : "Scan incomplete. Available files can be queued; some sources are missing.";
        for (const auto& warning : state_.catalog.warnings) state_.notice += " " + warning + ".";
    }, true);
}

void GuiModel::restore_sources(std::vector<SourceEntry> sources) {
    validate_sources(sources);
    for (auto& source : sources) source.tags = normalize_tags(source.tags);
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.scanning) throw std::runtime_error("Cannot restore sources during a scan");
    state_.sources = std::move(sources);
    ++state_.source_revision;
}

void GuiModel::select_source(std::size_t index, bool selected) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.scanning) return;
    state_.sources.at(index).selected = selected;
    ++state_.source_revision;
}

void GuiModel::set_source_tags(const std::string& url, std::vector<std::string> tags) {
    tags = normalize_tags(tags);
    auto identifier = item_id(url);
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.scanning) throw std::runtime_error("Cannot edit tags during a scan");
    auto source = std::find_if(state_.sources.begin(), state_.sources.end(), [&](const SourceEntry& entry) { return item_id(entry.url) == identifier; });
    if (source == state_.sources.end()) throw std::runtime_error("Source no longer exists");
    source->tags = std::move(tags);
    ++state_.source_revision;
}

void GuiModel::select_all_sources(bool selected) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.scanning) return;
    for (auto& source : state_.sources) source.selected = selected;
    ++state_.source_revision;
}

void GuiModel::remove_source(std::size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.scanning) return;
    if (index >= state_.sources.size()) throw std::runtime_error("Invalid source selection");
    state_.sources.erase(state_.sources.begin() + static_cast<std::ptrdiff_t>(index));
    ++state_.source_revision;
}

void GuiModel::add_source(std::string url, bool related, std::string ca_bundle, Fetch fetch) {
    auto item = item_id(url);
    launch(Lane::Library, "Validating Archive.org source", [this, item = std::move(item), related, ca_bundle = std::move(ca_bundle), fetch = std::move(fetch)] {
        TransferHooks hooks;
        hooks.cancelled = [this] { return scan_cancelled_.load(); };
        Http http(ca_bundle, hooks);
        auto metadata = [&](const std::string& identifier) {
            if (scan_cancelled_) throw OperationCancelled();
            activity("Validating " + identifier);
            auto address = "https://archive.org/download/" + identifier + "/" + identifier + "_meta.xml";
            return read_item_metadata(fetch ? fetch(address) : http.get(address));
        };
        auto primary = metadata(item);
        auto items = related ? discover_items(item, primary.description) : std::vector<std::string>{item};
        auto current = snapshot().sources;
        std::set<std::string> seen;
        for (auto& source : current) {
            auto identifier = item_id(source.url);
            seen.insert(identifier);
            if (identifier == item && !primary.title.empty()) source.name = primary.title.substr(0, 4096);
        }
        std::size_t added = 0;
        const auto first_added = current.size();
        for (const auto& identifier : items) {
            if (scan_cancelled_) throw OperationCancelled();
            if (seen.count(identifier)) continue;
            if (current.size() >= 128) throw std::runtime_error("Source list exceeds the 128-item limit");
            SourceEntry entry{"https://archive.org/details/" + identifier,
                              identifier == item && !primary.title.empty() ? primary.title : identifier, true, {}, {}};
            if (entry.name.size() > 4096) throw std::runtime_error("Source title is too large");
            current.push_back(std::move(entry));
            seen.insert(identifier);
            ++added;
        }
        if (scan_cancelled_) throw OperationCancelled();
        std::lock_guard<std::mutex> lock(mutex_);
        state_.sources = std::move(current);
        ++state_.source_revision;
        state_.notice = "Added " + std::to_string(added) + " sources.";
        for (auto index = first_added; index < state_.sources.size(); ++index) {
            std::cerr << ("Added source " + state_.sources[index].url +
                          " from https://archive.org/details/" + item + "\n");
        }
    });
}

void GuiModel::scan_selected_sources(Config config, Fetch fetch) {
    auto current = snapshot();
    if (current.scanning) return;
    if (std::none_of(current.sources.begin(), current.sources.end(), [](const SourceEntry& source) { return source.selected; })) {
        throw std::runtime_error("Select at least one source to scan.");
    }
    launch(Lane::Library, "Scanning selected sources", [this, config = std::move(config), fetch = std::move(fetch), current = std::move(current)]() mutable {
        TransferHooks hooks;
        hooks.cancelled = [this] { return scan_cancelled_.load(); };
        Http http(config.ca_bundle, hooks);
        std::vector<std::size_t> selected;
        for (std::size_t index = 0; index < current.sources.size(); ++index) {
            if (current.sources[index].selected) selected.push_back(index);
        }
        std::atomic<std::size_t> next{0};
        std::vector<std::string> warnings;
        std::vector<std::future<void>> workers;
        for (std::size_t worker = 0; worker < std::min<std::size_t>(3, selected.size()); ++worker) {
            workers.push_back(std::async(std::launch::async, [&] {
                auto source_config = config;
                source_config.follow_parts = false;
                while (!scan_cancelled_) {
                    auto position = next.fetch_add(1);
                    if (position >= selected.size() || scan_cancelled_) return;
                    auto index = selected[position];
                    const auto& source = current.sources[index];
                    source_config.sources = {source.url};
                    auto result = scan_catalog(source_config, [&](const std::string& url) {
                        if (scan_cancelled_) throw OperationCancelled();
                        activity("Scanning " + source.name);
                        return fetch ? fetch(url) : http.get(url);
                    });
                    auto item = item_id(source.url);
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (scan_cancelled_) throw OperationCancelled();
                    warnings.insert(warnings.end(), result.warnings.begin(), result.warnings.end());
                    auto catalog = state_.catalog;
                    auto updated_source = state_.sources[index];
                    auto& errors = catalog.errors;
                    errors.erase(std::remove_if(errors.begin(), errors.end(), [&](const std::string& error) {
                        return error.rfind(item + " (", 0) == 0;
                    }), errors.end());
                    updated_source.error.clear();
                    if (result.errors.empty()) {
                        auto& files = catalog.files;
                        files.erase(std::remove_if(files.begin(), files.end(), [&](const File& file) { return file.item == item; }), files.end());
                        merge_files(files, result.files);
                        if (std::find(catalog.items.begin(), catalog.items.end(), item) == catalog.items.end()) {
                            catalog.items.push_back(item);
                        }
                        updated_source.last_scanned = timestamp();
                    } else {
                        updated_source.error = result.errors.front().substr(0, 4096);
                        errors.insert(errors.end(), result.errors.begin(), result.errors.end());
                    }
                    state_.catalog = std::move(catalog);
                    ++state_.catalog_revision;
                    state_.sources[index] = std::move(updated_source);
                    state_.loaded = true;
                    ++state_.source_revision;
                }
            }));
        }
        std::exception_ptr failure;
        for (auto& worker : workers) {
            try { worker.get(); }
            catch (...) { if (!failure) failure = std::current_exception(); }
        }
        if (failure) std::rethrow_exception(failure);
        if (scan_cancelled_) throw OperationCancelled();
        std::lock_guard<std::mutex> lock(mutex_);
        state_.notice = state_.catalog.errors.empty() ? "Selected sources scanned. Library updated." :
            "Some sources failed. Previous files were retained; see source errors.";
        for (const auto& warning : warnings) state_.notice += " " + warning + ".";
    });
}

void GuiModel::import_catalog(std::filesystem::path path) {
    launch(Lane::Library, "Opening catalog", [this, path = std::move(path)] {
        auto catalog = read_catalog(path);
        if (scan_cancelled_) throw OperationCancelled();
        std::lock_guard<std::mutex> lock(mutex_);
        state_.catalog = std::move(catalog);
        ++state_.catalog_revision;
        state_.queue.clear();
        state_.loaded = true;
        state_.notice = "Catalog opened.";
    }, true);
}

// Files are matched by (item, name); queued entries keep their own copy of the file and are unaffected.
std::size_t GuiModel::remove_files(const std::set<std::pair<std::string, std::string>>& files) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.busy) throw std::runtime_error("Stop the current operation before cleaning the library");
    auto& catalog = state_.catalog.files;
    const auto before = catalog.size();
    catalog.erase(std::remove_if(catalog.begin(), catalog.end(), [&](const File& file) { return files.count({file.item, file.name}) != 0; }), catalog.end());
    const auto removed = before - catalog.size();
    if (!removed) return 0;
    std::set<std::string> remaining;
    for (const auto& file : catalog) remaining.insert(file.item);
    auto& items = state_.catalog.items;
    items.erase(std::remove_if(items.begin(), items.end(), [&](const std::string& item) { return !remaining.count(item); }), items.end());
    ++state_.catalog_revision;
    return removed;
}

std::size_t GuiModel::add_files(const std::vector<File>& files) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.scanning) throw std::runtime_error("Wait for the scan to finish before merging");
    const auto before = state_.catalog.files.size();
    merge_files(state_.catalog.files, files);
    const auto added = state_.catalog.files.size() - before;
    if (!added) return 0;
    auto& items = state_.catalog.items;
    for (const auto& file : files)
        if (std::find(items.begin(), items.end(), file.item) == items.end()) items.push_back(file.item);
    state_.loaded = true;
    ++state_.catalog_revision;
    return added;
}

void GuiModel::export_files(std::filesystem::path directory) {
    auto current = snapshot();
    if (!current.loaded || current.busy) return;
    launch(Lane::Library, "Exporting catalog", [this, catalog = std::move(current.catalog), directory = std::move(directory)] {
        export_catalog(catalog, directory);
        std::lock_guard<std::mutex> lock(mutex_);
        state_.notice = "Exported catalog.xml, catalog.csv and urls.txt.";
    }, true);
}

void GuiModel::enqueue(const std::vector<std::size_t>& indices, bool overwrite, std::filesystem::path destination) {
    if (!destination.empty()) destination = std::filesystem::weakly_canonical(std::filesystem::absolute(destination));
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.loaded) return;
    std::vector<Selection> selections;
    for (auto index : indices) {
        if (index >= state_.catalog.files.size()) throw std::runtime_error("Invalid catalog selection");
        const auto& file = state_.catalog.files[index];
        selections.push_back({file.item, file.name});
    }
    auto candidates = select_downloads(state_.catalog, selections, true);
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto& entry : state_.queue) seen.emplace(entry.file.item, entry.file.name);
    for (auto& file : candidates) {
        if (seen.emplace(file.item, file.name).second) {
            state_.queue.push_back({std::move(file), QueueStatus::Pending, 0, {}, overwrite});
            state_.queue.back().id = next_queue_id_++;
            state_.queue.back().destination = destination;
        } else if (overwrite) {
            auto existing = std::find_if(state_.queue.begin(), state_.queue.end(), [&](const QueueEntry& entry) {
                return entry.file.item == file.item && entry.file.name == file.name;
            });
            if (existing->status == QueueStatus::Active) continue;
            auto partial_path = existing->partial_path;
            *existing = {std::move(file), QueueStatus::Pending, 0, {}, true};
            existing->partial_path = std::move(partial_path);
            existing->destination = destination;
            existing->id = next_queue_id_++;
        }
    }
    state_.notice = "Selection added to queue.";
}

namespace {
void remove_partial(const QueueEntry& entry) {
    if (entry.partial_path.empty()) return;
    std::error_code error;
    for (int index = 1; index < max_partial_segments; ++index) {
        const auto segment = partial_segment(entry.partial_path, index);
        if (std::filesystem::is_regular_file(std::filesystem::symlink_status(segment, error))) std::filesystem::remove(segment);
    }
    auto status = std::filesystem::symlink_status(entry.partial_path, error);
    if (error == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found) return;
    if (error) throw std::filesystem::filesystem_error("Cannot inspect partial download", entry.partial_path, error);
    if (!std::filesystem::is_regular_file(status)) throw std::runtime_error("Partial download is not a regular file: " + entry.partial_path.string());
    std::filesystem::remove(entry.partial_path);
}
}

void GuiModel::remove(std::size_t index) {
    std::uint64_t id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= state_.queue.size()) return;
        id = state_.queue[index].id;
    }
    remove_entry(id);
}

void GuiModel::remove_entry(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = std::find_if(state_.queue.begin(), state_.queue.end(), [id](const QueueEntry& entry) { return entry.id == id; });
    if (found == state_.queue.end()) return;
    auto& entry = *found;
    if (state_.downloading && entry.status == QueueStatus::Active) {
        entry.removal_requested = true;
        entry.detail = "Cancelling and removing partial download";
        return;
    }
    bool shared_active_partial = std::any_of(state_.queue.begin(), state_.queue.end(), [&](const QueueEntry& other) {
        return other.id != entry.id && other.status == QueueStatus::Active && other.partial_path == entry.partial_path;
    });
    if (entry.status != QueueStatus::Verified && !shared_active_partial) remove_partial(entry);
    state_.queue.erase(found);
}

void GuiModel::clear_finished() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.busy && !state_.downloading) return;
    auto& queue = state_.queue;
    queue.erase(std::remove_if(queue.begin(), queue.end(), [](const QueueEntry& entry) {
        return entry.status == QueueStatus::Verified;
    }), queue.end());
}

void GuiModel::start_downloads(Config config, Download download, std::uint64_t retry_id) {
    auto current = snapshot();
    if (!current.loaded) return;
    if (retry_id) {
        auto entry = std::find_if(current.queue.begin(), current.queue.end(), [retry_id](const QueueEntry& queued) { return queued.id == retry_id; });
        if (entry == current.queue.end() || entry->status != QueueStatus::Failed)
            throw std::runtime_error("Failed download is no longer available for retry");
    }
    if (current.downloading) {
        // The running queue picks up new entries itself; a retried one gets a new id so it is picked up too.
        if (!retry_id) return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto entry = std::find_if(state_.queue.begin(), state_.queue.end(), [retry_id](const QueueEntry& queued) { return queued.id == retry_id; });
        if (entry != state_.queue.end() && entry->status == QueueStatus::Failed) {
            entry->status = QueueStatus::Pending;
            entry->detail.clear();
            entry->id = next_queue_id_++;
        }
        return;
    }
    if (config.download_dir.empty()) throw std::runtime_error("Choose a download directory.");
    config.s3.authorization();
    if (std::none_of(current.queue.begin(), current.queue.end(), [retry_id](const QueueEntry& entry) {
            return entry.status != QueueStatus::Verified && (!retry_id || entry.id == retry_id);
        })) return;
    config.download_dir = std::filesystem::weakly_canonical(std::filesystem::absolute(config.download_dir));
    std::uint64_t first_new_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        first_new_id = next_queue_id_;
        // Known before the run starts, so removing a pending entry also removes a partial file left by an earlier run.
        for (auto& entry : state_.queue) {
            if (entry.status == QueueStatus::Verified || (retry_id && entry.id != retry_id)) continue;
            if (entry.destination.empty()) entry.destination = config.download_dir;
            entry.partial_path = entry.destination / std::filesystem::u8path(entry.file.name).filename();
            entry.partial_path += ".part";
        }
    }
    launch(Lane::Downloads, "Starting download queue", [this, config = std::move(config), retry_id, first_new_id, download = std::move(download)] {
        bool failed = false;
        std::set<std::uint64_t> attempted;
        for (;;) {
            if (cancelled_) throw OperationCancelled();
            QueueEntry queued;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // Queue order, including entries added while this runs; a retry run also takes newly added ones.
                auto next = std::find_if(state_.queue.begin(), state_.queue.end(), [&](const QueueEntry& entry) {
                    return entry.status != QueueStatus::Verified && !attempted.count(entry.id) &&
                        (!retry_id || entry.id == retry_id || entry.id >= first_new_id);
                });
                if (next == state_.queue.end()) break;
                attempted.insert(next->id);
                if (next->destination.empty()) next->destination = config.download_dir;
                next->partial_path = next->destination / std::filesystem::u8path(next->file.name).filename();
                next->partial_path += ".part";
                next->removal_requested = false;
                next->status = QueueStatus::Active;
                next->detail.clear();
                next->stage.clear();
                state_.activity = next->file.name;
                queued = *next;
            }
            auto find_entry = [this, id = queued.id] {
                return std::find_if(state_.queue.begin(), state_.queue.end(), [id](const QueueEntry& entry) { return entry.id == id; });
            };
            TransferHooks hooks;
            hooks.cancelled = [this, find_entry] {
                if (cancelled_) return true;
                std::lock_guard<std::mutex> lock(mutex_);
                auto entry = find_entry();
                return entry == state_.queue.end() || entry->removal_requested;
            };
            // Resumed bytes are not counted: the first report only sets the starting point.
            struct Speed { std::chrono::steady_clock::time_point at; std::uint64_t bytes = 0; bool started = false; double rate = 0; };
            auto speed = std::make_shared<Speed>();
            hooks.progress = [this, find_entry, speed](std::uint64_t received, std::uint64_t) {
                const auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock(mutex_);
                auto entry = find_entry();
                if (entry == state_.queue.end()) return;
                entry->received = std::min(received, entry->file.size);
                if (!speed->started) { speed->started = true; speed->at = now; speed->bytes = received; return; }
                const double seconds = std::chrono::duration<double>(now - speed->at).count();
                if (seconds < 1) return;
                const double sample = received > speed->bytes ? static_cast<double>(received - speed->bytes) / seconds : 0;
                speed->rate = speed->rate == 0 ? sample : speed->rate * 0.7 + sample * 0.3;
                speed->at = now; speed->bytes = received;
                entry->rate = static_cast<std::uint64_t>(speed->rate);
            };
            hooks.status = [this, find_entry](const std::string& text) {
                std::lock_guard<std::mutex> lock(mutex_);
                auto entry = find_entry();
                if (entry != state_.queue.end()) entry->detail = text;
            };
            hooks.stage = [this, find_entry](const std::string& stage, std::uint64_t done, std::uint64_t total) {
                std::lock_guard<std::mutex> lock(mutex_);
                auto entry = find_entry();
                if (entry != state_.queue.end()) { entry->stage = stage; entry->stage_done = done; entry->stage_total = total; entry->rate = 0; }
            };
            bool stopped = false;
            std::string failure;
            try {
                if (download) download(queued.file, queued.destination, queued.overwrite, hooks);
                else {
                    Http http(config.ca_bundle, hooks, config.s3, config.download_connections);
                    http.download(queued.file, queued.destination, queued.overwrite);
                }
            } catch (const OperationCancelled&) { stopped = true; }
            catch (const std::exception& error) { failure = error.what(); }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto entry = find_entry();
                if (entry != state_.queue.end()) {
                    entry->stage.clear();
                    entry->rate = 0;
                    if (entry->removal_requested) {
                        try {
                            if (!stopped && failure.empty()) ++state_.download_revision;
                            remove_partial(*entry);
                            state_.queue.erase(entry);
                        } catch (const std::exception& error) {
                            entry->status = QueueStatus::Failed;
                            entry->removal_requested = false;
                            entry->detail = error.what();
                            failed = true;
                        }
                        stopped = false;
                    } else if (stopped) entry->status = QueueStatus::Paused;
                    else if (!failure.empty()) {
                        failed = true;
                        entry->status = QueueStatus::Failed;
                        entry->detail = failure;
                    } else {
                        entry->status = QueueStatus::Verified;
                        ++state_.download_revision;
                        entry->received = entry->file.size;
                    }
                }
            }
            if (cancelled_ || stopped) throw OperationCancelled();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        state_.queue.erase(std::remove_if(state_.queue.begin(), state_.queue.end(), [](const QueueEntry& entry) {
            return entry.status == QueueStatus::Verified;
        }), state_.queue.end());
        failed = failed || std::any_of(state_.queue.begin(), state_.queue.end(), [](const QueueEntry& entry) { return entry.status == QueueStatus::Failed; });
        state_.notice = failed ? "Queue finished with errors." : "Queue complete. All selected files verified.";
    });
}

}