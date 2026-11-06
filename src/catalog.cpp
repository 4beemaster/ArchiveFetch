#include "catalog.hpp"

#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/uri.h>
#include <libxml/xpath.h>

#include <algorithm>
#include <climits>
#include <memory>
#include <set>
#include <stdexcept>

namespace archive_catalog {
namespace {

using Document = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
using Uri = std::unique_ptr<xmlURI, decltype(&xmlFreeURI)>;

std::string take_text(xmlChar* text) {
    if (!text) return {};
    std::string result(reinterpret_cast<const char*>(text));
    xmlFree(text);
    return result;
}

std::string child_text(xmlNode* parent, const char* name) {
    for (auto* child = parent->children; child; child = child->next) {
        if (child->type == XML_ELEMENT_NODE && xmlStrEqual(child->name, BAD_CAST name)) {
            return take_text(xmlNodeGetContent(child));
        }
    }
    return {};
}

std::string lower(std::string text) {
    for (auto& character : text) {
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    }
    return text;
}

bool valid_id(const std::string& item) {
    return !item.empty() && item.size() <= 256 && item != "." && item != ".." &&
           item.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == std::string::npos;
}

void require_id(const std::string& item) {
    if (!valid_id(item)) throw std::runtime_error("Invalid Archive.org item identifier");
}

std::string checksum(xmlNode* node, const char* name, std::size_t length) {
    auto value = lower(child_text(node, name));
    if (!value.empty() && (value.size() != length ||
        value.find_first_not_of("0123456789abcdef") != std::string::npos)) {
        throw std::runtime_error(std::string("Invalid ") + name + " checksum");
    }
    return value;
}

}

const std::vector<std::string>& default_excluded_extensions() {
    static const std::vector<std::string> extensions{".xml", ".torrent", ".sqlite", ".sqlite3", ".txt", ".log", ".jpg", ".png"};
    return extensions;
}

bool excluded_file_type(const std::string& name, const std::vector<std::string>& extensions) {
    const auto filename = name.substr(name.find_last_of('/') + 1);
    const auto position = filename.find_last_of('.');
    if (position == std::string::npos || position == 0) return false;
    return std::find(extensions.begin(), extensions.end(), lower(filename.substr(position))) != extensions.end();
}

std::vector<std::string> parse_extension_list(const std::string& text) {
    std::vector<std::string> extensions;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = std::min(text.find_first_of(" \t\r\n,;", start), text.size());
        const auto typed = text.substr(start, end - start);
        auto entry = lower(typed);
        start = end + 1;
        if (entry.empty()) continue;
        if (entry.front() != '.') entry.insert(entry.begin(), '.');
        if (entry.size() < 2 || entry.size() > 32 || entry.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-~", 1) != std::string::npos)
            throw std::runtime_error("Hidden file types look like .txt (letters, digits, _ - ~), one extension each: " + typed);
        if (std::find(extensions.begin(), extensions.end(), entry) == extensions.end()) extensions.push_back(entry);
        if (extensions.size() > 64) throw std::runtime_error("Hide at most 64 file types");
    }
    return extensions;
}

std::string format_extension_list(const std::vector<std::string>& extensions) {
    std::string text;
    for (const auto& extension : extensions) text += (text.empty() ? "" : ", ") + extension;
    return text;
}

std::string archive_folder_name(const std::string& filename) {
    const auto name = filename.substr(filename.find_last_of('/') + 1);
    const auto lowered = lower(name);
    const auto extension = lowered.substr(lowered.find_last_of('.') + 1);
    const auto digits = [](const std::string& text) { return !text.empty() && text.find_first_not_of("0123456789") == std::string::npos; };
    if (lowered.find('.') == std::string::npos || digits(extension) ||
        (extension.size() == 3 && (extension[0] == 'r' || extension[0] == 'z') && digits(extension.substr(1)))) return {};
    if (extension == "rar") {
        const auto stem = lowered.substr(0, lowered.size() - 4);
        const auto part = stem.substr(stem.find_last_of('.') + 1);
        if (stem.find('.') != std::string::npos && part.rfind("part", 0) == 0 && digits(part.substr(4))) return {};
    }
    for (const char* suffix : {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tgz", ".tbz2", ".tbz", ".txz", ".tzst", ".tar", ".zip", ".7z", ".rar"}) {
        const std::string ending = suffix;
        if (lowered.size() <= ending.size() || lowered.compare(lowered.size() - ending.size(), ending.size(), ending) != 0) continue;
        auto folder = name.substr(0, name.size() - ending.size());
        while (!folder.empty() && (folder.back() == ' ' || folder.back() == '.')) folder.pop_back();
        return folder;
    }
    return {};
}

std::string item_id(const std::string& url) {
    Uri uri(xmlParseURI(url.c_str()), xmlFreeURI);
    if (!uri || !uri->scheme || lower(uri->scheme) != "https" || !uri->server || uri->user ||
        (uri->port != 0 && uri->port != 443)) {
        throw std::runtime_error("Expected an HTTPS Archive.org item or files XML URL");
    }
    auto host = lower(uri->server);
    if (host != "archive.org" && (host.size() <= 12 || host.substr(host.size() - 12) != ".archive.org")) {
        throw std::runtime_error("URL must belong to archive.org");
    }
    std::string path = uri->path ? uri->path : "";
    const std::string prefix = path.rfind("/details/", 0) == 0 ? "/details/" : "/download/";
    std::string item;
    if (path.rfind(prefix, 0) == 0) {
        auto remainder = path.substr(prefix.size());
        item = remainder.substr(0, remainder.find('/'));
    } else {
        auto location = path.find("/items/");
        if (location != std::string::npos) {
            auto remainder = path.substr(location + 7);
            item = remainder.substr(0, remainder.find('/'));
        }
    }
    require_id(item);
    return item;
}

std::string encode_path(const std::string& path) {
    const char* hexadecimal = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char character : path) {
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_' ||
            character == '.' || character == '~' || character == '/') {
            encoded += static_cast<char>(character);
        } else {
            encoded += '%';
            encoded += hexadecimal[character >> 4];
            encoded += hexadecimal[character & 15];
        }
    }
    return encoded;
}

