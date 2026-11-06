#include "application.hpp"
#include "transfer.hpp"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

namespace archive_catalog {
namespace {

using Document = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;

std::string text(xmlChar* value) {
    if (!value) return {};
    std::string result(reinterpret_cast<const char*>(value));
    xmlFree(value);
    return result;
}

Document parse(const std::string& xml, const char* root_name) {
    if (xml.size() > INT_MAX) throw std::runtime_error("XML document is too large");
    Document document(xmlReadMemory(xml.data(), static_cast<int>(xml.size()), nullptr, "UTF-8",
                                   XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING), xmlFreeDoc);
    if (!document || document->intSubset || document->extSubset) throw std::runtime_error("Invalid XML or forbidden DTD");
    auto* root = xmlDocGetRootElement(document.get());
    if (!root || !xmlStrEqual(root->name, BAD_CAST root_name)) throw std::runtime_error(std::string("Expected XML root: ") + root_name);
    return document;
}

std::string attribute(xmlNode* node, const char* name) {
    return text(xmlGetProp(node, BAD_CAST name));
}

std::string required_attribute(xmlNode* node, const char* name) {
    auto value = attribute(node, name);
    if (value.empty()) throw std::runtime_error(std::string("Missing configuration attribute: ") + name);
    return value;
}

void property(xmlNode* node, const char* name, const std::string& value) {
    if (!xmlNewProp(node, BAD_CAST name, BAD_CAST value.c_str())) throw std::runtime_error("Cannot create XML attribute");
}

std::string csv(std::string value) {
    if (!value.empty() && std::string("=+-@\t\r\n").find(value.front()) != std::string::npos) value.insert(value.begin(), '\'');
    std::string escaped = "\"";
    for (char character : value) {
        escaped += character;
        if (character == '"') escaped += '"';
    }
    return escaped + '"';
}

void save_xml(xmlDoc* document, const std::filesystem::path& path) {
    xmlChar* buffer = nullptr;
    int size = 0;
    xmlDocDumpFormatMemoryEnc(document, &buffer, &size, "UTF-8", 1);
    if (!buffer) throw std::runtime_error("Cannot serialize XML");
    std::unique_ptr<xmlChar, void (*)(void*)> owned(buffer, xmlFree);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(buffer), size);
    output.close();
    if (!output) throw std::runtime_error("Cannot save " + path.string());
}

}

std::filesystem::path default_download_directory() {
#ifdef _WIN32
    PWSTR directory = nullptr;
    auto result = SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DONT_VERIFY, nullptr, &directory);
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(directory, CoTaskMemFree);
    if (SUCCEEDED(result) && directory) return std::filesystem::path(directory);
    auto* home = _wgetenv(L"USERPROFILE");
    if (home && *home) return std::filesystem::path(home) / "Downloads";
#else
    auto* home = std::getenv("HOME");
    if (home && *home) return std::filesystem::u8path(home) / "Downloads";
#endif
    return "downloads";
}

ItemMetadata read_item_metadata(const std::string& xml) {
    auto document = parse(xml, "metadata");
    ItemMetadata result;
    for (auto* node = xmlDocGetRootElement(document.get())->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE) continue;
        if (xmlStrEqual(node->name, BAD_CAST "title")) {
            result.title = text(xmlNodeGetContent(node));
            for (auto& character : result.title) {
                if (static_cast<unsigned char>(character) < 32) character = ' ';
            }
        }
        if (xmlStrEqual(node->name, BAD_CAST "description")) {
            result.description += text(xmlNodeGetContent(node));
            result.description += '\n';
        }
    }
    return result;
}

