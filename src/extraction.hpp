#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace archive_catalog {

struct ExtractOutcome {
    enum class Result { Done, Failed, Cancelled } result = Result::Failed;
    std::string error;
    std::uint64_t files = 0, bytes = 0, unsafe = 0, links = 0;
};

// Unpacks archive into folder, which must not exist yet; after a failure or a stop the folder is gone again.
// progress receives the archive bytes read so far and the archive's size.
using Extractor = std::function<ExtractOutcome(const std::filesystem::path& archive, const std::filesystem::path& folder,
    const std::function<bool()>& cancelled, const std::function<void(const std::string&)>& status,
    const std::function<void(std::uint64_t, std::uint64_t)>& progress)>;

}