bool safe_relative_path(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.find_first_of("\\:<>\"|?*") != std::string::npos) return false;
    if (std::any_of(path.begin(), path.end(), [](unsigned char character) { return character < 32 || character == 127; })) return false;
    std::size_t start = 0;
    while (start < path.size()) {
        auto end = path.find('/', start);
        auto component = path.substr(start, end == std::string::npos ? end : end - start);
        if (component.empty() || component == "." || component == ".." ||
            component.back() == '.' || component.back() == ' ') return false;
        auto stem = lower(component.substr(0, component.find('.')));
        if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
            (stem.size() == 4 && (stem.substr(0, 3) == "com" || stem.substr(0, 3) == "lpt") &&
             stem[3] >= '1' && stem[3] <= '9')) return false;
        if (end == std::string::npos) return true;
        start = end + 1;
    }
    return false;
}

std::string download_url(const File& file) {
    require_id(file.item);
    if (!safe_relative_path(file.name)) throw std::runtime_error("Unsafe filename: " + file.name);
    return "https://archive.org/download/" + file.item + "/" + encode_path(file.name);
}

std::vector<std::string> discover_items(const std::string& item, const std::string& description) {
    require_id(item);
    std::vector<std::string> items{item};
    if (description.empty()) return items;
    if (description.size() > INT_MAX) throw std::runtime_error("Description is too large");
    Document document(htmlReadMemory(description.data(), static_cast<int>(description.size()),
                      nullptr, "UTF-8", HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING), xmlFreeDoc);
    if (!document) throw std::runtime_error("Cannot parse item description");
    std::unique_ptr<xmlXPathContext, decltype(&xmlXPathFreeContext)> context(xmlXPathNewContext(document.get()), xmlXPathFreeContext);
    if (!context) throw std::runtime_error("Cannot allocate HTML parser context");
    std::unique_ptr<xmlXPathObject, decltype(&xmlXPathFreeObject)> links(xmlXPathEvalExpression(BAD_CAST "//a/@href", context.get()), xmlXPathFreeObject);
    if (!links) throw std::runtime_error("Cannot read description links");
    const auto part = item.rfind("-part-");
    const auto family = part == std::string::npos ? "" : lower(item.substr(0, part + 6));
    std::set<std::string> seen{item};
    auto* nodes = links->nodesetval;
    for (int index = 0; nodes && index < nodes->nodeNr; ++index) {
        auto href = take_text(xmlNodeGetContent(nodes->nodeTab[index]));
        auto absolute = take_text(xmlBuildURI(BAD_CAST href.c_str(), BAD_CAST "https://archive.org/"));
        std::string candidate;
        try { candidate = item_id(absolute); } catch (const std::exception&) { continue; }
        if (!family.empty() && lower(candidate).rfind(family, 0) != 0) continue;
        if (seen.insert(candidate).second) items.push_back(candidate);
        if (items.size() > 128) throw std::runtime_error("Description exceeds the 128-item limit");
    }
    return items;
}