Config read_config(const std::filesystem::path& path, bool allow_empty_sources) {
    if (std::filesystem::file_size(path) > 1024 * 1024) throw std::runtime_error("Configuration exceeds 1 MiB");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open configuration: " + path.string());
    const std::string xml{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    auto document = parse(xml, "archive_catalog");
    auto* root = xmlDocGetRootElement(document.get());
    Config config;
    if (xmlHasProp(root, BAD_CAST "output_dir")) config.output_dir = std::filesystem::u8path(required_attribute(root, "output_dir"));
    if (xmlHasProp(root, BAD_CAST "download_dir")) config.download_dir = std::filesystem::u8path(required_attribute(root, "download_dir"));
    config.ca_bundle = attribute(root, "ca_bundle");
    auto follow = attribute(root, "follow_parts");
    if (!follow.empty() && follow != "true" && follow != "false") throw std::runtime_error("follow_parts must be true or false");
    config.follow_parts = follow != "false";
    auto automatic = attribute(root, "automatically_start_downloads");
    if (!automatic.empty() && automatic != "true" && automatic != "false") {
        throw std::runtime_error("automatically_start_downloads must be true or false");
    }
    config.automatically_start_downloads = automatic != "false";
    for (const auto& [name, target] : {std::pair<const char*, bool*>{"extract_archives", &config.extract_archives},
             {"delete_extracted_archives", &config.delete_extracted_archives}}) {
        const auto value = attribute(root, name);
        if (!value.empty() && value != "true" && value != "false") throw std::runtime_error(std::string(name) + " must be true or false");
        *target = value == "true";
    }
    auto debug = attribute(root, "debug_window");
    if (!debug.empty() && debug != "true" && debug != "false") {
        throw std::runtime_error("debug_window must be true or false");
    }
    config.debug_window = debug == "true";
    if (xmlHasProp(root, BAD_CAST "excluded_extensions")) config.excluded_extensions = parse_extension_list(attribute(root, "excluded_extensions"));
    for (auto* node = root->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE) continue;
        if (xmlStrEqual(node->name, BAD_CAST "source")) {
            auto url = required_attribute(node, "url");
            item_id(url);
            config.sources.push_back(url);
        } else if (xmlStrEqual(node->name, BAD_CAST "download")) {
            Selection selected{required_attribute(node, "item"), required_attribute(node, "name")};
            download_url({selected.item, selected.name, 0, {}, {}});
            config.downloads.push_back(selected);
        } else {
            throw std::runtime_error("Unknown configuration element: " + std::string(reinterpret_cast<const char*>(node->name)));
        }
    }
    if ((!allow_empty_sources && config.sources.empty()) || config.sources.size() > 128) throw std::runtime_error("Configure between 1 and 128 source URLs");
    return config;
}

Scan read_catalog(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) > 32 * 1024 * 1024) throw std::runtime_error("Catalog exceeds 32 MiB");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open catalog");
    const std::string xml{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    auto document = parse(xml, "catalog");
    auto* root = xmlDocGetRootElement(document.get());
    Scan scan;
    std::set<std::string> items;
    for (auto* node = root->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE) continue;
        if (xmlStrEqual(node->name, BAD_CAST "errors")) {
            for (auto* error = node->children; error; error = error->next) {
                if (error->type == XML_ELEMENT_NODE) scan.errors.push_back(text(xmlNodeGetContent(error)));
            }
        } else if (xmlStrEqual(node->name, BAD_CAST "item")) {
            items.insert(item_id("https://archive.org/details/" + required_attribute(node, "id")));
        } else if (xmlStrEqual(node->name, BAD_CAST "file")) {
            File file;
            file.item = required_attribute(node, "item");
            file.name = required_attribute(node, "name");
            download_url(file);
            auto size = required_attribute(node, "size");
            if (size.find_first_not_of("0123456789") != std::string::npos) throw std::runtime_error("Invalid catalog file size");
            file.size = std::stoull(size);
            if (!file.size) throw std::runtime_error("Empty catalog file");
            auto hash = [&](const char* name, std::size_t length) {
                auto value = attribute(node, name);
                if (!value.empty() && (value.size() != length || value.find_first_not_of("0123456789abcdef") != std::string::npos)) {
                    throw std::runtime_error("Invalid catalog checksum");
                }
                return value;
            };
            file.md5 = hash("md5", 32);
            file.sha1 = hash("sha1", 40);
            auto access = attribute(node, "restricted");
            if (access != "true" && access != "false") throw std::runtime_error("Catalog is missing a valid restricted flag; rescan the source");
            file.restricted = access == "true";
            items.insert(file.item);
            scan.files.push_back(std::move(file));
        }
    }
    auto complete = required_attribute(root, "complete");
    if (complete != "true" && complete != "false") throw std::runtime_error("Invalid catalog completeness flag");
    if (complete == "false" && scan.errors.empty()) scan.errors.push_back("Imported catalog is incomplete");
    scan.items.assign(items.begin(), items.end());
    std::vector<File> files;
    merge_files(files, scan.files);
    scan.files = std::move(files);
    return scan;
}

