#pragma once

#include <cstddef>

namespace archivefetch_diag {

struct WebAsset {
    const unsigned char* data;
    std::size_t size;
    const char* type;
};

// Exact-match lookup of an embedded UI file by URL path; "/" maps to /index.html.
bool find_web_asset(const char* path, WebAsset* out);

std::size_t web_asset_count();
std::size_t web_asset_bytes();

}