std::vector<File> parse_files(const std::string& item, const std::string& xml, std::vector<std::string>* skipped) {
    require_id(item);
    if (xml.size() > INT_MAX) throw std::runtime_error("Files XML is too large");
    Document document(xmlReadMemory(xml.data(), static_cast<int>(xml.size()), nullptr, "UTF-8",
                                   XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING), xmlFreeDoc);
    if (!document || document->intSubset || document->extSubset) throw std::runtime_error("Invalid files XML or forbidden DTD");
    auto* root = xmlDocGetRootElement(document.get());
    if (!root || !xmlStrEqual(root->name, BAD_CAST "files")) throw std::runtime_error("Expected a files XML document");
    std::vector<File> files;
    for (auto* node = root->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE || !xmlStrEqual(node->name, BAD_CAST "file")) continue;
        auto name = take_text(xmlGetProp(node, BAD_CAST "name"));
        // archive.org keeps replaced or deleted files here as "history/files/<name>.~N~".
        if (name.rfind("history/files/", 0) == 0) continue;
        auto private_flag = lower(child_text(node, "private"));
        if (private_flag.empty()) private_flag = lower(take_text(xmlGetProp(node, BAD_CAST "private")));
        bool restricted = !private_flag.empty() && private_flag != "false" && private_flag != "0";
        if (!safe_relative_path(name)) {
            if (!skipped) throw std::runtime_error("Unsafe filename: " + name);
            skipped->push_back(name);
            continue;
        }
        auto size = child_text(node, "size");
        // The item's own _files.xml is generated on request and listed without a size; it cannot be verified, so leave it out.
        if (size.empty()) continue;
        if (size.find_first_not_of("0123456789") != std::string::npos) throw std::runtime_error("Invalid size for " + name);
        auto bytes = std::stoull(size);
        if (!bytes) {
            if (!skipped) throw std::runtime_error("Empty file: " + name);
            continue;
        }
        files.push_back({item, name, bytes, checksum(node, "md5", 32), checksum(node, "sha1", 40), restricted});
    }
    return files;
}

void merge_files(std::vector<File>& catalog, const std::vector<File>& incoming) {
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto& file : catalog) seen.emplace(file.item, file.name);
    for (const auto& file : incoming) {
        if (seen.emplace(file.item, file.name).second) catalog.push_back(file);
    }
    std::sort(catalog.begin(), catalog.end(), [](const File& first, const File& second) {
        return std::make_pair(first.name, first.item) < std::make_pair(second.name, second.item);
    });
}

}