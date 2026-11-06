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

}
