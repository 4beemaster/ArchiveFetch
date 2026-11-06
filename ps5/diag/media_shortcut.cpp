#include "media_shortcut.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <ps5/kernel.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diag_config.hpp"
#include "diag_log.hpp"

asm(".pushsection .rodata\n"
    ".global diag_icon_begin\n"
    "diag_icon_begin:\n"
    ".incbin \"icon0.png\"\n"
    ".global diag_icon_end\n"
    "diag_icon_end:\n"
    ".popsection\n");
extern "C" const unsigned char diag_icon_begin[];
extern "C" const unsigned char diag_icon_end[];

extern "C" int sceAppInstUtilInitialize();
extern "C" int sceAppInstUtilAppInstallAll(void* options);

namespace archivefetch_diag {

static const char* presence(const void* address) {
    return address ? "found" : "missing";
}

static bool ensure_directory(const char* path) {
    if (mkdir(path, 0755) == 0) return true;
    const int error = errno;
    struct stat st{};
    if (error == EEXIST && lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    log_event("MEDIA_DIRECTORY_FAILED", {field("path", path), field("errno", error),
        field("error", std::strerror(error))});
    return false;
}

// Returns 1 if the file holds exactly these bytes, 0 if it differs or is missing, -1 if not a regular file.
static int compare_file(const char* path, const void* data, size_t size) {
    struct stat st{};
    if (lstat(path, &st) != 0) return 0;
    if (!S_ISREG(st.st_mode)) return -1;
    if (static_cast<size_t>(st.st_size) != size) return 0;
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    const auto* expected = static_cast<const unsigned char*>(data);
    unsigned char buffer[4096];
    size_t offset = 0;
    int same = 1;
    while (offset < size) {
        const ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got <= 0 || offset + static_cast<size_t>(got) > size ||
            std::memcmp(buffer, expected + offset, static_cast<size_t>(got)) != 0) {
            same = 0;
            break;
        }
        offset += static_cast<size_t>(got);
    }
    close(fd);
    return same;
}

static bool write_file(const char* path, const void* data, size_t size) {
    const int existing = compare_file(path, data, size);
    if (existing < 0) {
        log_event("MEDIA_FILE_UNSAFE", {field("path", path)});
        return false;
    }
    if (existing == 1) {
        log_event("MEDIA_FILE_UNCHANGED", {field("path", path), field("bytes", static_cast<long long>(size))});
        return true;
    }

    char temp[256];
    std::snprintf(temp, sizeof(temp), "%s.tmp", path);
    const int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (fd < 0) {
        const int error = errno;
        log_event("MEDIA_FILE_OPEN_FAILED", {field("path", temp), field("errno", error),
            field("error", std::strerror(error))});
        return false;
    }
    const auto* bytes = static_cast<const char*>(data);
    size_t offset = 0;
    while (offset < size) {
        const ssize_t written = write(fd, bytes + offset, size - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) break;
        offset += static_cast<size_t>(written);
    }
    const int write_error = offset == size ? 0 : (errno ? errno : EIO);
    const bool synced = fsync(fd) == 0;
    close(fd);
    if (write_error || !synced || rename(temp, path) != 0) {
        const int error = write_error ? write_error : errno;
        unlink(temp);
        log_event("MEDIA_FILE_WRITE_FAILED", {field("path", path), field("errno", error),
            field("error", std::strerror(error))});
        return false;
    }
    log_event("MEDIA_FILE_WRITTEN", {field("path", path), field("bytes", static_cast<long long>(size))});
    return true;
}

// AppInstallAll registers every staged folder, so record which of ours are present.
static void log_staged_titles() {
    char titles[512] = "";
    size_t used = 0;
    if (DIR* directory = opendir("/user/app")) {
        while (const dirent* entry = readdir(directory)) {
            if (std::strncmp(entry->d_name, "ARFT", 4) != 0) continue;
            const int added = std::snprintf(titles + used, sizeof(titles) - used, "%s%s",
                used ? "," : "", entry->d_name);
            if (added < 0 || static_cast<size_t>(added) >= sizeof(titles) - used) break;
            used += static_cast<size_t>(added);
        }
        closedir(directory);
    }
    log_event("MEDIA_STAGED_ARCHIVEFETCH_TITLES", {field("titles", titles)});
}

InstallerExports resolve_installer() {
    InstallerExports exports{};

    // Linked imports: the loader resolved these before main, so reaching here means the sprx loaded.
    exports.initialize = sceAppInstUtilInitialize;
    exports.install_all = sceAppInstUtilAppInstallAll;
    log_event("MEDIA_LINKED_IMPORTS", {
        field("sceAppInstUtilInitialize", presence(reinterpret_cast<void*>(exports.initialize))),
        field("sceAppInstUtilAppInstallAll", presence(reinterpret_cast<void*>(exports.install_all)))});

    exports.title_dir_by_symbol = reinterpret_cast<InstallTitleDir>(
        dlsym(RTLD_DEFAULT, "sceAppInstUtilAppInstallTitleDir"));
    log_event("MEDIA_TITLE_DIR_DLSYM", {
        field("result", presence(reinterpret_cast<void*>(exports.title_dir_by_symbol)))});

    std::uint32_t handle = 0;
    exports.module_handle_result = kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle);
    exports.module_handle = handle;
    log_event("MEDIA_MODULE_HANDLE", {field_hex("result", exports.module_handle_result),
        field_hex("handle", static_cast<int>(handle))});

