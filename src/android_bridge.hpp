#pragma once
#include <jni.h>
#include <curl/curl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/evp.h>
#include "native_extraction.hpp"

namespace {
std::mutex android_mutex;
std::unique_ptr<DiagnosticLog> android_log;
std::unique_ptr<WebApplication> android_app;
std::unique_ptr<ConsoleLink> android_consoles;
std::string android_session;
std::filesystem::path android_root;
std::filesystem::path android_downloads;
std::filesystem::path android_ca;
thread_local JNIEnv* android_environment = nullptr;
jclass android_bridge_class = nullptr;
JavaVM* android_vm = nullptr;
thread_local const TransferHooks* android_copy_hooks = nullptr;
thread_local bool android_overwrite = false;

class AndroidThread {
public:
    AndroidThread() {
        if (!android_vm) throw std::runtime_error("Android runtime unavailable");
        if (android_vm->GetEnv(reinterpret_cast<void**>(&android_environment), JNI_VERSION_1_6) == JNI_EDETACHED) {
            if (android_vm->AttachCurrentThread(&android_environment, nullptr) != JNI_OK) throw std::runtime_error("Cannot attach download worker");
            attached_ = true;
        }
    }
    ~AndroidThread() { if (attached_) { android_vm->DetachCurrentThread(); android_environment = nullptr; } }
private:
    bool attached_ = false;
};

std::string java_text(JNIEnv* environment, jstring value) {
    if (!value) return {};
    auto string_type = environment->FindClass("java/lang/String");
    auto encode = environment->GetMethodID(string_type, "getBytes", "(Ljava/lang/String;)[B");
    auto charset = environment->NewStringUTF("UTF-8");
    auto bytes = static_cast<jbyteArray>(environment->CallObjectMethod(value, encode, charset));
    if (environment->ExceptionCheck() || !bytes) { environment->ExceptionClear(); throw std::runtime_error("Cannot read Android message"); }
    std::string result(static_cast<std::size_t>(environment->GetArrayLength(bytes)), '\0');
    environment->GetByteArrayRegion(bytes, 0, static_cast<jsize>(result.size()), reinterpret_cast<jbyte*>(result.data()));
    environment->DeleteLocalRef(bytes); environment->DeleteLocalRef(charset); environment->DeleteLocalRef(string_type);
    return result;
}

std::string credential_call(const char* method, const std::string* value = nullptr) {
    auto* environment = android_environment;
    if (!environment || !android_bridge_class) throw std::runtime_error("Android credential context unavailable");
    auto identifier = environment->GetStaticMethodID(android_bridge_class, method,
        value ? "(Ljava/lang/String;)Ljava/lang/String;" : "()Ljava/lang/String;");
    if (!identifier) { environment->ExceptionClear(); throw std::runtime_error("Android credential adapter unavailable"); }
    jstring argument = value ? environment->NewStringUTF(value->c_str()) : nullptr;
    auto response = static_cast<jstring>(value ? environment->CallStaticObjectMethod(android_bridge_class, identifier, argument)
        : environment->CallStaticObjectMethod(android_bridge_class, identifier));
    if (argument) environment->DeleteLocalRef(argument);
    if (environment->ExceptionCheck()) { environment->ExceptionClear(); throw std::runtime_error("Android Keystore operation failed; saved credentials were not changed"); }
    auto result = java_text(environment, response);
    if (response) environment->DeleteLocalRef(response);
    return result;
}

Json android_storage(const Json& request) {
    AndroidThread thread;
    auto text = request.dump(-1, ' ', true);
    auto result = Json::parse(credential_call("storageRequest", &text));
    const bool copy_status = request.at("action") == "status" && result.contains("path") && result.contains("status") &&
        (result.at("status") == "Location unavailable" || result.at("status") == "Missing" || result.at("status") == "Changed");
    if (result.contains("error") && !copy_status) throw std::runtime_error(result.at("error").get<std::string>());
    return result;
}

std::string android_tree() { return android_storage({{"action", "tree"}}).at("tree").get<std::string>(); }

class AndroidCopyScope {
public:
    explicit AndroidCopyScope(const TransferHooks& hooks) { android_copy_hooks = &hooks; }
    ~AndroidCopyScope() { android_copy_hooks = nullptr; }
};

std::filesystem::path android_staging() { return android_downloads / ".staging"; }

std::filesystem::path android_staging(const std::filesystem::path& destination, const std::filesystem::path& downloads) {
    const auto text = destination.u8string();
    unsigned char bytes[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(text.data(), text.size(), bytes, &length, EVP_sha256(), nullptr) != 1 || length != 32)
        throw std::runtime_error("Cannot identify private download staging");
    constexpr char digits[] = "0123456789abcdef";
    std::string digest;
    digest.reserve(length * 2);
    for (unsigned int index = 0; index < length; ++index) {
        digest += digits[bytes[index] >> 4]; digest += digits[bytes[index] & 15];
    }
    return downloads / ".staging" / digest;
}

std::filesystem::path android_staging(const std::filesystem::path& destination) { return android_staging(destination, android_downloads); }

void android_remove_staging(const File& file, const std::filesystem::path& destination) {
    const auto archive = download_target(android_staging(destination), file);
    std::filesystem::remove(archive);
}

void android_trust_diagnostics() {
    ERR_clear_error();
    std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store(X509_STORE_new(), X509_STORE_free);
    bool loaded = store && X509_STORE_load_locations(store.get(), android_ca.u8string().c_str(), nullptr) == 1;
    if (!loaded) std::cerr << "[network] Cannot load CA certificate bundle\n";
    ERR_clear_error();
}
}

