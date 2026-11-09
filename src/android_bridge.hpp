#pragma once
#include <jni.h>
#include <curl/curl.h>
#include <openssl/err.h>
#include <openssl/x509.h>

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
    auto text = request.dump(-1, ' ', true);
    auto result = Json::parse(credential_call("storageRequest", &text));
    if (result.contains("error")) throw std::runtime_error(result.at("error").get<std::string>());
    return result;
}

std::string android_tree() { return android_storage({{"action", "tree"}}).at("tree").get<std::string>(); }

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
            android_app->config.download_dir = android_downloads;
            android_app->config.output_dir = android_root / "exports";
            android_app->config.ca_bundle = android_ca.u8string();
            android_app->destination_exists = [](const File& file) {
                auto name = std::filesystem::u8path(file.name).filename().u8string();
                return android_tree().empty() ? std::filesystem::exists(android_downloads / std::filesystem::u8path(name)) :
                    android_storage({{"action", "exists"}, {"name", name}}).at("exists").get<bool>();
            };
            android_app->download = [app = android_app.get()](const File& file, const std::filesystem::path& directory, bool overwrite, const TransferHooks& hooks) {
                AndroidThread thread;
                bool selected = !android_tree().empty();
                std::cerr << "[download] Starting; destination=" << (selected ? "selected folder" : "app-specific storage")
                          << " expected_bytes=" << file.size << " overwrite=" << (overwrite ? "yes" : "no") << '\n';
                Http http(app->config.ca_bundle, hooks, app->config.s3, app->config.download_connections);
                bool staged = false;
                if (selected) {
                    auto target = download_target(directory, file);
                    if (std::filesystem::exists(target)) {
                        try { verify_file(file, target, hooks); staged = true; }
                        catch (const OperationCancelled&) { throw; }
                        catch (const std::exception&) { }
                    }
                }
                if (!staged) http.download(file, directory, selected || overwrite);
                if (!selected) { std::cerr << "[download] Verified in app-specific storage\n"; return; }
                auto name = std::filesystem::u8path(file.name).filename();
                auto path = directory / name;
                android_copy_hooks = &hooks;
                std::cerr << "[storage] Publishing verified staging file to selected folder\n";
                try {
                    android_storage({{"action", "publish"}, {"path", path.u8string()}, {"name", name.u8string()}, {"size", file.size}, {"overwrite", overwrite}});
                    android_copy_hooks = nullptr;
                } catch (...) {
                    android_copy_hooks = nullptr;
                    if (hooks.cancelled && hooks.cancelled()) throw OperationCancelled();
                    throw;
                }
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
                std::cerr << "[storage] Publication complete; staging_cleanup_error=" << ignored.value() << '\n';
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
                auto tree = android_tree();
                if (!tree.empty()) result["settings"]["destination"] = tree;
            }
            else if (route == "/api/library") {
                result = android_app->library();
                if (!android_tree().empty()) {
                    std::map<std::string, std::uint64_t> sizes;
                    auto stored = android_storage({{"action", "list"}});
                    for (const auto& file : stored.at("files"))
                        if (file.at("size").get<std::int64_t>() >= 0) sizes[file.at("name")] = file.at("size");
                    for (auto& file : result["files"]) {
                        auto name = std::filesystem::u8path(file.at("name").get<std::string>()).filename().u8string();
                        file["downloaded"] = sizes.count(name) && sizes.at(name) == file.at("size").get<std::uint64_t>();
                    }
                }
            }
            else if (route == "/api/diagnostics") result = {{"log", android_log->text()}};
            else if (route == "/api/diagnostics/clear") { android_log->clear(); result = Json::object(); }
            else if (route == "/api/command") {
                auto value = Json::parse(java_text(environment, body));
                auto action = value.at("action").get<std::string>();
                std::string chosen_tree;
                if (action == "settings") {
                    chosen_tree = value.at("destination").get<std::string>();
                    if (chosen_tree == android_downloads.u8string()) chosen_tree.clear();
                    if (chosen_tree != android_tree() && !android_app->model.snapshot().queue.empty())
                        throw std::runtime_error("Finish or remove queued downloads before changing the destination folder");
                    if (value.at("output") != (android_root / "exports").u8string() || (value.contains("ca") && value.at("ca") != android_ca.u8string()))
                        throw std::runtime_error("Android export and trust paths are managed by the app");
                    android_storage({{"action", "validate"}, {"tree", chosen_tree}});
                    value["destination"] = android_downloads.u8string();
                }
                if (action == "import" && value.at("path") != (android_root / "import.xml").u8string())
                    throw std::runtime_error("Use the Android document picker to import a catalog");
                if (action == "share") {
                    auto state = android_app->model.snapshot();
                    if (value.at("revision").get<std::uint64_t>() != state.catalog_revision) throw std::runtime_error("Library changed; refresh the selection");
                    auto status = library_download_status(state.catalog.files, android_downloads);
                    std::map<std::string, Json> destination;
                    auto selected_tree = android_tree();
                    if (!selected_tree.empty()) {
                        auto stored = android_storage({{"action", "list"}});
                        for (const auto& file : stored.at("files")) destination[file.at("name")] = file;
                    }
                    Json files = Json::array();
                    for (auto index : value.at("indices").get<std::vector<std::size_t>>()) {
                        if (index >= status.size()) throw std::runtime_error("Invalid library selection");
                        const auto& file = state.catalog.files.at(index);
                        auto name = std::filesystem::u8path(file.name).filename().u8string();
                        if (!selected_tree.empty()) {
                            if (!destination.count(name) || destination.at(name).at("size") != file.size) throw std::runtime_error("Only completed downloads can be shared");
                            files.push_back(destination.at(name).at("uri"));
                        } else {
                            if (!status[index]) throw std::runtime_error("Only completed downloads can be shared");
                            files.push_back((android_downloads / std::filesystem::u8path(name)).u8string());
                        }
                    }
                    if (files.empty() || files.size() > 100) throw std::runtime_error("Select between 1 and 100 downloaded files");
                    result = {{"ok", true}, {"shareFiles", files}};
                } else {
                    result = android_app->command(value);
                    if (action == "settings") android_storage({{"action", "select"}, {"tree", chosen_tree}});
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