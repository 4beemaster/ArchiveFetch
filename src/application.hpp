#pragma once

#include "catalog.hpp"
#include "transfer.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace archive_catalog {

struct Selection {
    std::string item;
    std::string name;
};

std::filesystem::path default_download_directory();

struct Config {
    std::vector<std::string> sources;
    std::filesystem::path output_dir = "catalog-output";
    std::filesystem::path download_dir = default_download_directory();
    std::string ca_bundle;
    S3Credentials s3;
    bool follow_parts = true;
    bool automatically_start_downloads = true;
    bool extract_archives = false;
    bool delete_extracted_archives = false;
    // Hides files of these types from the Library unless they are downloaded; the catalog keeps them.
    std::vector<std::string> excluded_extensions = default_excluded_extensions();
    bool debug_window = false;
    std::vector<Selection> downloads;
};

struct Scan {
    std::vector<std::string> items;
    std::vector<File> files;
    std::vector<std::string> errors;
    std::vector<std::string> warnings; // not saved; files that were left out of an otherwise good scan
};

using Fetch = std::function<std::string(const std::string&)>;

struct ItemMetadata {
    std::string title;
    std::string description;
};

ItemMetadata read_item_metadata(const std::string& xml);

Config read_config(const std::filesystem::path& path, bool allow_empty_sources = false);
Scan read_catalog(const std::filesystem::path& path);
Scan scan_catalog(const Config& config, const Fetch& fetch);
void save_catalog(const Scan& scan, const std::filesystem::path& path);
void export_catalog(const Scan& scan, const std::filesystem::path& directory);
std::vector<File> select_downloads(const Scan& scan, const std::vector<Selection>& selections, bool allow_incomplete = false);
int run(const Config& config);

}