Scan scan_catalog(const Config& config, const Fetch& fetch) {
    if (config.sources.empty()) throw std::runtime_error("No source URLs configured");
    Scan scan;
    std::set<std::string> seen;
    auto add_item = [&](const std::string& item) {
        if (seen.count(item)) return;
        if (scan.items.size() >= 128) throw std::runtime_error("Scan exceeds the 128-item limit");
        seen.insert(item);
        scan.items.push_back(item);
    };
    std::set<std::string> source_items;
    for (const auto& source : config.sources) {
        auto item = item_id(source);
        if (!source_items.insert(item).second) continue;
        add_item(item);
        if (!config.follow_parts) continue;
        try {
            auto meta = fetch("https://archive.org/download/" + item + "/" + item + "_meta.xml");
            for (const auto& discovered : discover_items(item, read_item_metadata(meta).description)) add_item(discovered);
        } catch (const OperationCancelled&) {
            throw;
        } catch (const std::exception& error) {
            scan.errors.push_back(item + " (description): " + error.what());
        }
    }
    for (const auto& item : scan.items) {
        std::cout << "Scanning: " << item << std::endl;
        try {
            auto manifest = fetch("https://archive.org/download/" + item + "/" + item + "_files.xml");
            std::vector<std::string> skipped;
            merge_files(scan.files, parse_files(item, manifest, &skipped));
            if (!skipped.empty()) {
                std::string names;
                for (std::size_t index = 0; index < skipped.size() && index < 3; ++index) names += (index ? ", " : "") + skipped[index];
                if (skipped.size() > 3) names += " and " + std::to_string(skipped.size() - 3) + " more";
                scan.warnings.push_back(item + ": skipped " + std::to_string(skipped.size()) + " file" + (skipped.size() == 1 ? "" : "s") +
                    " with a name that cannot be saved safely: " + names);
                std::cerr << scan.warnings.back() << std::endl;
            }
        } catch (const OperationCancelled&) {
            throw;
        } catch (const std::exception& error) {
            scan.errors.push_back(item + " (files): " + error.what());
        }
    }
    return scan;
}

void save_catalog(const Scan& scan, const std::filesystem::path& path) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    Document document(xmlNewDoc(BAD_CAST "1.0"), xmlFreeDoc);
    if (!document) throw std::runtime_error("Cannot create catalog XML");
    auto* root = xmlNewNode(nullptr, BAD_CAST "catalog");
    if (!root) throw std::runtime_error("Cannot create catalog root");
    xmlDocSetRootElement(document.get(), root);
    property(root, "complete", scan.errors.empty() ? "true" : "false");
    property(root, "item_count", std::to_string(scan.items.size()));
    property(root, "file_count", std::to_string(scan.files.size()));
    auto* errors = xmlNewChild(root, nullptr, BAD_CAST "errors", nullptr);
    for (const auto& error : scan.errors) {
        auto* node = xmlNewChild(errors, nullptr, BAD_CAST "error", nullptr);
        xmlNodeSetContent(node, BAD_CAST "");
        xmlAddChild(node, xmlNewText(BAD_CAST error.c_str()));
    }
    for (const auto& item : scan.items) {
        item_id("https://archive.org/details/" + item);
        auto* node = xmlNewChild(root, nullptr, BAD_CAST "item", nullptr);
        property(node, "id", item);
    }
    for (const auto& file : scan.files) {
        auto url = download_url(file);
        auto* node = xmlNewChild(root, nullptr, BAD_CAST "file", nullptr);
        property(node, "item", file.item);
        property(node, "name", file.name);
        property(node, "size", std::to_string(file.size));
        property(node, "md5", file.md5);
        property(node, "sha1", file.sha1);
        property(node, "url", url);
        property(node, "restricted", file.restricted ? "true" : "false");
    }
    auto temporary = path;
    temporary += ".tmp";
    save_xml(document.get(), temporary);
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Cannot replace saved catalog: " + path.string());
    }
