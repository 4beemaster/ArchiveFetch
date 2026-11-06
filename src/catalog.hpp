#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace archive_catalog {

struct File {
    std::string item;
    std::string name;
    std::uint64_t size;
    std::string md5;
    std::string sha1;
    bool restricted = false;
};

std::string item_id(const std::string& url);
std::string encode_path(const std::string& path);
std::string download_url(const File& file);
bool safe_relative_path(const std::string& path);
// Extensions are lowercase ".ext" and match the last extension of the filename, ignoring case.
const std::vector<std::string>& default_excluded_extensions();
bool excluded_file_type(const std::string& name, const std::vector<std::string>& extensions);
// Reads ".xml, torrent .JPG" style text (spaces, commas or semicolons); throws on an invalid entry.
std::vector<std::string> parse_extension_list(const std::string& text);
std::string format_extension_list(const std::vector<std::string>& extensions);
// Folder name for a single-file archive (zip, 7z, rar, tar and compressed tar); empty for anything else,
// including the parts of a multi-part set, which are never extracted.
std::string archive_folder_name(const std::string& filename);
std::vector<std::string> discover_items(const std::string& item,
                                      const std::string& description);
// With `skipped`, unsafe names are listed there and left out instead of failing the whole item.
std::vector<File> parse_files(const std::string& item, const std::string& xml, std::vector<std::string>* skipped = nullptr);
void merge_files(std::vector<File>& catalog, const std::vector<File>& incoming);

}