    if (exports.module_handle_result == 0) {
        exports.title_dir_by_nid = reinterpret_cast<InstallTitleDir>(
            kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE"));
        exports.title_dir_by_handle_symbol = reinterpret_cast<InstallTitleDir>(
            kernel_dynlib_dlsym(-1, handle, "sceAppInstUtilAppInstallTitleDir"));
    }
    log_event("MEDIA_TITLE_DIR_MODULE", {
        field("by_nid", presence(reinterpret_cast<void*>(exports.title_dir_by_nid))),
        field("by_symbol", presence(reinterpret_cast<void*>(exports.title_dir_by_handle_symbol)))});
    return exports;
}

bool stage_shortcut() {
    char app_directory[64];
    char sce_sys[96];
    char appmeta[64];
    std::snprintf(app_directory, sizeof(app_directory), "/user/app/%s", title_id);
    std::snprintf(sce_sys, sizeof(sce_sys), "%s/sce_sys", app_directory);
    std::snprintf(appmeta, sizeof(appmeta), "/user/appmeta/%s", title_id);

    log_event("MEDIA_STAGE_BEGIN", {field("path", app_directory), field("appmeta", appmeta)});
    if (!ensure_directory(app_directory) || !ensure_directory(sce_sys) ||
        !ensure_directory("/user/appmeta") || !ensure_directory(appmeta))
        return false;

    char metadata[512];
    const int length = std::snprintf(metadata, sizeof(metadata),
        "{\n"
        "  \"applicationCategoryType\": 65536,\n"
        "  \"titleId\": \"%s\",\n"
        "  \"deeplinkUri\": \"http://127.0.0.1:%d/\",\n"
        "  \"localizedParameters\": {\n"
        "    \"defaultLanguage\": \"en-US\",\n"
        "    \"en-US\": { \"titleName\": \"ArchiveFetch\" }\n"
        "  }\n"
        "}\n", title_id, http_port);
    const auto icon_size = static_cast<size_t>(diag_icon_end - diag_icon_begin);

    // ps5upload and ShadowMount+ refresh the appmeta copy too, which the home screen reads.
    for (const char* directory : {static_cast<const char*>(sce_sys), static_cast<const char*>(appmeta)}) {
        char param_path[128];
        char icon_path[128];
        std::snprintf(param_path, sizeof(param_path), "%s/param.json", directory);
        std::snprintf(icon_path, sizeof(icon_path), "%s/icon0.png", directory);
        if (!write_file(param_path, metadata, static_cast<size_t>(length)) ||
            !write_file(icon_path, diag_icon_begin, icon_size))
            return false;
    }

    log_event("MEDIA_STAGE_END", {field("path", app_directory), field("http_port", http_port)});
    return true;
}

bool register_shortcut(const InstallerExports& exports) {
    log_staged_titles();

    log_event("MEDIA_INITIALIZE_BEGIN");
    const int initialized = exports.initialize();
    log_event("MEDIA_INITIALIZE_END", {field_hex("result", initialized)});
    if (initialized != 0) return false;

    InstallTitleDir title_dir = exports.title_dir_by_nid;
    if (!title_dir) title_dir = exports.title_dir_by_symbol;
    if (!title_dir) title_dir = exports.title_dir_by_handle_symbol;
    const char* method = title_dir ? "sceAppInstUtilAppInstallTitleDir" : "sceAppInstUtilAppInstallAll";

    log_event("MEDIA_INSTALL_BEGIN", {field("method", method), field("title_id", title_id)});
    const int result = title_dir ? title_dir(title_id, "/user/app/", nullptr) : exports.install_all(nullptr);
    const bool success = result == 0 || static_cast<std::uint32_t>(result) == 0x80990002u;
    log_event("MEDIA_INSTALL_END", {field("method", method), field_hex("result", result),
        field("success", success ? "true" : "false")});
    return success;
}

}
