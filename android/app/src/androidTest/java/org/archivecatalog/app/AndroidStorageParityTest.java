package org.archivecatalog.app;

import static org.junit.Assert.*;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.DocumentsContract;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.UUID;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Assume;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class AndroidStorageParityTest {
    private final Context target = InstrumentationRegistry.getInstrumentation().getTargetContext();
    private static final class Fixture implements AutoCloseable {
        final File root, downloads, staging;
        final DownloadStorage storage;
        final Context context;
        Fixture(Context context) throws Exception {
            root = new File(context.getCacheDir(), "storage-parity-" + UUID.randomUUID()); assertTrue(root.mkdir());
            downloads = new File(root, "downloads"); assertTrue(downloads.mkdir());
            staging = new File(downloads, ".staging"); assertTrue(staging.mkdir());
            String preferences = "storage-parity-" + UUID.randomUUID();
            this.context = new ContextWrapper(context) {
                @Override public android.content.SharedPreferences getSharedPreferences(String name, int mode) {
                    return context.getSharedPreferences(preferences + "-" + name, mode);
                }
                @Override public Context getApplicationContext() { return this; }
            };
            storage = new DownloadStorage(this.context);
            storage.request(new JSONObject().put("action", "configure").put("private", downloads.getAbsolutePath()));
        }
        File source(String name, String text) throws Exception {
            File file = new File(staging, name); Files.write(file.toPath(), text.getBytes(StandardCharsets.UTF_8)); return file;
        }
        File queueSource(String destination, String name, String text) throws Exception {
            File directory = NativeIntegrationTest.stagingDirectory(downloads, destination);
            Files.createDirectories(directory.toPath());
            File file = new File(directory, name); Files.write(file.toPath(), text.getBytes(StandardCharsets.UTF_8)); return file;
        }
        JSONObject archive(String tree, String name) throws Exception {
            File source = source(name, "archive");
            JSONObject record = storage.request(new JSONObject().put("action", "publish").put("tree", tree).put("path", source.getAbsolutePath())
                .put("name", name).put("size", source.length()));
            return record.put("item", "fixture").put("name", name).put("size", source.length());
        }
        JSONObject extracted(JSONObject record, String name) throws Exception {
            File directory = new File(staging, name); assertTrue(directory.mkdir());
            Files.write(new File(directory, "payload.bin").toPath(), "payload".getBytes(StandardCharsets.UTF_8));
            return storage.request(new JSONObject().put("action", "publishExtracted").put("record", record)
                .put("name", name).put("path", directory.getAbsolutePath()));
        }
        @Override public void close() throws Exception {
            try (java.util.stream.Stream<java.nio.file.Path> paths = Files.walk(root.toPath())) {
                for (java.nio.file.Path path : paths.sorted(java.util.Comparator.reverseOrder()).toArray(java.nio.file.Path[]::new)) Files.delete(path);
            }
        }
    }
    private static void rejected(Throwing action, String message) throws Exception {
        try { action.run(); fail("Expected refusal: " + message); }
        catch (IllegalStateException | java.io.IOException expected) { assertNotNull(expected.getMessage()); }
    }
    private interface Throwing { void run() throws Exception; }
    private static String sha1(String text) throws Exception {
        StringBuilder hash = new StringBuilder();
        for (byte value : java.security.MessageDigest.getInstance("SHA-1").digest(text.getBytes(StandardCharsets.UTF_8)))
            hash.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        return hash.toString();
    }
    private static JSONObject command(String token, JSONObject value) throws Exception {
        JSONObject result = new JSONObject(NativeBridge.request("/api/command", value.toString(), token));
        assertFalse(result.toString(), result.has("error")); return result;
    }
    private static JSONObject state(String token) throws Exception {
        return new JSONObject(NativeBridge.request("/api/state", "{}", token));
    }
    private static JSONObject idle(String token) throws Exception {
        for (int attempt = 0; attempt < 400; ++attempt) {
            JSONObject state = state(token);
            assertFalse(state.toString(), state.has("error") && !state.has("busy"));
            if (!state.getBoolean("busy")) return state;
            Thread.sleep(25);
        }
        throw new AssertionError("Native fixture did not become idle");
    }
    @Test public void namedSafLocationRoutingPersistenceAndConfirmationCleanup() throws Exception {
        Context instrumentation = InstrumentationRegistry.getInstrumentation().getContext();
        Uri authority = Uri.parse("content://" + instrumentation.getPackageName() + ".storage-fixture-control");
        Bundle created = target.getContentResolver().call(authority, "fixtureCreate", null, null);
        assertNotNull(created);
        Uri tree = Uri.parse(created.getString("tree"));
        target.getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try (Fixture fixture = new Fixture(target)) {
            NativeBridge.initialize(fixture.context);
            Files.write(new File(fixture.root, "sources.xml").toPath(),
                "<sources><source url=\"https://archive.org/details/parity-source\" name=\"Fixture\" selected=\"true\"><tag>source-route</tag></source></sources>".getBytes(StandardCharsets.UTF_8));
            Files.write(new File(fixture.root, "import.xml").toPath(),
                "<catalog complete=\"true\"><item id=\"parity-source\"/><file item=\"parity-source\" name=\"routed.bin\" size=\"3\" sha1=\"a9993e364706816aba3e25717850c26c9cd0d89d\" restricted=\"false\"/></catalog>".getBytes(StandardCharsets.UTF_8));
            File ca = new File(fixture.root, "trust.pem");
            assertFalse(new JSONObject(NativeBridge.start(fixture.root.getAbsolutePath(), fixture.downloads.getAbsolutePath(), ca.getAbsolutePath())).has("error"));
            String token = new JSONObject(NativeBridge.request("/session", "{}", "")).getString("token");
            JSONObject downloaded = null;
            try {
                command(token, new JSONObject().put("action", "settings").put("destination", fixture.storage.defaultLocation())
                    .put("output", new File(fixture.root, "exports").getAbsolutePath()).put("ca", ca.getAbsolutePath()).put("automatic", false));
                command(token, new JSONObject().put("action", "import").put("path", new File(fixture.root, "import.xml").getAbsolutePath()));
                long revision = idle(token).getLong("catalogRevision");
                assertEquals("source-route", state(token).getJSONArray("sources").getJSONObject(0).getJSONArray("tags").getString(0));
                String first = command(token, new JSONObject().put("action", "addLocation").put("name", "SAF games").put("path", tree.toString())
                    .put("tags", new JSONArray().put("source-route")).put("extract", true)).getString("id");
                String second = command(token, new JSONObject().put("action", "addLocation").put("name", "Private other").put("path", fixture.downloads.getAbsolutePath())
                    .put("tags", new JSONArray().put("file-route"))).getString("id");
                command(token, new JSONObject().put("action", "settings").put("destination", fixture.storage.defaultLocation()).put("defaultLocation", first)
                    .put("output", new File(fixture.root, "exports").getAbsolutePath()).put("ca", ca.getAbsolutePath()).put("automatic", false));
                assertEquals(tree.toString(), state(token).getJSONObject("settings").getString("destination"));
                assertEquals(tree.toString(), new DownloadStorage(fixture.context).tree());
                command(token, new JSONObject().put("action", "defaultLocation").put("id", "default"));
                command(token, new JSONObject().put("action", "fileTags").put("index", 0).put("revision", revision)
                    .put("tags", new JSONArray().put("source-route").put("file-route")));
                JSONObject enqueue = new JSONObject().put("action", "enqueue").put("indices", new JSONArray().put(0)).put("revision", revision).put("location", "auto");
                assertEquals(1, command(token, enqueue).getJSONArray("locationConflicts").length());
                command(token, enqueue.put("locationChoices", new JSONObject().put("0", first)));
                JSONObject queued = state(token).getJSONArray("queue").getJSONObject(0);
                assertEquals(tree.toString(), queued.getString("destination"));
                command(token, new JSONObject().put("action", "removeQueue").put("id", queued.getLong("id")));
                command(token, new JSONObject().put("action", "fileTags").put("index", 0).put("revision", revision).put("tags", new JSONArray()));
                enqueue.remove("locationChoices");
                command(token, enqueue);
                queued = state(token).getJSONArray("queue").getJSONObject(0);
                assertEquals(tree.toString(), queued.getString("destination"));
                command(token, new JSONObject().put("action", "removeQueue").put("id", queued.getLong("id")));
                command(token, new JSONObject().put("action", "fileTags").put("index", 0).put("revision", revision).put("tags", new JSONArray().put("file-route")));
                command(token, enqueue);
                queued = state(token).getJSONArray("queue").getJSONObject(0);
                assertEquals(fixture.downloads.getAbsolutePath(), queued.getString("destination"));
                fixture.queueSource(fixture.downloads.getAbsolutePath(), "routed.bin", "abc");
                command(token, new JSONObject().put("action", "start"));
                assertEquals(idle(token).toString(), 0, state(token).getJSONArray("queue").length());
                JSONObject library = new JSONObject(NativeBridge.request("/api/library", "{}", token)).getJSONArray("files").getJSONObject(0);
                assertEquals("Downloaded", library.getString("downloadState"));
                downloaded = library.getJSONArray("downloadCopies").getJSONObject(0);
                JSONObject preview = command(token, new JSONObject().put("action", "cleanLibrary").put("preview", true)
                    .put("indices", new JSONArray().put(0)).put("revision", revision));
                assertEquals(1, preview.getJSONObject("delete").getInt("files"));
                JSONObject refused = new JSONObject(NativeBridge.request("/api/command", new JSONObject().put("action", "cleanLibrary")
                    .put("mode", "deleteDownloads").put("indices", new JSONArray().put(0)).put("revision", revision).toString(), token));
                assertTrue(refused.has("error"));
                NativeBridge.stop(token);
                assertFalse(new JSONObject(NativeBridge.start(fixture.root.getAbsolutePath(), fixture.downloads.getAbsolutePath(), ca.getAbsolutePath())).has("error"));
                token = new JSONObject(NativeBridge.request("/session", "{}", "")).getString("token");
                JSONArray locations = state(token).getJSONObject("settings").getJSONArray("locations");
                assertEquals(3, locations.length());
                boolean retained = false;
                for (int index = 0; index < locations.length(); ++index) {
                    JSONObject location = locations.getJSONObject(index);
                    if (first.equals(location.getString("id"))) retained = location.getBoolean("extract") && tree.toString().equals(location.getString("path"));
                }
                assertTrue(retained); assertNotEquals(first, second);
                revision = state(token).getLong("catalogRevision");
                command(token, new JSONObject().put("action", "cleanLibrary").put("mode", "deleteDownloads").put("confirm", "DELETE")
                    .put("indices", new JSONArray().put(0)).put("revision", revision));
                downloaded = null;
                assertEquals("Not downloaded", new JSONObject(NativeBridge.request("/api/library", "{}", token)).getJSONArray("files").getJSONObject(0).getString("downloadState"));
            } finally {
                if (downloaded != null) {
                    JSONObject current = state(token);
                    if (current.has("catalogRevision")) command(token, new JSONObject().put("action", "deleteDownload").put("index", 0)
                        .put("revision", current.getLong("catalogRevision")).put("path", downloaded.getString("path")));
                }
                NativeBridge.stop(token);
            }
        }
    }
    @Test public void sameBasenamePartialsStayIsolatedByDestination() throws Exception {
        Context instrumentation = InstrumentationRegistry.getInstrumentation().getContext();
        Uri authority = Uri.parse("content://" + instrumentation.getPackageName() + ".storage-fixture-control");
        Bundle created = target.getContentResolver().call(authority, "fixtureCreate", null, null);
        assertNotNull(created);
        Uri tree = Uri.parse(created.getString("tree"));
        target.getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try (Fixture fixture = new Fixture(target)) {
            NativeBridge.initialize(fixture.context);
            File ca = new File(fixture.root, "trust.pem");
            Files.write(new File(fixture.root, "import.xml").toPath(), ("<catalog complete=\"true\"><item id=\"parity-a\"/><item id=\"parity-b\"/>" +
                "<file item=\"parity-a\" name=\"same.bin\" size=\"3\" sha1=\"" + sha1("abc") + "\" restricted=\"false\"/>" +
                "<file item=\"parity-b\" name=\"same.bin\" size=\"3\" sha1=\"" + sha1("def") + "\" restricted=\"false\"/></catalog>").getBytes(StandardCharsets.UTF_8));
            assertFalse(new JSONObject(NativeBridge.start(fixture.root.getAbsolutePath(), fixture.downloads.getAbsolutePath(), ca.getAbsolutePath())).has("error"));
            String token = new JSONObject(NativeBridge.request("/session", "{}", "")).getString("token");
            try {
                command(token, new JSONObject().put("action", "settings").put("destination", fixture.downloads.getAbsolutePath())
                    .put("defaultLocation", state(token).getJSONObject("settings").getString("defaultLocation"))
                    .put("output", new File(fixture.root, "exports").getAbsolutePath()).put("ca", ca.getAbsolutePath()).put("automatic", false));
                command(token, new JSONObject().put("action", "import").put("path", new File(fixture.root, "import.xml").getAbsolutePath()));
                long revision = idle(token).getLong("catalogRevision");
                String saf = command(token, new JSONObject().put("action", "addLocation").put("name", "SAF isolated").put("path", tree.toString())).getString("id");
                File first = fixture.queueSource(fixture.downloads.getAbsolutePath(), "same.bin.part", "abc");
                File second = fixture.queueSource(tree.toString(), "same.bin.part", "def");
                assertNotEquals(first.getParentFile(), second.getParentFile());
                assertEquals(64, first.getParentFile().getName().length());
                assertEquals(first.getParentFile(), NativeIntegrationTest.stagingDirectory(fixture.downloads, fixture.downloads.getAbsolutePath()));
                command(token, new JSONObject().put("action", "enqueue").put("indices", new JSONArray().put(0)).put("revision", revision).put("location", "default"));
                command(token, new JSONObject().put("action", "enqueue").put("indices", new JSONArray().put(1)).put("revision", revision).put("location", saf));
                JSONArray queued = state(token).getJSONArray("queue"); assertEquals(2, queued.length());
                assertEquals(tree.toString(), queued.getJSONObject(1).getString("destination"));
                command(token, new JSONObject().put("action", "start"));
                JSONObject finished = idle(token); assertEquals(finished.toString(), 0, finished.getJSONArray("queue").length());
                assertFalse(first.exists()); assertFalse(second.exists());
                assertEquals("abc", new String(Files.readAllBytes(new File(fixture.downloads, "same.bin").toPath()), StandardCharsets.UTF_8));
                JSONArray library = new JSONObject(NativeBridge.request("/api/library", "{}", token)).getJSONArray("files");
                Uri saved = Uri.parse(library.getJSONObject(1).getString("downloadPath"));
                try (java.io.InputStream input = target.getContentResolver().openInputStream(saved)) {
                    assertNotNull(input);
                    assertEquals('d', input.read()); assertEquals('e', input.read()); assertEquals('f', input.read()); assertEquals(-1, input.read());
                }
                command(token, new JSONObject().put("action", "cleanLibrary").put("mode", "deleteDownloads").put("confirm", "DELETE")
                    .put("indices", new JSONArray().put(0).put(1)).put("revision", revision));
            } finally { NativeBridge.stop(token); }
        }
    }
    @Test public void revokedDefaultLocationStillStartsAndRecoversToPrivate() throws Exception {
        Context instrumentation = InstrumentationRegistry.getInstrumentation().getContext();
        Uri authority = Uri.parse("content://" + instrumentation.getPackageName() + ".storage-fixture-control");
        Bundle created = target.getContentResolver().call(authority, "fixtureCreate", null, null);
        assertNotNull(created); assertFalse(created.getString("error"), created.containsKey("error"));
        Uri tree = Uri.parse(created.getString("tree"));
        target.getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try (Fixture fixture = new Fixture(target)) {
            fixture.storage.request(new JSONObject().put("action", "select").put("tree", tree.toString()));
            File ca = new File(fixture.root, "trust.pem");
            Files.write(new File(fixture.root, "import.xml").toPath(),
                "<catalog complete=\"true\"><item id=\"recovery\"/><file item=\"recovery\" name=\"recovery.bin\" size=\"3\" sha1=\"a9993e364706816aba3e25717850c26c9cd0d89d\" restricted=\"false\"/></catalog>".getBytes(StandardCharsets.UTF_8));
            NativeBridge.initialize(fixture.context);
            String started = NativeBridge.start(fixture.root.getAbsolutePath(), fixture.downloads.getAbsolutePath(), ca.getAbsolutePath());
            assertFalse(started, new JSONObject(started).has("error"));
            String token = new JSONObject(NativeBridge.request("/session", "{}", "")).getString("token");
            JSONObject record = null;
            try {
                command(token, new JSONObject().put("action", "settings").put("destination", tree.toString())
                    .put("output", new File(fixture.root, "exports").getAbsolutePath()).put("ca", ca.getAbsolutePath()).put("automatic", false));
                command(token, new JSONObject().put("action", "import").put("path", new File(fixture.root, "import.xml").getAbsolutePath()));
                long revision = idle(token).getLong("catalogRevision");
                fixture.queueSource(tree.toString(), "recovery.bin", "abc");
                command(token, new JSONObject().put("action", "enqueue").put("indices", new JSONArray().put(0)).put("revision", revision));
                command(token, new JSONObject().put("action", "start"));
                assertEquals(0, idle(token).getJSONArray("queue").length());
                record = new JSONObject(new String(Files.readAllBytes(new File(fixture.root, "download-locations.json").toPath()), StandardCharsets.UTF_8))
                    .getJSONArray("records").getJSONObject(0);
                NativeBridge.stop(token);
                target.getContentResolver().call(authority, "fixtureRevoke", tree.toString(), null);
                Uri document = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
                try (android.database.Cursor ignored = target.getContentResolver().query(document, null, null, null, null)) {
                    fail("Revoked document access must be denied by Android, not only by the storage adapter");
                } catch (SecurityException expected) { assertNotNull(expected.getMessage()); }
                NativeBridge.initialize(fixture.context);
                started = NativeBridge.start(fixture.root.getAbsolutePath(), fixture.downloads.getAbsolutePath(), ca.getAbsolutePath());
                assertFalse(started, new JSONObject(started).has("error"));
                token = new JSONObject(NativeBridge.request("/session", "{}", "")).getString("token");
                assertEquals(tree.toString(), state(token).getJSONObject("settings").getString("destination"));
                JSONObject listing = new JSONObject(NativeBridge.request("/api/library", "{}", token));
                assertFalse(listing.toString(), listing.has("error"));
                JSONObject library = listing.getJSONArray("files").getJSONObject(0);
                assertEquals(library.toString(), "Location unavailable", library.getString("downloadState"));
                revision = state(token).getLong("catalogRevision");
                JSONObject refused = new JSONObject(NativeBridge.request("/api/command", new JSONObject().put("action", "enqueue")
                    .put("indices", new JSONArray().put(0)).put("revision", revision).toString(), token));
                assertTrue(refused.toString(), refused.has("error"));
                JSONObject owned = record;
                rejected(() -> fixture.storage.request(new JSONObject().put("action", "deleteCopy").put("record", owned)), "revoked destination deletion");
                command(token, new JSONObject().put("action", "settings").put("destination", fixture.downloads.getAbsolutePath())
                    .put("defaultLocation", state(token).getJSONObject("settings").getString("defaultLocation"))
                    .put("output", new File(fixture.root, "exports").getAbsolutePath()).put("ca", ca.getAbsolutePath()).put("automatic", false));
                assertEquals(fixture.downloads.getAbsolutePath(), state(token).getJSONObject("settings").getString("destination"));
                assertEquals(fixture.downloads.getAbsolutePath(), new DownloadStorage(fixture.context).tree());
            } finally {
                NativeBridge.stop(token);
                target.getContentResolver().call(authority, "fixtureGrant", tree.toString(), null);
                target.getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                if (record != null) fixture.storage.request(new JSONObject().put("action", "deleteCopy").put("record", record));
            }
        }
    }
    @Test public void privateConfiguredDirectoryAliasesAreSameDestination() throws Exception {
        try (Fixture fixture = new Fixture(target)) {
            String alias = new File(fixture.downloads, "../downloads").getAbsolutePath();
            JSONObject record = fixture.archive(alias, "alias.zip");
            assertEquals("Downloaded", fixture.storage.request(new JSONObject().put("action", "status").put("record", record)).getString("status"));
            fixture.storage.request(new JSONObject().put("action", "validate").put("tree", fixture.downloads.getCanonicalPath()));
            File unrelated = new File(fixture.root, "unrelated"); assertTrue(unrelated.mkdir());
            try {
                fixture.storage.request(new JSONObject().put("action", "validate").put("tree", unrelated.getAbsolutePath()));
                fail("An unrelated raw filesystem destination must still require the picker");
            } catch (IllegalArgumentException expected) { assertNotNull(expected.getMessage()); }
            fixture.storage.request(new JSONObject().put("action", "deleteCopy").put("record", record));
            assertFalse(new File(fixture.downloads, "alias.zip").exists());
        }
    }
    @Test public void metadataPollingDoesNotReadContentsOrEnumerateExtraction() throws Exception {
        Context instrumentation = InstrumentationRegistry.getInstrumentation().getContext();
        Uri authority = Uri.parse("content://" + instrumentation.getPackageName() + ".storage-fixture-control");
        Bundle created = target.getContentResolver().call(authority, "fixtureCreate", null, null);
        assertNotNull(created); assertFalse(created.getString("error"), created.containsKey("error"));
        Uri tree = Uri.parse(created.getString("tree"));
        target.getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try (Fixture fixture = new Fixture(target)) {
            JSONObject record = fixture.extracted(fixture.archive(tree.toString(), "poll.zip"), "poll");
            try {
                Bundle failure = new Bundle(); failure.putBoolean("read", true); failure.putBoolean("children", true);
                target.getContentResolver().call(authority, "fixtureFailures", null, failure);
                JSONObject status = fixture.storage.request(new JSONObject().put("action", "status").put("record", record).put("metadataOnly", true));
                assertEquals(status.toString(), "Downloaded", status.getString("status")); assertTrue(status.has("extracted"));
                rejected(() -> fixture.storage.request(new JSONObject().put("action", "deleteCopy").put("record", record)), "deletion must verify contents");
                failure.putBoolean("children", false);
                target.getContentResolver().call(authority, "fixtureFailures", null, failure);
                JSONObject discovery = fixture.storage.request(new JSONObject().put("action", "discover").put("tree", tree.toString())
                    .put("name", "poll.zip").put("size", 7).put("sha1", sha1("archive")).put("metadataOnly", true));
                assertEquals("Unrecorded", discovery.getString("status")); assertFalse(discovery.has("record"));
                rejected(() -> fixture.storage.request(new JSONObject().put("action", "discover").put("tree", tree.toString())
                    .put("name", "poll.zip").put("size", 7).put("sha1", sha1("archive"))), "verified discovery must read contents");
            } finally {
                target.getContentResolver().call(authority, "fixtureFailures", null, new Bundle());
                fixture.storage.request(new JSONObject().put("action", "deleteCopy").put("record", record));
            }
        }
    }
    @Test public void privateExtractionOwnershipAndArchiveDelete() throws Exception {
        try (Fixture fixture = new Fixture(target)) {
            JSONObject record = fixture.archive(fixture.downloads.getAbsolutePath(), "fixture.zip");
            JSONObject legacy = fixture.storage.request(new JSONObject().put("action", "discover").put("tree", fixture.downloads.getAbsolutePath())
                .put("name", "fixture.zip").put("size", 7).put("sha1", sha1("archive")));
            assertEquals("Unrecorded", legacy.getString("status")); assertFalse(legacy.has("record"));
            record = fixture.extracted(record, "fixture");
            JSONObject saved = record;
            assertEquals("Downloaded", fixture.storage.request(new JSONObject().put("action", "status").put("record", record)).getString("status"));
            File extra = new File(fixture.downloads, "fixture/user.txt"); Files.write(extra.toPath(), new byte[]{1});
            rejected(() -> fixture.storage.request(new JSONObject().put("action", "deleteCopy").put("record", saved)), "unowned file");
            assertTrue(new File(fixture.downloads, "fixture.zip").exists());
            Files.delete(extra.toPath());
            File extraDirectory = new File(fixture.downloads, "fixture/user-folder"); assertTrue(extraDirectory.mkdir());
            rejected(() -> fixture.storage.request(new JSONObject().put("action", "deleteCopy").put("record", saved)), "unowned directory");
            Files.delete(extraDirectory.toPath());
            File payload = new File(fixture.downloads, "fixture/payload.bin"); Files.write(payload.toPath(), "changed".getBytes(StandardCharsets.UTF_8));
            assertEquals("Changed", fixture.storage.request(new JSONObject().put("action", "status").put("record", saved)).getString("status"));
            rejected(() -> fixture.storage.request(new JSONObject().put("action", "deleteArchive").put("record", saved)), "changed extraction");
            Files.write(payload.toPath(), "payload".getBytes(StandardCharsets.UTF_8));
            record = fixture.storage.request(new JSONObject().put("action", "deleteArchive").put("record", record));
            assertFalse(new File(fixture.downloads, "fixture.zip").exists());
            assertEquals("Extracted", fixture.storage.request(new JSONObject().put("action", "status").put("record", record)).getString("status"));
            assertEquals(7, fixture.storage.request(new JSONObject().put("action", "deleteCopy").put("record", record)).getLong("bytes"));
            assertFalse(payload.exists());
        }
    }
    @Test public void privateRefusesExistingDestinationAndUnsafeNames() throws Exception {
        try (Fixture fixture = new Fixture(target)) {
            JSONObject record = fixture.archive(fixture.downloads.getAbsolutePath(), "keep.zip");
            rejected(() -> fixture.archive(fixture.downloads.getAbsolutePath(), "keep.zip"), "existing archive");
            File directory = new File(fixture.downloads, "keep"); assertTrue(directory.mkdir());
            rejected(() -> fixture.extracted(record, "keep"), "existing extraction");
            assertTrue(directory.exists());
            for (String unsafe : new String[]{"../escape", ".", "..", "a/b", "a\\b", "bad\nname"}) {
                try { DownloadStorage.safeName(unsafe); fail("Unsafe name accepted"); }
                catch (IllegalArgumentException expected) { assertNotNull(expected.getMessage()); }
            }
        }
    }
    @Test public void nativeExtractionSafePathsCancellationAndExistingFolder() throws Exception {
        try (Fixture fixture = new Fixture(target)) {
            NativeBridge.initialize(fixture.context);
            File archive = new File(fixture.staging, "native.zip");
            try (ZipOutputStream zip = new ZipOutputStream(Files.newOutputStream(archive.toPath()))) {
                zip.putNextEntry(new ZipEntry("nested/ok.txt")); zip.write("ok".getBytes(StandardCharsets.UTF_8)); zip.closeEntry();
                zip.putNextEntry(new ZipEntry("../escape.txt")); zip.write(1); zip.closeEntry();
            }
            String started = NativeBridge.start(fixture.root.getAbsolutePath(), fixture.downloads.getAbsolutePath(), new File(fixture.root, "trust.pem").getAbsolutePath());
            assertFalse(started, new JSONObject(started).has("error"));
            String token = new JSONObject(NativeBridge.request("/session", "{}", "")).getString("token");
            try {
                File output = new File(fixture.staging, "native");
                JSONObject result = new JSONObject(NativeBridge.extractPrivate(archive.getAbsolutePath(), output.getAbsolutePath(), false));
                assertEquals(result.toString(), "Done", result.getString("result")); assertEquals(1, result.getInt("unsafe"));
                assertTrue(new File(output, "nested/ok.txt").isFile()); assertFalse(new File(fixture.staging, "escape.txt").exists());
                result = new JSONObject(NativeBridge.extractPrivate(archive.getAbsolutePath(), output.getAbsolutePath(), false));
                assertEquals("Failed", result.getString("result")); assertTrue(new File(output, "nested/ok.txt").exists());
                File cancelled = new File(fixture.staging, "cancelled");
                assertEquals("Cancelled", new JSONObject(NativeBridge.extractPrivate(archive.getAbsolutePath(), cancelled.getAbsolutePath(), true)).getString("result"));
                assertFalse(cancelled.exists()); assertTrue(archive.exists());
                assertTrue(new JSONObject(NativeBridge.extractPrivate(archive.getAbsolutePath(), new File(fixture.root, "outside").getAbsolutePath(), false)).has("error"));
            } finally { NativeBridge.stop(token); }
        }
    }
    @Test public void nativeQueueExtractionPreservesArchiveOnExistingDestinationThenPublishesAndDeletes() throws Exception {
        try (Fixture fixture = new Fixture(target)) {
            NativeBridge.initialize(fixture.context);
            File directory = NativeIntegrationTest.stagingDirectory(fixture.downloads, fixture.downloads.getAbsolutePath());
            Files.createDirectories(directory.toPath());
            File archive = new File(directory, "queue.zip");
            try (ZipOutputStream zip = new ZipOutputStream(Files.newOutputStream(archive.toPath()))) {
                zip.putNextEntry(new ZipEntry("payload.txt")); zip.write("payload".getBytes(StandardCharsets.UTF_8)); zip.closeEntry();
            }
            StringBuilder checksum = new StringBuilder();
            for (byte value : java.security.MessageDigest.getInstance("SHA-1").digest(Files.readAllBytes(archive.toPath())))
                checksum.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
            Files.write(new File(fixture.root, "import.xml").toPath(), ("<catalog complete=\"true\"><item id=\"parity-source\"/>" +
                "<file item=\"parity-source\" name=\"queue.zip\" size=\"" + archive.length() + "\" sha1=\"" + checksum +
                "\" restricted=\"false\"/></catalog>").getBytes(StandardCharsets.UTF_8));
            File ca = new File(fixture.root, "trust.pem");
            assertFalse(new JSONObject(NativeBridge.start(fixture.root.getAbsolutePath(), fixture.downloads.getAbsolutePath(), ca.getAbsolutePath())).has("error"));
            String token = new JSONObject(NativeBridge.request("/session", "{}", "")).getString("token");
            try {
                command(token, new JSONObject().put("action", "settings").put("destination", fixture.downloads.getAbsolutePath())
                    .put("output", new File(fixture.root, "exports").getAbsolutePath()).put("ca", ca.getAbsolutePath())
                    .put("automatic", false).put("extractArchives", true).put("deleteExtracted", true));
                command(token, new JSONObject().put("action", "import").put("path", new File(fixture.root, "import.xml").getAbsolutePath()));
                long revision = idle(token).getLong("catalogRevision");
                File existing = new File(fixture.downloads, "queue"); assertTrue(existing.mkdir());
                File userFile = new File(existing, "user.txt"); Files.write(userFile.toPath(), new byte[]{1});
                JSONObject enqueue = new JSONObject().put("action", "enqueue").put("indices", new JSONArray().put(0))
                    .put("revision", revision).put("overwrite", true);
                command(token, enqueue); command(token, new JSONObject().put("action", "start"));
                JSONObject finished = idle(token);
                assertEquals(finished.toString(), 0, finished.getJSONArray("queue").length());
                assertFalse(finished.getString("extractionError").isEmpty());
                assertTrue(userFile.isFile()); assertTrue(new File(fixture.downloads, "queue.zip").isFile());
                JSONObject copy = new JSONObject(NativeBridge.request("/api/library", "{}", token)).getJSONArray("files").getJSONObject(0)
                    .getJSONArray("downloadCopies").getJSONObject(0);
                assertEquals("Downloaded", copy.getString("status")); assertFalse(copy.has("extracted"));
                Files.delete(new File(fixture.downloads, "queue.zip").toPath());
                assertEquals("Missing", new JSONObject(NativeBridge.request("/api/library", "{}", token)).getJSONArray("files").getJSONObject(0).getString("downloadState"));
                command(token, new JSONObject().put("action", "deleteDownload").put("index", 0).put("revision", revision).put("path", copy.getString("path")));
                assertTrue(userFile.exists());
                Files.delete(userFile.toPath()); Files.delete(existing.toPath());
                command(token, enqueue); command(token, new JSONObject().put("action", "start"));
                finished = idle(token); assertEquals(finished.toString(), 0, finished.getJSONArray("queue").length());
                assertFalse(new File(fixture.downloads, "queue.zip").exists());
                assertTrue(new File(fixture.downloads, "queue/payload.txt").isFile());
                JSONObject library = new JSONObject(NativeBridge.request("/api/library", "{}", token)).getJSONArray("files").getJSONObject(0);
                assertEquals("Extracted", library.getString("downloadState"));
                command(token, new JSONObject().put("action", "cleanLibrary").put("mode", "deleteDownloads").put("confirm", "DELETE")
                    .put("indices", new JSONArray().put(0)).put("revision", revision));
                assertFalse(existing.exists());
            } finally { NativeBridge.stop(token); }
        }
    }
    @Test public void mediaStorePublicationCleanupAndChangedCopies() throws Exception {
        Assume.assumeTrue(Build.VERSION.SDK_INT >= 29);
        try (Fixture fixture = new Fixture(target)) {
            String name = "parity-" + UUID.randomUUID() + ".zip", folder = name.substring(0, name.length() - 4);
            JSONObject record = fixture.archive(fixture.storage.defaultLocation(), name);
            try {
                JSONObject legacy = fixture.storage.request(new JSONObject().put("action", "discover").put("tree", fixture.storage.defaultLocation())
                    .put("name", name).put("size", 7).put("sha1", sha1("archive")));
                assertEquals("Downloaded", legacy.getString("status")); assertTrue(legacy.has("record"));
                record = fixture.extracted(record, folder);
                JSONObject saved = record;
                assertEquals("Downloaded", fixture.storage.request(new JSONObject().put("action", "status").put("record", record)).getString("status"));
                Uri file = Uri.parse(record.getJSONObject("storage").getJSONObject("extracted").getJSONArray("entries").getJSONObject(0).getString("uri"));
                try (java.io.OutputStream output = target.getContentResolver().openOutputStream(file, "w")) { assertNotNull(output); output.write("changed".getBytes(StandardCharsets.UTF_8)); }
                rejected(() -> fixture.storage.request(new JSONObject().put("action", "deleteCopy").put("record", saved)), "changed MediaStore copy");
                try (java.io.OutputStream output = target.getContentResolver().openOutputStream(file, "w")) { assertNotNull(output); output.write("payload".getBytes(StandardCharsets.UTF_8)); }
                record = fixture.storage.request(new JSONObject().put("action", "deleteArchive").put("record", record));
                assertEquals("Extracted", fixture.storage.request(new JSONObject().put("action", "status").put("record", record)).getString("status"));
            } finally { fixture.storage.request(new JSONObject().put("action", "deleteCopy").put("record", record)); }
        }
    }
    @Test public void safPermissionPublicationFailureAndScopedCleanup() throws Exception {
        Context instrumentation = InstrumentationRegistry.getInstrumentation().getContext();
        Uri authority = Uri.parse("content://" + instrumentation.getPackageName() + ".storage-fixture-control");
        Bundle created = target.getContentResolver().call(authority, "fixtureCreate", null, null);
        assertNotNull(created); assertFalse(created.getString("error"), created.containsKey("error"));
        Uri tree = Uri.parse(created.getString("tree"));
        target.getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try (Fixture fixture = new Fixture(target)) {
            fixture.storage.request(new JSONObject().put("action", "select").put("tree", tree.toString()));
            assertEquals(tree.toString(), new DownloadStorage(fixture.context).tree());
            JSONObject record = fixture.archive(tree.toString(), "saf.zip");
            record = fixture.extracted(record, "saf");
            JSONObject saved = record;
            Bundle write = new Bundle(); write.putString("text", "user");
            target.getContentResolver().call(authority, "fixtureWrite", created.getString("id") + "/saf/user.txt", write);
            rejected(() -> fixture.storage.request(new JSONObject().put("action", "deleteCopy").put("record", saved)), "unowned SAF file");
            Uri extra = DocumentsContract.buildDocumentUriUsingTree(tree, created.getString("id") + "/saf/user.txt");
            assertTrue(DocumentsContract.deleteDocument(target.getContentResolver(), extra));
            Bundle failure = new Bundle(); failure.putBoolean("delete", true);
            target.getContentResolver().call(authority, "fixtureFailures", null, failure);
            rejected(() -> fixture.storage.request(new JSONObject().put("action", "deleteCopy").put("record", saved)), "SAF delete failure");
            target.getContentResolver().call(authority, "fixtureFailures", null, new Bundle());
            fixture.storage.request(new JSONObject().put("action", "deleteCopy").put("record", record));
            failure = new Bundle(); failure.putBoolean("rename", true);
            target.getContentResolver().call(authority, "fixtureFailures", null, failure);
            rejected(() -> fixture.archive(tree.toString(), "failed.zip"), "SAF rename rollback");
            target.getContentResolver().call(authority, "fixtureFailures", null, new Bundle());
            failure = new Bundle(); failure.putBoolean("write", true);
            target.getContentResolver().call(authority, "fixtureFailures", null, failure);
            rejected(() -> fixture.archive(tree.toString(), "copy-failed.zip"), "SAF write rollback");
            target.getContentResolver().call(authority, "fixtureFailures", null, new Bundle());
            JSONArray files = fixture.storage.request(new JSONObject().put("action", "list").put("tree", tree.toString())).getJSONArray("files");
            assertEquals(files.toString(), 0, files.length());
            JSONObject revoked = fixture.archive(tree.toString(), "revoked.zip");
            fixture.storage.request(new JSONObject().put("action", "deleteCopy").put("record", revoked));
            target.getContentResolver().call(authority, "fixtureRevoke", tree.toString(), null);
            assertEquals("Location unavailable", fixture.storage.request(new JSONObject().put("action", "status").put("record", revoked)).getString("status"));
        } finally {
            target.getContentResolver().call(authority, "fixtureFailures", null, new Bundle());
        }
    }
}
