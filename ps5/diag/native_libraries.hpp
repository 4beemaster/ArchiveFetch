#pragma once

namespace archivefetch_diag {

// Initializes OpenSSL, curl and libxml2 once and logs versions and CA paths; makes no network requests.
bool init_native_libraries();

}
