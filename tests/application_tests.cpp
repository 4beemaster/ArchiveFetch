#include "application.hpp"

#include <libxml/parser.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace archive_catalog;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Action>
void rejects(Action action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "Expected input rejection");
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

int main() {
    auto directory = std::filesystem::temp_directory_path() /
        ("archive-catalog-app-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directory(directory);
        auto path = directory / "config.xml";
        {
            std::ofstream output(path);
            output << "<archive_catalog follow_parts='true'><source url='https://archive.org/details/example-part-0'/>"
                   << "<download item='example-part-0' name='A &amp; B.chd'/></archive_catalog>";
        }
        auto config = read_config(path);
        require(config.sources.size() == 1 && config.downloads[0].name == "A & B.chd", "Parse config and decode attributes");
        auto default_downloads = default_download_directory();
        require(default_downloads.is_absolute() && Config{}.download_dir == default_downloads && config.download_dir == default_downloads,
                "New and unspecified download destinations use the user's Downloads folder");
        for (const auto& destination : {"custom-downloads", "downloads"}) {
            {
                std::ofstream output(path);
                output << "<archive_catalog download_dir='" << destination
                       << "'><source url='https://archive.org/details/example-part-0'/></archive_catalog>";
            }
            require(read_config(path).download_dir == destination, "An explicitly saved download directory overrides the system default");
        }
        require(config.automatically_start_downloads, "Automatic downloads default to enabled for existing settings");
        require(!config.debug_window, "Debug window defaults to disabled for existing settings");
        for (const auto& value : {"false", "true", "invalid"}) {
            {
                std::ofstream output(path);
                output << "<archive_catalog debug_window='" << value
                       << "'><source url='https://archive.org/details/example-part-0'/></archive_catalog>";
            }
            if (std::string(value) == "invalid") rejects([&] { read_config(path); });
            else require(read_config(path).debug_window == (std::string(value) == "true"), "Parse debug window preference");
        }
        for (const auto& value : {"false", "true", "invalid"}) {
            {
                std::ofstream output(path);
                output << "<archive_catalog automatically_start_downloads='" << value
                       << "'><source url='https://archive.org/details/example-part-0'/></archive_catalog>";
            }
            if (std::string(value) == "invalid") rejects([&] { read_config(path); });
            else require(read_config(path).automatically_start_downloads == (std::string(value) == "true"),
                         "Parse automatic download preference");
        }
        int requests = 0;
        {
            std::ofstream output(path);
            output << "<archive_catalog/>";
        }
        require(read_config(path, true).sources.empty(), "GUI settings may have an empty source list");
        rejects([&] { read_config(path); });
        auto fetch = [&](const std::string& url) -> std::string {
            ++requests;
            if (url.find("_meta.xml") != std::string::npos) return
                "<metadata><description>&lt;a href='/details/example-part-g_202207'&gt;G&lt;/a&gt;</description></metadata>";
            return "<files><file name='A &amp; B.iso'><size>3</size></file></files>";
        };
        auto scan = scan_catalog(config, fetch);
        require(requests == 3 && scan.items.size() == 2 && scan.files.size() == 2 && scan.errors.empty(), "Discover then merge multiple item manifests");
        export_catalog(scan, directory);
        auto cache_path = directory / "cache" / "library.xml";
        save_catalog(scan, cache_path);
        auto cached = read_catalog(cache_path);
        require(cached.files.size() == scan.files.size() && cached.items == scan.items && cached.errors == scan.errors,
            "Saved library restores files, source identifiers and errors");
        auto previous_cache = read(cache_path);
        auto blocked_temporary = cache_path;
        blocked_temporary += ".tmp";
        std::filesystem::create_directory(blocked_temporary);
        rejects([&] { save_catalog(scan, cache_path); });
        require(read(cache_path) == previous_cache, "Failed library save preserves the previous cache");
        std::filesystem::remove(blocked_temporary);
        auto empty_library = scan;
        empty_library.files.clear();
        empty_library.errors = {"example-part-0 (files): HTTP 503"};
        save_catalog(empty_library, cache_path);
        cached = read_catalog(cache_path);
        require(cached.files.empty() && cached.items == scan.items && cached.errors == empty_library.errors,
            "Atomic library replacement preserves empty sources and incomplete scan errors");
        auto exported = read(directory / "catalog.xml");
        require(exported.find("complete=\"true\"") != std::string::npos, "Mark complete catalog");
        auto* document = xmlReadMemory(exported.data(), static_cast<int>(exported.size()), nullptr, nullptr, XML_PARSE_NONET);
        require(document != nullptr, "Export well-formed combined XML");
        xmlFreeDoc(document);
        require(read(directory / "catalog.csv").find("\"A & B.iso\"") != std::string::npos, "CSV export of non-CHD files");
        require(read(directory / "urls.txt").find("A%20%26%20B.iso") != std::string::npos, "Download URL list for non-CHD files");
        scan.files[0].restricted = true;
        export_catalog(scan, directory);
        require(read(directory / "catalog.xml").find("restricted=\"true\"") != std::string::npos, "Export restricted file marker");
        require(read(directory / "catalog.csv").find(",true\r\n") != std::string::npos, "CSV restricted file marker");
        auto imported = read_catalog(directory / "catalog.xml");
        require(imported.files.size() == scan.files.size() && imported.files[0].restricted && imported.files[0].name == "A & B.iso",
            "Import catalog and preserve restricted non-CHD entries");
        require(imported.errors.empty(), "Import complete catalog");
        auto excluded_catalog = scan;
        excluded_catalog.files.push_back({"example-part-0", "Metadata.XML", 3, {}, {}, false});
        excluded_catalog.files.push_back({"example-part-0", "Download.ToRrEnT", 3, {}, {}, false});
        excluded_catalog.files.push_back({"example-part-0", "example_meta.sqlite", 3, {}, {}, false});
        save_catalog(excluded_catalog, cache_path);
        require(read_catalog(cache_path).files.size() == scan.files.size() + 3, "Imported catalogs keep XML, torrent and SQLite entries");
        require(select_downloads(scan, {}).empty(), "Never download by default");
        rejects([&] { select_downloads(scan, config.downloads); });
        scan.files[0].sha1 = std::string(40, 'a');
        const Selection selected{scan.files[0].item, scan.files[0].name};
        auto queue = select_downloads(scan, {selected, selected});
        require(queue.size() == 1 && queue[0].item == selected.item, "Select exact source and deduplicate queue");
        rejects([&] { select_downloads(scan, {{"example-part-0", "missing.chd"}}); });
        auto partial = scan;
        partial.errors.push_back("Another manifest failed");
        rejects([&] { select_downloads(partial, {selected}); });
        require(select_downloads(partial, {selected, selected}, true).size() == 1, "Explicit partial selection preserves deduplication");
        rejects([&] { select_downloads(partial, {{selected.item, "missing.chd"}}, true); });
        partial.files[0].sha1.clear();
        partial.files[0].md5.clear();
        rejects([&] { select_downloads(partial, {selected}, true); });
        config.follow_parts = false;
        requests = 0;
        scan = scan_catalog(config, fetch);
        require(requests == 1 && scan.items.size() == 1, "Single-item mode does not fetch descriptions");
        scan = scan_catalog(config, [](const std::string&) -> std::string { throw std::runtime_error("HTTP 403"); });
        require(scan.errors.size() == 1 && scan.files.empty(), "Report failed manifests");
        rejects([&] { select_downloads(scan, {}); });
        export_catalog(scan, directory);
        require(read(directory / "catalog.xml").find("complete=\"false\"") != std::string::npos, "Never mark partial catalog complete");
        require(!read_catalog(directory / "catalog.xml").errors.empty(), "Imported incomplete catalog retains source errors");
        {
            std::ofstream output(path);
            output << "<!DOCTYPE archive_catalog [<!ENTITY leak SYSTEM 'file:///etc/passwd'>]><archive_catalog/>";
        }
        rejects([&] { read_config(path); });
        {
            std::ofstream output(path);
            output << "<archive_catalog follow_parts='maybe'><source url='https://archive.org/details/example'/></archive_catalog>";
        }
        rejects([&] { read_config(path); });
        std::filesystem::remove_all(directory);
        std::cout << "All application tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(directory);
        std::cerr << error.what() << '\n';
        return 1;
    }
}