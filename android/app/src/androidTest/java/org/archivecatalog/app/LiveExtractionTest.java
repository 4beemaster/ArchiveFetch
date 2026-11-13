package org.archivecatalog.app;

import static org.junit.Assert.*;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import android.provider.MediaStore;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.HashMap;
import java.util.Map;
import java.util.UUID;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Assume;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
@androidx.test.filters.SdkSuppress(minSdkVersion = 29)
public final class LiveExtractionTest {
    private static final String ITEM = "alicesadventuresinwonderland_1902_librivox";
    private static final String ARCHIVE = "alicewonderland_1902_jp2.zip";
    private static final String FOLDER = "alicewonderland_1902_jp2";
    private static final long SIZE = 1333886;
    private static final String SHA1 = "3e2b6b7331d338467e54f3c3dff8bdc8c306bbab";
    private static final String MD5 = "7c5f3fff45549bd3ba87ae2c1f042b1b";

    private static String hash(InputStream input, String algorithm) throws Exception {
        MessageDigest digest = MessageDigest.getInstance(algorithm);
        byte[] buffer = new byte[65536];
        for (int count; (count = input.read(buffer)) != -1;) digest.update(buffer, 0, count);
        StringBuilder text = new StringBuilder();
        for (byte value : digest.digest()) text.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        return text.toString();
    }
    private static String member(InputStream input) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        long bytes = 0;
        byte[] buffer = new byte[65536];
        for (int count; (count = input.read(buffer)) != -1;) { digest.update(buffer, 0, count); bytes += count; }
        StringBuilder text = new StringBuilder();
        for (byte value : digest.digest()) text.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        return bytes + ":" + text;
    }
    private static final class Fixture implements AutoCloseable {
        final Context target = InstrumentationRegistry.getInstrumentation().getTargetContext();
        final String prefix = "live-zip-" + UUID.randomUUID();
        final Context context = new ContextWrapper(target) {
            @Override public Context getApplicationContext() { return this; }
            @Override public SharedPreferences getSharedPreferences(String name, int mode) {
                return target.getSharedPreferences(prefix + "-" + name, mode);
            }
        };
        final File root = new File(target.getCacheDir(), prefix);
        final File downloads = new File(root, "downloads"), ca = new File(root, "trust.pem");
        final DownloadStorage storage = new DownloadStorage(context);
        final Uri control = Uri.parse("content://" + InstrumentationRegistry.getInstrumentation().getContext().getPackageName() + ".storage-fixture-control");
        Uri tree, collision, sentinel, unowned;
        String token;
        Fixture() throws Exception {
            assertTrue(root.mkdir());
            CatalogService.writeTrust(ca);
            NativeBridge.initialize(context);
            start();
        }
        Bundle control(String method, String argument) {
            Bundle result = target.getContentResolver().call(control, method, argument, null);
            assertNotNull(result); assertFalse(result.getString("error"), result.containsKey("error")); return result;
        }
        void start() throws Exception {
            JSONObject result = new JSONObject(NativeBridge.start(root.getAbsolutePath(), downloads.getAbsolutePath(), ca.getAbsolutePath()));
            assertFalse(result.toString(), result.has("error"));
            token = new JSONObject(NativeBridge.request("/session", "{}", "")).getString("token");
            storage.request(new JSONObject().put("action", "configure").put("private", downloads.getAbsolutePath()));
        }
        JSONObject request(String path) throws Exception {
            JSONObject result = new JSONObject(NativeBridge.request(path, "{}", token));
            assertEquals(result.toString(), "", result.optString("error")); return result;
        }
        JSONObject command(JSONObject value) throws Exception {
            JSONObject result = new JSONObject(NativeBridge.request("/api/command", value.toString(), token));
            assertFalse(result.toString(), result.has("error")); return result;
        }
        JSONObject idle() throws Exception {
            long deadline = SystemClock.elapsedRealtime() + 180000;
            JSONObject state;
            do {
                state = request("/api/state");
                JSONArray queue = state.getJSONArray("queue");
                for (int index = 0; index < queue.length(); ++index)
                    assertNotEquals(queue.getJSONObject(index).toString(), "Failed", queue.getJSONObject(index).getString("status"));
                if (!state.getBoolean("busy")) return state;
                SystemClock.sleep(50);
            } while (SystemClock.elapsedRealtime() < deadline);
            throw new AssertionError("Live ZIP transfer/extraction timed out: " + state);
        }
        void settings(String destination, boolean extract, boolean delete) throws Exception {
            command(new JSONObject().put("action", "settings").put("destination", destination)
                .put("defaultLocation", request("/api/state").getJSONObject("settings").getString("defaultLocation"))
                .put("output", new File(root, "exports").getAbsolutePath()).put("ca", ca.getAbsolutePath())
                .put("automatic", false).put("connections", 1).put("extractArchives", extract)
                .put("deleteExtracted", delete).put("excludedExtensions", ""));
        }
        JSONObject record(String destination) throws Exception {
            JSONArray records = records();
            for (int index = 0; index < records.length(); ++index)
                if (destination.equals(records.getJSONObject(index).getJSONObject("storage").getString("tree"))) return records.getJSONObject(index);
            throw new AssertionError("Missing live download ownership record for " + destination);
        }
        JSONArray records() throws Exception {
            File state = new File(root, "download-locations.json");
            if (!state.isFile()) return new JSONArray();
            return new JSONObject(new String(Files.readAllBytes(state.toPath()), StandardCharsets.UTF_8)).getJSONArray("records");
        }
        void transfer(String destination, boolean collisionExpected) throws Exception {
            File staging = NativeIntegrationTest.stagingDirectory(downloads, destination);
            assertFalse("The live transfer must not start from a completed staging fixture", new File(staging, ARCHIVE).exists());
            assertFalse("The live transfer must not start from a partial fixture", new File(staging, ARCHIVE + ".part").exists());
            long revision = request("/api/state").getLong("catalogRevision");
            JSONObject enqueue = new JSONObject().put("action", "enqueue").put("indices", new JSONArray().put(0))
                .put("revision", revision).put("location", "auto");
            JSONObject result = command(enqueue);
            assertEquals(result.toString(), collisionExpected, result.has("confirmOverwrite"));
            if (collisionExpected) command(enqueue.put("overwrite", true));
            assertEquals(destination, request("/api/state").getJSONArray("queue").getJSONObject(0).getString("destination"));
            command(new JSONObject().put("action", "start"));
            JSONObject finished = idle();
            assertEquals(finished.toString(), 0, finished.getJSONArray("queue").length());
            if (!collisionExpected) assertEquals(finished.toString(), "", finished.getString("extractionError"));
        }
        void verifyArchive(JSONObject record) throws Exception {
            Uri uri = Uri.parse(record.getJSONObject("storage").getJSONObject("archive").getString("uri"));
            try (InputStream input = target.getContentResolver().openInputStream(uri)) { assertNotNull(input); assertEquals(SHA1, hash(input, "SHA-1")); }
            try (InputStream input = target.getContentResolver().openInputStream(uri)) { assertNotNull(input); assertEquals(MD5, hash(input, "MD5")); }
        }
        @Override public void close() throws Exception {
            NativeBridge.stop(token);
            for (Uri uri : new Uri[]{unowned, sentinel, collision})
                if (uri != null) assertTrue(DocumentsContract.deleteDocument(target.getContentResolver(), uri));
            JSONArray records = records();
            for (int index = 0; index < records.length(); ++index) {
                JSONObject record = records.getJSONObject(index);
                assertEquals(ITEM, record.getString("item")); assertEquals(ARCHIVE, record.getString("name"));
                storage.request(new JSONObject().put("action", "deleteCopy").put("record", record));
            }
            if (tree != null) {
                control("fixtureRemove", tree.toString());
                for (android.content.UriPermission permission : target.getContentResolver().getPersistedUriPermissions())
                    assertNotEquals("The exact live fixture grant must be removed", tree, permission.getUri());
            }
            try (java.util.stream.Stream<java.nio.file.Path> paths = Files.walk(root.toPath())) {
                for (java.nio.file.Path path : paths.sorted(java.util.Comparator.reverseOrder()).toArray(java.nio.file.Path[]::new)) Files.delete(path);
            }
            context.getSharedPreferences("download-location", Context.MODE_PRIVATE).edit().clear().commit();
            context.getSharedPreferences("credentials", Context.MODE_PRIVATE).edit().clear().commit();
        }
    }
    private static Map<String, String> oracle(Fixture fixture) throws Exception {
        File archive = new File(fixture.root, "oracle.zip");
        HttpURLConnection connection = (HttpURLConnection) new URL("https://archive.org/download/" + ITEM + "/" + ARCHIVE).openConnection();
        connection.setConnectTimeout(30000); connection.setReadTimeout(30000);
        try {
            assertEquals(200, connection.getResponseCode());
            try (InputStream input = connection.getInputStream()) { Files.copy(input, archive.toPath()); }
        } finally { connection.disconnect(); }
        assertEquals(SIZE, archive.length());
        try (InputStream input = Files.newInputStream(archive.toPath())) { assertEquals(SHA1, hash(input, "SHA-1")); }
        try (InputStream input = Files.newInputStream(archive.toPath())) { assertEquals(MD5, hash(input, "MD5")); }
        Map<String, String> expected = new HashMap<>();
        try (ZipInputStream zip = new ZipInputStream(Files.newInputStream(archive.toPath()))) {
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null;) {
                String name = entry.getName();
                assertFalse(name, name.startsWith("/") || name.contains("\\") || name.contains("../"));
                if (!entry.isDirectory()) assertNull("Duplicate ZIP member", expected.put(name, member(zip)));
                zip.closeEntry();
            }
        }
        assertFalse("The real ZIP oracle must contain files", expected.isEmpty());
        android.util.Log.i("LiveExtraction", "Independent ZIP oracle SHA1=" + SHA1 + " MD5=" + MD5 + " bytes=" + SIZE + " members=" + expected.size());
        return expected;
    }
    private static void safMembers(Fixture fixture, Uri directory, String prefix, Map<String, String> actual) throws Exception {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(fixture.tree, DocumentsContract.getDocumentId(directory));
        try (Cursor cursor = fixture.target.getContentResolver().query(children, new String[]{DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
            assertNotNull(cursor);
            while (cursor.moveToNext()) {
                Uri uri = DocumentsContract.buildDocumentUriUsingTree(fixture.tree, cursor.getString(0));
                String name = prefix + cursor.getString(1);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2))) safMembers(fixture, uri, name + "/", actual);
                else try (InputStream input = fixture.target.getContentResolver().openInputStream(uri)) {
                    assertNotNull(input); assertNull("Duplicate published member", actual.put(name, member(input)));
                }
            }
        }
    }
    private static void verifyMembers(Fixture fixture, JSONObject record, Map<String, String> expected, boolean media) throws Exception {
        Map<String, String> actual = new HashMap<>();
        if (media) {
            String prefix = "Download/ArchiveFetch/" + FOLDER + "/";
            try (Cursor cursor = fixture.target.getContentResolver().query(MediaStore.Downloads.EXTERNAL_CONTENT_URI,
                    new String[]{MediaStore.Downloads._ID, MediaStore.Downloads.DISPLAY_NAME, MediaStore.Downloads.RELATIVE_PATH},
                    MediaStore.Downloads.RELATIVE_PATH + " LIKE ? ESCAPE '\\'",
                    new String[]{prefix.replace("_", "\\_") + "%"}, null)) {
                assertNotNull(cursor);
                while (cursor.moveToNext()) {
                    Uri uri = android.content.ContentUris.withAppendedId(MediaStore.Downloads.EXTERNAL_CONTENT_URI, cursor.getLong(0));
                    String name = cursor.getString(2).substring(prefix.length()) + cursor.getString(1);
                    try (InputStream input = fixture.target.getContentResolver().openInputStream(uri)) {
                        assertNotNull(input); assertNull("Duplicate published member", actual.put(name, member(input)));
                    }
                }
            }
        } else safMembers(fixture, Uri.parse(record.getJSONObject("storage").getJSONObject("extracted").getString("uri")), "", actual);
        assertEquals("Every actual published member must match the independent Java ZIP oracle", expected, actual);
        for (Map.Entry<String, String> entry : actual.entrySet())
            android.util.Log.i("LiveExtraction", (media ? "MediaStore" : "SAF") + " member=" + entry.getKey() + " bytes:SHA256=" + entry.getValue());
    }
    private static Uri sentinel(Fixture fixture, Uri directory) throws Exception {
        Uri uri = DocumentsContract.createDocument(fixture.target.getContentResolver(), directory, "text/plain", "live-user-sentinel.txt");
        assertNotNull(uri);
        try (java.io.OutputStream output = fixture.target.getContentResolver().openOutputStream(uri, "w")) {
            assertNotNull(output); output.write("keep".getBytes(StandardCharsets.UTF_8));
        }
        return uri;
    }
    private static void sentinelUnchanged(Fixture fixture, Uri uri) throws Exception {
        try (InputStream input = fixture.target.getContentResolver().openInputStream(uri)) {
            assertNotNull(input); assertEquals('k', input.read()); assertEquals('e', input.read());
            assertEquals('e', input.read()); assertEquals('p', input.read()); assertEquals(-1, input.read());
        }
    }
    @Test public void liveHttpsZipExtractionMediaStoreAndNamedSaf() throws Exception {
        Assume.assumeTrue("Requires explicit public-network approval on the owned disposable candidate",
            "true".equals(InstrumentationRegistry.getArguments().getString("allowLiveExtraction")));
        assertTrue("This live test specifically validates public MediaStore Downloads", android.os.Build.VERSION.SDK_INT >= 29);
        try (Fixture fixture = new Fixture()) {
            String publicLocation = fixture.request("/api/state").getJSONObject("settings").getString("destination");
            assertEquals(fixture.storage.defaultLocation(), publicLocation);
            assertFalse("Never replace an existing public archive or folder",
                fixture.storage.request(new JSONObject().put("action", "exists").put("tree", publicLocation).put("name", ARCHIVE).put("folder", FOLDER)).getBoolean("exists"));
            Map<String, String> expected = oracle(fixture);
            Files.write(new File(fixture.root, "import.xml").toPath(), ("<catalog complete=\"true\"><item id=\"" + ITEM + "\"/>" +
                "<file item=\"" + ITEM + "\" name=\"" + ARCHIVE + "\" size=\"" + SIZE + "\" sha1=\"" + SHA1 + "\" md5=\"" + MD5 +
                "\" restricted=\"false\"/></catalog>").getBytes(StandardCharsets.UTF_8));
            fixture.settings(publicLocation, true, false);
            fixture.command(new JSONObject().put("action", "import").put("path", new File(fixture.root, "import.xml").getAbsolutePath()));
            fixture.idle();
            fixture.transfer(publicLocation, false);
            JSONObject mediaRecord = fixture.record(publicLocation);
            fixture.verifyArchive(mediaRecord); verifyMembers(fixture, mediaRecord, expected, true);
            fixture.tree = Uri.parse(fixture.control("fixtureCreate", null).getString("tree"));
            fixture.target.getContentResolver().takePersistableUriPermission(fixture.tree,
                Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            String location = fixture.command(new JSONObject().put("action", "addLocation").put("name", "Live ZIP SAF")
                .put("path", fixture.tree.toString()).put("tags", new JSONArray().put("live-zip")).put("extract", true)).getString("id");
            fixture.settings(publicLocation, false, true);
            long revision = fixture.request("/api/state").getLong("catalogRevision");
            fixture.command(new JSONObject().put("action", "fileTags").put("index", 0).put("revision", revision).put("tags", new JSONArray().put("live-zip")));
            Uri treeDocument = DocumentsContract.buildDocumentUriUsingTree(fixture.tree, DocumentsContract.getTreeDocumentId(fixture.tree));
            fixture.collision = DocumentsContract.createDocument(fixture.target.getContentResolver(), treeDocument, DocumentsContract.Document.MIME_TYPE_DIR, FOLDER);
            assertNotNull(fixture.collision); fixture.sentinel = sentinel(fixture, fixture.collision);
            fixture.transfer(fixture.tree.toString(), true);
            assertFalse(fixture.request("/api/state").getString("extractionError").isEmpty());
            JSONObject failed = fixture.record(fixture.tree.toString());
            assertTrue(failed.getJSONObject("storage").has("archive")); assertFalse(failed.getJSONObject("storage").has("extracted"));
            fixture.verifyArchive(failed); sentinelUnchanged(fixture, fixture.sentinel);
            fixture.command(new JSONObject().put("action", "deleteDownload").put("index", 0).put("revision", revision).put("path", failed.getString("path")));
            sentinelUnchanged(fixture, fixture.sentinel);
            assertTrue(DocumentsContract.deleteDocument(fixture.target.getContentResolver(), fixture.sentinel)); fixture.sentinel = null;
            assertTrue(DocumentsContract.deleteDocument(fixture.target.getContentResolver(), fixture.collision)); fixture.collision = null;
            File staged = new File(NativeIntegrationTest.stagingDirectory(fixture.downloads, fixture.tree.toString()), ARCHIVE);
            assertTrue(staged.isFile()); Files.delete(staged.toPath());
            fixture.transfer(fixture.tree.toString(), false);
            JSONObject safRecord = fixture.record(fixture.tree.toString());
            assertFalse("Delete the archive only after successful published extraction", safRecord.getJSONObject("storage").has("archive"));
            assertFalse("Archive deletion must remove the actual SAF document",
                fixture.storage.request(new JSONObject().put("action", "exists").put("tree", fixture.tree.toString()).put("name", ARCHIVE)).getBoolean("exists"));
            verifyMembers(fixture, safRecord, expected, false);
            NativeBridge.stop(fixture.token); NativeBridge.initialize(fixture.context); fixture.start();
            JSONObject settings = fixture.request("/api/state").getJSONObject("settings");
            assertEquals(publicLocation, settings.getString("destination")); assertFalse(settings.getBoolean("extractArchives"));
            boolean retained = false;
            JSONArray locations = settings.getJSONArray("locations");
            for (int index = 0; index < locations.length(); ++index)
                if (location.equals(locations.getJSONObject(index).getString("id"))) retained = locations.getJSONObject(index).getBoolean("extract");
            assertTrue("Per-location extraction must survive restart", retained);
            JSONObject library = fixture.request("/api/library").getJSONArray("files").getJSONObject(0);
            assertEquals(2, library.getJSONArray("downloadCopies").length());
            int downloaded = 0, extracted = 0;
            JSONArray copies = library.getJSONArray("downloadCopies");
            for (int index = 0; index < copies.length(); ++index) {
                JSONObject copy = copies.getJSONObject(index);
                assertTrue(copy.toString(), copy.has("extracted"));
                if ("Downloaded".equals(copy.getString("status"))) ++downloaded;
                else if ("Extracted".equals(copy.getString("status"))) ++extracted;
                else fail("Unexpected restarted copy state: " + copy);
            }
            assertEquals(1, downloaded); assertEquals(1, extracted);
            assertEquals(2, fixture.records().length());
            fixture.verifyArchive(fixture.record(publicLocation));
            verifyMembers(fixture, fixture.record(publicLocation), expected, true);
            safRecord = fixture.record(fixture.tree.toString()); verifyMembers(fixture, safRecord, expected, false);
            fixture.unowned = sentinel(fixture, Uri.parse(safRecord.getJSONObject("storage").getJSONObject("extracted").getString("uri")));
            revision = fixture.request("/api/state").getLong("catalogRevision");
            JSONObject delete = new JSONObject().put("action", "deleteDownload").put("index", 0).put("revision", revision).put("path", safRecord.getString("path"));
            JSONObject refused = new JSONObject(NativeBridge.request("/api/command", delete.toString(), fixture.token));
            assertTrue(refused.toString(), refused.has("error")); sentinelUnchanged(fixture, fixture.unowned);
            assertTrue(DocumentsContract.deleteDocument(fixture.target.getContentResolver(), fixture.unowned)); fixture.unowned = null;
            verifyMembers(fixture, safRecord, expected, false);
            JSONObject preview = fixture.command(new JSONObject().put("action", "cleanLibrary").put("preview", true)
                .put("indices", new JSONArray().put(0)).put("revision", revision)).getJSONObject("delete");
            assertEquals(2, preview.getInt("files")); assertEquals(2, preview.getInt("folders"));
            long extractedBytes = 0;
            for (String value : expected.values()) extractedBytes += Long.parseLong(value.substring(0, value.indexOf(':')));
            assertEquals(SIZE + 2 * extractedBytes, preview.getLong("bytes"));
            JSONObject clean = new JSONObject().put("action", "cleanLibrary").put("mode", "deleteDownloads")
                .put("indices", new JSONArray().put(0)).put("revision", revision);
            refused = new JSONObject(NativeBridge.request("/api/command", clean.toString(), fixture.token));
            assertTrue(refused.toString(), refused.has("error")); assertEquals(2, fixture.records().length());
            fixture.command(clean.put("confirm", "DELETE"));
            assertEquals(0, fixture.records().length());
            assertFalse(fixture.storage.request(new JSONObject().put("action", "exists").put("tree", publicLocation).put("name", ARCHIVE).put("folder", FOLDER)).getBoolean("exists"));
            try (Cursor cursor = fixture.target.getContentResolver().query(
                    DocumentsContract.buildChildDocumentsUriUsingTree(fixture.tree, DocumentsContract.getTreeDocumentId(fixture.tree)), null, null, null, null)) {
                assertNotNull(cursor); assertEquals(0, cursor.getCount());
            }
            android.util.Log.i("LiveExtraction", "PASS: actual HTTPS, all members in MediaStore/SAF, archive retention/deletion, restart history and confirmed scoped cleanup");
        }
    }
}