#else
    std::filesystem::rename(temporary, path);
#endif
}

void export_catalog(const Scan& scan, const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    std::ofstream table(directory / "catalog.csv", std::ios::binary | std::ios::trunc);
    std::ofstream urls(directory / "urls.txt", std::ios::binary | std::ios::trunc);
    if (!table || !urls) throw std::runtime_error("Cannot create catalog exports");
    table << "item,name,size_bytes,md5,sha1,url,restricted\r\n";
    for (const auto& file : scan.files) {
        auto url = download_url(file);
        table << csv(file.item) << ',' << csv(file.name) << ',' << file.size << ','
              << csv(file.md5) << ',' << csv(file.sha1) << ',' << csv(url) << ','
              << (file.restricted ? "true" : "false") << "\r\n";
        urls << url << '\n';
    }
    table.close();
    urls.close();
    if (!table || !urls) throw std::runtime_error("Cannot finish catalog exports");
    save_catalog(scan, directory / "catalog.xml");
}

std::vector<File> select_downloads(const Scan& scan, const std::vector<Selection>& selections, bool allow_incomplete) {
    if (!allow_incomplete && !scan.errors.empty()) throw std::runtime_error("Cannot download from an incomplete catalog");
    std::map<std::pair<std::string, std::string>, const File*> index;
    for (const auto& file : scan.files) index[{file.item, file.name}] = &file;
    std::vector<File> queue;
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto& selection : selections) {
        auto key = std::make_pair(selection.item, selection.name);
        auto found = index.find(key);
        if (found == index.end()) throw std::runtime_error("Selected file not in catalog: " + selection.item + "/" + selection.name);
        if (found->second->md5.empty() && found->second->sha1.empty()) throw std::runtime_error("Selected file has no checksum: " + selection.name);
        if (seen.insert(key).second) queue.push_back(*found->second);
    }
    return queue;
}

int run(const Config& config) {
    auto credentials = config.s3;
    if (credentials.access_key.empty() && credentials.secret_key.empty()) {
        if (const auto* value = std::getenv("IA_S3_ACCESS_KEY")) credentials.access_key = value;
        if (const auto* value = std::getenv("IA_S3_SECRET_KEY")) credentials.secret_key = value;
    }
    Http http(config.ca_bundle, {}, credentials);
    auto scan = scan_catalog(config, [&](const std::string& url) { return http.get(url); });
    scan.files.erase(std::remove_if(scan.files.begin(), scan.files.end(),
        [&](const File& file) { return excluded_file_type(file.name, config.excluded_extensions); }), scan.files.end());
    export_catalog(scan, config.output_dir);
    std::cout << "Catalog: " << scan.files.size() << " files across " << scan.items.size() << " items\n";
    auto restricted = std::count_if(scan.files.begin(), scan.files.end(), [](const File& file) { return file.restricted; });
    std::cout << "Restricted entries: " << restricted << " (listing does not confirm download permission)\n";
    if (!scan.errors.empty()) {
        for (const auto& error : scan.errors) std::cerr << error << '\n';
        std::cerr << "Catalog is incomplete. Download queue was not started.\n";
        return 2;
    }
    auto queue = select_downloads(scan, config.downloads);
    bool failed = false;
    for (const auto& file : queue) {
        std::cout << "Downloading: " << file.name << " (" << file.size << " bytes)" << std::endl;
        try { http.download(file, config.download_dir); }
        catch (const std::exception& error) {
            failed = true;
            std::cerr << "Download failed: " << file.name << ": " << error.what() << std::endl;
        }
    }
    return failed ? 3 : 0;
}

}