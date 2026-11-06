#include "catalog.hpp"

#include <iostream>
#include <stdexcept>

using namespace archive_catalog;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Action>
void rejects(Action action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "Expected input rejection");
}

int main() {
    try {
        const std::string item = "example-part-0";
        require(item_id("https://archive.org/details/" + item) == item, "Details URL");
        require(item_id("https://ia601503.us.archive.org/25/items/" + item + "/" + item + "_files.xml") == item, "Direct XML URL");
        rejects([] { item_id("https://archive.org.evil.test/details/example"); });
        rejects([] { item_id("https://evilarchive.org/details/example"); });
        rejects([] { item_id("https://user@archive.org/details/example"); });
        rejects([] { item_id("https://archive.org/details/.."); });
        const auto items = discover_items(item,
            "<a href='/details/example-part-A'>A</a><br>"
            "<a href='https://archive.org/details/example-part-g_202207'>G</a>"
            "<a href='/details/example-part-A'>Duplicate</a>"
            "<a href='/details/unrelated'>Other</a>"
            "<a href='https://evil.test/details/example-part-X'>Bad</a>");
        require(items.size() == 3 && items[2] == "example-part-g_202207", "Discover real links, not guessed identifiers");
        const auto case_sensitive = discover_items("ps2-redump-usa-chd-part-0",
            "K - <a href=\"https://archive.org/details/ps2-redump-usa-chd-part-K\" rel=\"ugc nofollow\">"
            "https://archive.org/details/ps2-redump-usa-chd-part-K</a><br />");
        require(case_sensitive.size() == 2 && case_sensitive[1] == "ps2-redump-usa-chd-part-K",
            "Preserve uppercase K from the real part-0 description link");
        require(download_url({case_sensitive[1], "Example.chd", 3, {}, {}}) ==
            "https://archive.org/download/ps2-redump-usa-chd-part-K/Example.chd",
            "Preserve discovered identifier case in download URLs");
        const std::string xml = "<files>"
            "<file name='Folder/A &amp; B (USA).chd'><size>5759416166</size><sha1>0123456789012345678901234567890123456789</sha1></file>"
            "<file name='B.CHD'><size>25</size></file>"
            "<file name='hidden.chd'><size>25</size><private>true</private></file>"
            "<file name='metadata.xml'><size>25</size></file></files>";
        auto files = parse_files(item, xml);
        require(files.size() == 4 && files[0].size == 5759416166ULL && files[3].name == "metadata.xml", "Keep XML metadata in the catalog and preserve 64-bit sizes");
        auto general_files = parse_files(item, "<files>"
            "<file name='Game.iso'><size>3</size></file>"
            "<file name='Archive.zip'><size>3</size></file>"
            "<file name='Notes.txt'><size>3</size></file>"
            "<file name='README'><size>3</size></file>"
            "<file name='Metadata.XML'><size>3</size></file>"
            "<file name='Download.ToRrEnT'><size>3</size></file>"
            "<file name='item_meta.SQLite'><size>3</size></file>"
            "<file name='index.sqlite3'><size>3</size></file>"
            "<file name='nested/Metadata.xml'><size>3</size></file>"
            "<file name='Metadata.xml.zip' private='true'><size>3</size></file></files>");
        require(general_files.size() == 10 && general_files.back().restricted,
            "Keep every file type in the catalog, including XML, torrent and SQLite files");
        const auto& defaults = default_excluded_extensions();
        std::size_t hidden = 0;
        for (const auto& file : general_files) hidden += excluded_file_type(file.name, defaults);
        require(hidden == 6 && !excluded_file_type("Metadata.xml.zip", defaults) && !excluded_file_type("README", defaults) &&
            excluded_file_type("Cover.JPG", defaults) && excluded_file_type("logs/run.log", defaults) && !excluded_file_type(".png", defaults) &&
            !excluded_file_type("dir.txt/README", defaults), "Default exclusions match the last extension of the filename, ignoring case");
        require(!excluded_file_type("Notes.txt", {}) && excluded_file_type("Game.ISO", {".iso"}), "Exclusions follow the configured list");
        require(parse_extension_list(" .XML, torrent;;.xml\t.tar_gz ") == std::vector<std::string>{".xml", ".torrent", ".tar_gz"} &&
            parse_extension_list("").empty() && format_extension_list({".xml", ".txt"}) == ".xml, .txt" &&
            parse_extension_list(format_extension_list(defaults)) == defaults, "Read and write extension lists");
        rejects([] { parse_extension_list(".tar.gz"); });
        rejects([] { parse_extension_list("."); });
        rejects([] { parse_extension_list("*.txt"); });
        rejects([] { parse_extension_list("." + std::string(32, 'a')); });
        rejects([&] { parse_files(item, "<files><file name='../bad.iso'><size>1</size></file></files>"); });
        require(!files[0].restricted && files[2].restricted, "Keep restricted files with their access flag");
        auto attribute_private = parse_files(item, "<files><file name='restricted.chd' private='true'><size>3</size></file></files>");
        require(attribute_private.size() == 1 && attribute_private[0].restricted, "Attribute private flag");
        auto public_file = parse_files(item, "<files><file name='public.chd'><size>3</size><private>false</private></file></files>");
        require(public_file.size() == 1 && !public_file[0].restricted, "Explicit public file flag");
        require(download_url(files[0]) == "https://archive.org/download/example-part-0/Folder/A%20%26%20B%20%28USA%29.chd", "Encode file paths");
        std::vector<File> merged;
        merge_files(merged, files);
        merge_files(merged, files);
        require(merged.size() == 4, "Deduplicate repeated manifests");
        auto other = files[0];
        other.item = "example-part-A";
        merge_files(merged, {other});
        require(merged.size() == 5, "Preserve distinct source items");
        for (const auto* path : {"../bad.chd", "/bad.chd", "folder/../bad.chd", "C:/bad.chd", "bad\\file.chd", "folder//bad.chd", "folder/", "NUL.chd"}) {
            require(!safe_relative_path(path), "Reject unsafe local path");
        }
        rejects([&] { parse_files(item, "<files><file name='../bad.chd'><size>1</size></file></files>"); });
        rejects([&] { parse_files(item, "<files><file name='bad.chd'><size>-1</size></file></files>"); });
        const auto sizeless = parse_files(item, "<files><file name='example-part-0_files.xml' source='original'><format>Metadata</format></file>"
            "<file name='example-part-0_meta.xml'><size>5209</size></file></files>");
        require(sizeless.size() == 1 && sizeless[0].name == "example-part-0_meta.xml", "Leave out the size-less generated _files.xml without failing the item");
        rejects([&] { parse_files(item, "<files><file name='bad.chd'><size>1</size><md5>oops</md5></file></files>"); });
        rejects([&] { parse_files(item, "<!DOCTYPE files [<!ENTITY leak SYSTEM 'file:///etc/passwd'>]><files/>"); });
        rejects([&] { parse_files(item, "<files>"); });
        std::vector<std::string> skipped;
        auto kept = parse_files(item, "<files><file name='good.chd'><size>3</size></file><file name='Game: Part?.iso'><size>3</size></file>"
            "<file name='../bad.chd'><size>3</size></file><file name='empty.txt'><size>0</size></file>"
            "<file name='history/files/ndsF:/old.zip.~1~'><size>3</size></file><file name='history/files/good.chd.~2~'><size>3</size></file></files>", &skipped);
        require(kept.size() == 1 && kept[0].name == "good.chd" && skipped == std::vector<std::string>{"Game: Part?.iso", "../bad.chd"},
            "Unsafe and empty files and archive.org history copies are left out without dropping the rest of the item");
        for (const auto& [name, folder] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"Game (USA).zip", "Game (USA)"}, {"Folder/Pack.7Z", "Pack"}, {"Disc.rar", "Disc"}, {"Set.tar.gz", "Set"},
                 {"Set.TGZ", "Set"}, {"Data.tar", "Data"}, {"Dots..zip", "Dots"}, {".zip", ""}, {"Game.iso", ""}, {"Game.chd", ""},
                 {"Big.part1.rar", ""}, {"Big.part02.rar", ""}, {"Big.r00", ""}, {"Big.z01", ""}, {"Big.7z.001", ""}, {"Big.zip.001", ""},
                 {"Big.001", ""}, {"party.rar", "party"}, {"README", ""}}) {
            require(archive_folder_name(name) == folder, name);
        }
        std::cout << "All catalog tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}