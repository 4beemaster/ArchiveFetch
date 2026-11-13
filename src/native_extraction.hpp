#pragma once

#include "extraction.hpp"
#include "extract_engine.hpp"
#include <algorithm>

namespace archive_catalog {

inline ExtractOutcome native_extract(const std::filesystem::path& archive, const std::filesystem::path& folder,
    const std::function<bool()>& cancelled, const std::function<void(const std::string&)>& status,
    const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
    ExtractOutcome outcome;
    std::error_code failure;
    const auto size = std::filesystem::file_size(archive, failure);
    if (failure) {
        outcome.error = "cannot read archive size: " + failure.message();
        return outcome;
    }
    archivefetch_extract::Options options;
    options.archive = archive.u8string();
    options.destination = folder.u8string();
    options.cancelled = cancelled;
    options.started = [&] { if (status) status("Extracting archive"); };
    options.progress = [&](const archivefetch_extract::Progress& state) {
        if (progress) progress(std::min<std::uint64_t>(state.consumed, size), size);
    };
    archivefetch_extract::Progress state;
    const auto result = archivefetch_extract::extract_archive(options, state, outcome.error);
    switch (result) {
    case archivefetch_extract::Result::Done: outcome.result = ExtractOutcome::Result::Done; break;
    case archivefetch_extract::Result::Failed: outcome.result = ExtractOutcome::Result::Failed; break;
    case archivefetch_extract::Result::Cancelled: outcome.result = ExtractOutcome::Result::Cancelled; break;
    }
    outcome.files = state.files;
    outcome.bytes = state.bytes;
    outcome.unsafe = state.unsafe;
    outcome.links = state.links;
    if (result == archivefetch_extract::Result::Done && progress) progress(size, size);
    return outcome;
}

}
