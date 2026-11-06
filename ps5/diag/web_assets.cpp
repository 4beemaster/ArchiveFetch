#include "web_assets.hpp"

#include <cstring>

#include "ps5_assets.hpp"

namespace archivefetch_diag {

bool find_web_asset(const char* path, WebAsset* out) {
    if (std::strcmp(path, "/") == 0) path = "/index.html";
    for (const EmbeddedAsset& asset : embedded_assets) {
        if (std::strcmp(asset.path, path) == 0) {
            *out = {asset.data, asset.size, asset.type};
            return true;
        }
    }
    return false;
}

std::size_t web_asset_count() {
    return sizeof(embedded_assets) / sizeof(embedded_assets[0]);
}

std::size_t web_asset_bytes() {
    std::size_t total = 0;
    for (const EmbeddedAsset& asset : embedded_assets) total += asset.size;
    return total;
}

}
