#include "credential_store.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace archive_catalog;

template <typename Action>
void rejects(Action action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    if (!rejected) throw std::runtime_error("Expected credential storage rejection");
}

int main() {
    auto directory = std::filesystem::temp_directory_path() /
        ("archive-credentials-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        CredentialStore store(directory / "credentials.bin");
        if (store.load()) throw std::runtime_error("Unexpected saved credentials");
        rejects([&] { store.save({"access", ""}); });
        rejects([&] { store.save({"access", "secret\ninvalid"}); });
        store.save({"test-access-not-real", "test-secret-not-real"});
        {
            std::ifstream input(directory / "credentials.bin", std::ios::binary);
            std::string contents((std::istreambuf_iterator<char>(input)), {});
            if (contents.find("test-access-not-real") != std::string::npos ||
                contents.find("test-secret-not-real") != std::string::npos) {
                throw std::runtime_error("Plaintext credentials were saved");
            }
        }
        auto loaded = CredentialStore(directory / "credentials.bin").load();
        if (!loaded || loaded->access_key != "test-access-not-real" || loaded->secret_key != "test-secret-not-real") {
            throw std::runtime_error("Credential round-trip failed");
        }
        store.save({"replacement-access", "replacement-secret"});
        if (store.load()->secret_key != "replacement-secret") throw std::runtime_error("Credential update failed");
        {
            std::fstream file(directory / "credentials.bin", std::ios::binary | std::ios::in | std::ios::out);
            file.seekg(-1, std::ios::end);
            char value = 0;
            file.read(&value, 1);
            value ^= 1;
            file.seekp(-1, std::ios::end);
            file.write(&value, 1);
        }
        rejects([&] { store.load(); });
        store.forget();
        store.forget();
        if (store.load()) throw std::runtime_error("Credentials were not forgotten");
        { std::ofstream file(directory / "credentials.bin"); }
        rejects([&] { store.load(); });
        std::filesystem::remove_all(directory);
        std::cout << "All credential storage tests passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::filesystem::remove_all(directory);
        std::cerr << failure.what() << '\n';
        return 1;
    }
}