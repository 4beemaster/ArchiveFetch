#include "application.hpp"

#include <cstdio>
#include <iostream>
#include <stdexcept>

using namespace archive_catalog;

int main(int argc, char** argv) {
    try {
#ifdef PS5_PAYLOAD
        std::filesystem::create_directories("/data/archive-catalog");
        if (!std::freopen("/data/archive-catalog/run.log", "a", stdout) ||
            !std::freopen("/data/archive-catalog/run.log", "a", stderr)) return 1;
        std::filesystem::current_path("/data/archive-catalog");
#endif
        Config config;
        if (argc == 1) return run(read_config("config.xml"));
        bool configured = false;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--help") {
                std::cout << "ArchiveFetch\n"
                          << "  archive_catalog --config config.xml\n"
                          << "  archive_catalog --url HTTPS_URL [--output DIRECTORY] [--single-item]\n"
                          << "  No arguments: read config.xml (PS5: /data/archive-catalog/config.xml).\n"
                          << "  Downloads require explicit <download> entries in the configuration.\n";
                return 0;
            }
            if (argument == "--single-item") { config.follow_parts = false; continue; }
            if (index + 1 >= argc) throw std::runtime_error("Missing value for " + argument);
            std::string value = argv[++index];
            if (argument == "--config") {
                if (configured || argc != 3) throw std::runtime_error("Use --config alone");
                config = read_config(std::filesystem::u8path(value));
                configured = true;
            } else if (argument == "--url") {
                item_id(value);
                config.sources.push_back(value);
            } else if (argument == "--output") {
                config.output_dir = std::filesystem::u8path(value);
            } else {
                throw std::runtime_error("Unknown option: " + argument);
            }
        }
        return run(config);
    } catch (const std::exception& error) {
        std::cerr << "ArchiveFetch: " << error.what() << std::endl;
        return 1;
    }
}