namespace archive_catalog {
CredentialStore::CredentialStore(std::filesystem::path path) : path_(std::move(path)) {}
std::filesystem::path CredentialStore::default_path() { return android_root / "credentials.bin"; }
bool CredentialStore::supported() { return android_bridge_class != nullptr; }
// Saved keys are encrypted with an Android Keystore AES key (NativeBridge.writeCredentials).
bool CredentialStore::protected_storage() { return android_bridge_class != nullptr; }
std::optional<S3Credentials> CredentialStore::load() const {
    auto saved = credential_call("readCredentials");
    if (saved.empty()) return std::nullopt;
    auto value = Json::parse(saved);
    S3Credentials result{value.at("access"), value.at("secret")};
    OPENSSL_cleanse(saved.data(), saved.size());
    return result;
}
void CredentialStore::save(const S3Credentials& credentials) const {
    auto saved = Json({{"access", credentials.access_key}, {"secret", credentials.secret_key}}).dump();
    try { credential_call("writeCredentials", &saved); }
    catch (...) { OPENSSL_cleanse(saved.data(), saved.size()); throw; }
    OPENSSL_cleanse(saved.data(), saved.size());
}
void CredentialStore::forget() const { credential_call("forgetCredentials"); }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_archivecatalog_app_NativeBridge_copyCancelled(JNIEnv*, jclass) {
    try { return android_copy_hooks && android_copy_hooks->cancelled && android_copy_hooks->cancelled() ? JNI_TRUE : JNI_FALSE; }
    catch (...) { return JNI_TRUE; }
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_archivecatalog_app_NativeBridge_extractPrivate(JNIEnv* environment, jclass, jstring archive, jstring folder, jboolean cancel) {
    std::lock_guard<std::mutex> guard(android_mutex);
    android_environment = environment;
    try {
        if (!android_app || android_app->model.snapshot().busy) throw std::runtime_error("Stop current work before extracting");
        const auto source = std::filesystem::weakly_canonical(std::filesystem::u8path(java_text(environment, archive)));
        const auto destination = std::filesystem::weakly_canonical(std::filesystem::u8path(java_text(environment, folder)));
        const auto staging = std::filesystem::weakly_canonical(android_staging());
        auto contained = [&staging](const std::filesystem::path& path) {
            const auto relative = path.lexically_relative(staging);
            return !relative.empty() && !relative.is_absolute() && *relative.begin() != ".." && path != staging;
        };
        if (!contained(source) || !contained(destination)) throw std::runtime_error("Extraction must remain in private staging");
        const auto result = native_extract(source, destination, [cancel] { return cancel == JNI_TRUE; }, {}, {});
        return environment->NewStringUTF(Json({{"result", result.result == ExtractOutcome::Result::Done ? "Done" :
            result.result == ExtractOutcome::Result::Cancelled ? "Cancelled" : "Failed"}, {"error", result.error},
            {"files", result.files}, {"unsafe", result.unsafe}, {"links", result.links}}).dump(-1, ' ', true).c_str());
    } catch (const std::exception& failure) {
        return environment->NewStringUTF(Json({{"error", failure.what()}}).dump(-1, ' ', true).c_str());
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_archivecatalog_app_NativeBridge_copyProgress(JNIEnv*, jclass, jlong received, jlong total) {
    if (android_copy_hooks && android_copy_hooks->progress) android_copy_hooks->progress(received, total);
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_archivecatalog_app_NativeBridge_start(JNIEnv* environment, jclass type, jstring root, jstring downloads, jstring ca) {
    std::lock_guard<std::mutex> guard(android_mutex);
    android_environment = environment;
    try {
        environment->GetJavaVM(&android_vm);
        if (!android_bridge_class) android_bridge_class = static_cast<jclass>(environment->NewGlobalRef(type));
        android_consoles.reset(); android_app.reset(); android_log.reset();
        {
            android_root = std::filesystem::u8path(java_text(environment, root));
            android_downloads = std::filesystem::u8path(java_text(environment, downloads));
            android_ca = std::filesystem::u8path(java_text(environment, ca));
            std::filesystem::create_directories(android_downloads);
            android_log = std::make_unique<DiagnosticLog>();
            std::cerr << "[android] Native catalog starting\n[android] App version=" << credential_call("appVersion")
                      << "\n[storage] Private directory=" << android_root.u8string()
                      << "\n[storage] Download staging directory=" << android_downloads.u8string() << '\n';
            android_trust_diagnostics();
            android_app = std::make_unique<WebApplication>(android_root);
            android_consoles = std::make_unique<ConsoleLink>(android_root / "consoles.json", ConsoleSecrets{
                [](const std::string& text) { return credential_call("protectText", &text); },
                [](const std::string& text) { return credential_call("unprotectText", &text); },
                credential_call("deviceName")});
            android_consoles->payload_dir = android_root / "payload";
            android_storage({{"action", "configure"}, {"private", android_downloads.u8string()}});
            android_app->platform_storage = android_storage;
            android_app->platform_partial_path = [downloads = android_downloads](const File& file, const std::filesystem::path& destination) {
                auto partial = download_target(android_staging(destination, downloads), file); partial += ".part";
                return partial;
            };
            if (android_app->download_state_writable) {
                auto next = android_app->download_state;
                for (auto& location : next["locations"]) if (!next.contains("platformStorage") && location.at("id") == "default" &&
                    (!std::filesystem::exists(android_root / "download-locations.json") || location.at("path") == android_downloads.u8string()))
                    location["path"] = android_tree();
                next["platformStorage"] = 1;
                android_app->commit_download_state(std::move(next));
            }
            android_app->config.download_dir = std::filesystem::u8path(find_location(android_app->download_state,
                android_app->download_state.at("default")).at("path").get<std::string>());
            android_app->config.output_dir = android_root / "exports";
            android_app->config.ca_bundle = android_ca.u8string();
            android_app->extract = native_extract;
            android_app->download = [app = android_app.get()](const File& file, const std::filesystem::path& directory, bool overwrite, const TransferHooks& hooks) {
                AndroidThread thread;
                android_overwrite = overwrite;
                android_storage({{"action", "validate"}, {"tree", directory.u8string()}});
                const auto staging = android_staging(directory);
                std::filesystem::create_directories(staging);
                Http http(app->config.ca_bundle, hooks, app->config.s3, app->config.download_connections);
                bool staged = false;
                auto target = download_target(staging, file);
                if (std::filesystem::exists(target)) {
                    try { verify_file(file, target, hooks); staged = true; }
                    catch (const OperationCancelled&) { throw; }
                    catch (const std::exception& failure) { std::cerr << "[download] Staging verification failed: " << failure.what() << '\n'; }
                }
                if (!staged) http.download(file, staging, true);
            };
            android_app->platform_record_download = [app = android_app.get()](const File& file, const std::filesystem::path& directory, const TransferHooks& hooks) {
                AndroidCopyScope copy(hooks);
                if (hooks.status) hooks.status("Publishing verified download");
                Json previous;
                {
                    std::lock_guard<std::mutex> guard(app->download_state_mutex);
                    for (const auto& entry : app->download_state.at("records")) if (entry.contains("storage") &&
                        entry.at("storage").at("tree") == directory.u8string() && entry.at("storage").contains("archive") &&
                        entry.at("storage").at("archive").at("name") == std::filesystem::u8path(file.name).filename().u8string()) previous = entry;
                }
                if (!previous.is_null() && previous.at("item") == file.item && previous.at("name") == file.name && previous.at("size") == file.size && !android_overwrite &&
                    android_storage({{"action", "status"}, {"record", previous}}).at("status") == "Downloaded" &&
                    android_storage({{"action", "matchesStaging"}, {"record", previous}, {"path", download_target(android_staging(directory), file).u8string()}}).at("matches").get<bool>()) return;
                Json record;
                try {
                    record = android_storage({{"action", "publish"}, {"tree", directory.u8string()}, {"previous", previous}, {"overwrite", android_overwrite},
                        {"path", download_target(android_staging(directory), file).u8string()},
                        {"name", std::filesystem::u8path(file.name).filename().u8string()}, {"size", file.size}});
                } catch (const std::exception&) {
                    if (hooks.cancelled && hooks.cancelled()) throw OperationCancelled();
                    throw;
                }
                record["item"] = file.item; record["name"] = file.name; record["size"] = file.size;
                std::lock_guard<std::mutex> guard(app->download_state_mutex);
                auto next = app->download_state;
                if (!previous.is_null()) {
                    auto& records = next["records"];
                    records.erase(std::remove_if(records.begin(), records.end(), [&](const Json& entry) { return entry.at("path") == previous.at("path"); }), records.end());
                }
                next["records"].push_back(record);
                try {
                    app->commit_download_state(std::move(next));
                } catch (const std::exception& failure) {
                    try { android_storage({{"action", "deleteCopy"}, {"record", record}}); }
                    catch (const std::exception& rollback) { throw std::runtime_error(std::string(failure.what()) + "; published copy rollback failed: " + rollback.what()); }
                    throw;
                }
            };
            android_app->platform_extract_after_download = [app = android_app.get()](const File& file, const std::filesystem::path& directory, const TransferHooks& hooks) {
                AndroidCopyScope copy(hooks);
                const auto folder_name = archive_folder_name(file.name);
                bool wanted = app->config.extract_archives;
                Json record;
                {
                    std::lock_guard<std::mutex> guard(app->download_state_mutex);
                    for (const auto& location : app->download_state.at("locations"))
                        if (location.at("path") == directory.u8string() && location.value("extract", false)) wanted = true;
                    for (const auto& entry : app->download_state.at("records"))
                        if (entry.at("item") == file.item && entry.at("name") == file.name && entry.contains("storage") &&
                            entry.at("storage").at("tree") == directory.u8string()) record = entry;
                }
                if (!wanted || folder_name.empty()) { android_remove_staging(file, directory); return; }
                { std::lock_guard<std::mutex> guard(app->extraction_mutex); app->extraction_error.clear(); }
                if (record.is_null()) throw std::runtime_error("Published download record is unavailable");
                const auto folder = android_staging(directory) / std::filesystem::u8path(folder_name);
                const auto status = [&hooks](const std::string& text) { if (hooks.status) hooks.status(text); };
                const auto progress = [&hooks](std::uint64_t done, std::uint64_t total) { if (hooks.stage) hooks.stage("Extracting", done, total); };
                const std::function<bool()> cancelled = hooks.cancelled ? hooks.cancelled : [] { return false; };
                const auto outcome = app->extract(download_target(android_staging(directory), file), folder, cancelled, status, progress);
                if (outcome.result != ExtractOutcome::Result::Done) {
                    const auto problem = outcome.result == ExtractOutcome::Result::Cancelled ? "Extraction stopped; the archive was kept"
                        : "Could not extract " + file.name + ": " + outcome.error + ". The archive was kept.";
                    status(problem);
                    std::lock_guard<std::mutex> guard(app->extraction_mutex); app->extraction_error = problem;
                    return;
                }
                try {
                    record = android_storage({{"action", "publishExtracted"}, {"record", record}, {"path", folder.u8string()}, {"name", folder_name}});
                    {
                        std::lock_guard<std::mutex> guard(app->download_state_mutex);
                        auto next = app->download_state;
                        for (auto& entry : next["records"]) if (entry.at("path") == record.at("path")) entry = record;
                        try { app->commit_download_state(std::move(next)); }
                        catch (const std::exception& failure) {
                            try { android_storage({{"action", "deleteExtraction"}, {"record", record}}); }
                            catch (const std::exception& rollback) { throw std::runtime_error(std::string(failure.what()) + "; extraction rollback failed: " + rollback.what()); }
                            throw;
                        }
                    }
                    if (app->config.delete_extracted_archives && !outcome.unsafe && !outcome.links) {
                        record = android_storage({{"action", "deleteArchive"}, {"record", record}});
                        std::lock_guard<std::mutex> guard(app->download_state_mutex);
                        auto next = app->download_state;
                        for (auto& entry : next["records"]) if (entry.at("path") == record.at("path")) entry = record;
                        app->commit_download_state(std::move(next));
                    }
                    std::error_code removal;
                    if (!remove_tree(folder, removal)) throw std::runtime_error("Published extraction staging cleanup failed: " + removal.message());
                    android_remove_staging(file, directory);
                    status("Extracted " + std::to_string(outcome.files) + " files to " + folder_name);
                    if (outcome.unsafe || outcome.links) {
                        std::lock_guard<std::mutex> guard(app->extraction_mutex);
                        app->extraction_error = "Unsafe names or links were skipped; the archive was kept.";
                    }
                } catch (const std::exception& failure) {
                    std::error_code removal;
                    if (real_directory(folder) && !remove_tree(folder, removal))
                        std::cerr << "[storage] Extraction staging rollback failed: " << removal.message() << '\n';
                    std::lock_guard<std::mutex> guard(app->extraction_mutex);
                    app->extraction_error = std::string(failure.what()) + "; archive was not automatically discarded after publication failure";
                    status(app->extraction_error);
                }
            };
            android_session = random_token();
        }
        return environment->NewStringUTF("{}");
    } catch (const std::exception& failure) {
        return environment->NewStringUTF(Json({{"error", failure.what()}}).dump(-1, ' ', true).c_str());
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_archivecatalog_app_NativeBridge_request(JNIEnv* environment, jclass, jstring path, jstring body, jstring token) {
    std::lock_guard<std::mutex> guard(android_mutex);
    android_environment = environment;
    try {
        if (!android_app) throw std::runtime_error("Native service is starting");
        auto route = java_text(environment, path);
        Json result;
        if (route == "/session") result = {{"token", android_session}, {"version", ARCHIVEFETCH_VERSION}, {"consoles", true}};
        else {
            if (java_text(environment, token) != android_session) throw std::runtime_error("Invalid app session");
            if (route == "/api/consoles" || route == "/api/relay") {
                int status = 200;
                result = console_route(*android_consoles, route, Json::parse(java_text(environment, body)), status);
                // The page has no HTTP status here, so a refusal without its own message still reads as an error.
                if (status != 200 && !result.contains("error")) result["error"] = "The console returned HTTP " + std::to_string(status);
                if (status != 200) result["status"] = status;
            }
            else if (route == "/api/state") {
                result = android_app->snapshot(); result["platform"] = "android";
            }
            else if (route == "/api/library") {
                result = android_app->library();
            }
            else if (route == "/api/diagnostics") result = {{"log", android_log->text()}};
            else if (route == "/api/diagnostics/clear") { android_log->clear(); result = Json::object(); }
            else if (route == "/api/command") {
                auto value = Json::parse(java_text(environment, body));
                auto action = value.at("action").get<std::string>();
                std::string chosen_tree;
                if (action == "settings") {
                    chosen_tree = value.at("destination").get<std::string>();
                    if (value.contains("defaultLocation")) {
                        if (chosen_tree != android_app->config.download_dir.u8string()) {
                            // The Android picker changes the legacy destination field, not the saved location radio.
                            value.erase("defaultLocation");
                        } else {
                            std::lock_guard<std::mutex> storage_guard(android_app->download_state_mutex);
                            chosen_tree = find_location(android_app->download_state, value.at("defaultLocation").get<std::string>())
                                .at("path").get<std::string>();
                        }
                    }
                    if (chosen_tree != android_app->config.download_dir.u8string() && !android_app->model.snapshot().queue.empty())
                        throw std::runtime_error("Finish or remove queued downloads before changing the destination folder");
                    if (value.at("output") != (android_root / "exports").u8string() || (value.contains("ca") && value.at("ca") != android_ca.u8string()))
                        throw std::runtime_error("Android export and trust paths are managed by the app");
                    android_storage({{"action", "validate"}, {"tree", chosen_tree}});
                }
                if (action == "import" && value.at("path") != (android_root / "import.xml").u8string())
                    throw std::runtime_error("Use the Android document picker to import a catalog");
                if (action == "share") {
                    auto state = android_app->model.snapshot();
                    if (value.at("revision").get<std::uint64_t>() != state.catalog_revision) throw std::runtime_error("Library changed; refresh the selection");
                    auto listing = android_app->library(true);
                    Json files = Json::array();
                    for (auto index : value.at("indices").get<std::vector<std::size_t>>()) {
                        if (index >= state.catalog.files.size()) throw std::runtime_error("Invalid library selection");
                        bool found = false;
                        for (const auto& entry : listing.at("files").at(index).at("downloadCopies")) if (entry.at("status") == "Downloaded" || entry.at("status") == "Unrecorded") {
                            files.push_back(entry.at("path")); found = true; break;
                        }
                        if (!found) throw std::runtime_error("Only unchanged completed downloads can be shared");
                    }
                    if (files.empty() || files.size() > 100) throw std::runtime_error("Select between 1 and 100 downloaded files");
                    result = {{"ok", true}, {"shareFiles", files}};
                } else {
                    result = android_app->command(value);
                    if (action == "settings" || action == "defaultLocation" || action == "editLocation" || action == "removeLocation")
                        android_storage({{"action", "select"}, {"tree", android_app->config.download_dir.u8string()}});
                    if (action == "export") result["shareFiles"] = {
                        (android_app->config.output_dir / "catalog.xml").u8string(),
                        (android_app->config.output_dir / "catalog.csv").u8string(),
                        (android_app->config.output_dir / "urls.txt").u8string()};
                }
            } else throw std::runtime_error("Unknown app request");
        }
        return environment->NewStringUTF(result.dump(-1, ' ', true).c_str());
    } catch (const std::exception& failure) {
        return environment->NewStringUTF(Json({{"error", failure.what()}}).dump(-1, ' ', true).c_str());
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_archivecatalog_app_NativeBridge_stop(JNIEnv* environment, jclass, jstring token) {
    std::lock_guard<std::mutex> guard(android_mutex);
    android_environment = environment;
    if (java_text(environment, token) != android_session) return;
    android_consoles.reset(); android_app.reset(); android_log.reset(); android_session.clear();
}