#pragma once

#include <string>

namespace archivefetch_diag {

// Writes the embedded Mozilla CA bundle to <data_root>/cacert.pem when it differs and checks OpenSSL can
// load it; returns the path, or an empty string if it cannot be used.
std::string install_ca_bundle(const char* data_root);

}
