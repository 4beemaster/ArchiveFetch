#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "extraction.hpp"

namespace archivefetch_diag {

// Extracts through ArchFtchX.elf, started via the console's own ELF loader, so decompression never runs in ArchiveFetch.
archive_catalog::ExtractOutcome run_extraction(const std::filesystem::path& archive, const std::filesystem::path& folder,
    const std::function<bool()>& cancelled, const std::function<void(const std::string&)>& status,
    const std::function<void(std::uint64_t, std::uint64_t)>& progress);

// Sends an ELF file to the console's own loader on 127.0.0.1; on failure, error says why.
bool send_to_local_loader(const std::filesystem::path& elf, std::string& error);

}
