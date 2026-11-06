#include "native_libraries.hpp"

#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/xmlversion.h>
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include <sys/stat.h>

#include "diag_log.hpp"

namespace archivefetch_diag {

static const char* text_or_empty(const char* text) {
    return text ? text : "";
}

static const char* path_exists(const char* path) {
    struct stat st{};
    return path && *path && stat(path, &st) == 0 ? "true" : "false";
}

bool init_native_libraries() {
    log_event("OPENSSL_INIT_BEGIN");
    const int ssl_result = OPENSSL_init_ssl(0, nullptr);
    log_event("OPENSSL_INIT_END", {field("result", ssl_result), field("version", OpenSSL_version(OPENSSL_VERSION)),
        field("dir", OpenSSL_version(OPENSSL_DIR))});

    log_event("CURL_INIT_BEGIN");
    const CURLcode curl_result = curl_global_init(CURL_GLOBAL_DEFAULT);
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    log_event("CURL_INIT_END", {field("result", static_cast<int>(curl_result)),
        field("error", curl_easy_strerror(curl_result)), field("version", text_or_empty(info->version)),
        field("tls", text_or_empty(info->ssl_version))});
    // An empty CA setting makes curl use these compiled-in defaults for every HTTPS request.
    log_event("CURL_CA_DEFAULTS", {field("cainfo", text_or_empty(info->cainfo)),
        field("cainfo_exists", path_exists(info->cainfo)), field("capath", text_or_empty(info->capath)),
        field("capath_exists", path_exists(info->capath))});

    log_event("LIBXML_INIT_BEGIN");
    xmlInitParser();
    log_event("LIBXML_INIT_END", {field("version", LIBXML_DOTTED_VERSION)});

    return ssl_result == 1 && curl_result == CURLE_OK;
}

}
