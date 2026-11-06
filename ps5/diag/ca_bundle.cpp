#include "ca_bundle.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include "diag_log.hpp"

asm(".pushsection .rodata\n"
    ".global diag_ca_bundle_begin\n"
    "diag_ca_bundle_begin:\n"
    ".incbin \"" DIAG_CA_BUNDLE "\"\n"
    ".global diag_ca_bundle_end\n"
    "diag_ca_bundle_end:\n"
    ".popsection\n");
extern "C" const unsigned char diag_ca_bundle_begin[];
extern "C" const unsigned char diag_ca_bundle_end[];

namespace archivefetch_diag {
namespace {

bool same_contents(const std::string& path, const char* data, std::size_t size) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    const std::string existing((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return existing.size() == size && std::memcmp(existing.data(), data, size) == 0;
}

bool write_atomic(const std::string& path, const char* data, std::size_t size) {
    const std::string temporary = path + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(data, static_cast<std::streamsize>(size));
        output.close();
        if (!output) {
            std::remove(temporary.c_str());
            return false;
        }
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

long long count_certificates(const char* data, std::size_t size) {
    static const char marker[] = "-----BEGIN CERTIFICATE-----";
    long long count = 0;
    const std::string text(data, size);
    for (std::size_t at = text.find(marker); at != std::string::npos; at = text.find(marker, at + 1)) ++count;
    return count;
}

// Number of certificates OpenSSL actually parsed from the file, or -1 if it could not load it.
int openssl_certificates(const std::string& path) {
    X509_STORE* store = X509_STORE_new();
    if (!store) return -1;
    const int loaded = X509_STORE_load_file(store, path.c_str());
    const int count = loaded == 1 ? sk_X509_OBJECT_num(X509_STORE_get0_objects(store)) : -1;
    X509_STORE_free(store);
    return count;
}

}

std::string install_ca_bundle(const char* data_root) {
    const char* data = reinterpret_cast<const char*>(diag_ca_bundle_begin);
    const auto size = static_cast<std::size_t>(diag_ca_bundle_end - diag_ca_bundle_begin);
    const std::string path = std::string(data_root) + "/cacert.pem";
    const long long embedded = count_certificates(data, size);

    const char* action = "unchanged";
    if (!same_contents(path, data, size)) {
        if (!write_atomic(path, data, size)) {
            const int error = errno;
            log_event("CA_BUNDLE_FAILED", {field("path", path.c_str()), field("errno", error),
                field("error", std::strerror(error))});
            return {};
        }
        action = "written";
    }
    const int parsed = openssl_certificates(path);
    log_event("CA_BUNDLE", {field("action", action), field("path", path.c_str()), field("bytes", static_cast<long long>(size)),
        field("mozilla_date", DIAG_CA_BUNDLE_DATE), field("certificates", embedded), field("openssl_parsed", parsed)});
    return parsed > 0 ? path : std::string();
}

}
