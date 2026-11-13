package org.archivecatalog.app;

import static org.junit.Assert.*;
import android.app.Instrumentation;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.os.Environment;
import android.os.SystemClock;
import android.view.View;
import android.view.ViewGroup;
import android.webkit.WebView;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.uiautomator.By;
import androidx.test.uiautomator.UiDevice;
import androidx.test.uiautomator.UiObject2;
import androidx.test.uiautomator.Until;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class NativeIntegrationTest {
    private static final String MANIFEST = "<catalog complete=\"true\"><item id=\"archive-native-test\"/>"
        + "<file item=\"archive-native-test\" name=\"Alpha &#x1F3AE;.chd\" size=\"3\" sha1=\"a9993e364706816aba3e25717850c26c9cd0d89d\" restricted=\"false\"/>"
        + "<file item=\"archive-native-test\" name=\"Beta.chd\" size=\"3\" md5=\"900150983cd24fb0d6963f7d28e17f72\" restricted=\"true\"/></catalog>";
    private static final String ACCESS = "fixture-access-only";
    private static final String SECRET = "fixture-secret-only";
    private String token;

    private static void write(File file, String text) throws Exception {
        File parent = file.getParentFile();
        assertTrue(parent != null && (parent.isDirectory() || parent.mkdirs()));
        try (FileOutputStream stream = new FileOutputStream(file)) { stream.write(text.getBytes(StandardCharsets.UTF_8)); }
    }
    static File stagingDirectory(File downloads, String destination) throws Exception {
        StringBuilder hash = new StringBuilder();
        for (byte value : java.security.MessageDigest.getInstance("SHA-256").digest(destination.getBytes(StandardCharsets.UTF_8)))
            hash.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        return new File(new File(downloads, ".staging"), hash.toString());
    }
    private JSONObject request(String path) throws Exception { return new JSONObject(NativeBridge.request(path, "{}", token)); }
    private JSONObject command(JSONObject value) throws Exception { return new JSONObject(NativeBridge.request("/api/command", value.toString(), token)); }
    private JSONObject stateWhenIdle() throws Exception {
        long deadline = SystemClock.elapsedRealtime() + 15000;
        JSONObject state;
        do {
            state = request("/api/state");
            assertFalse(state.toString(), state.has("error") && !state.has("busy"));
            if (!state.getBoolean("busy")) return state;
            Thread.sleep(25);
        } while (SystemClock.elapsedRealtime() < deadline);
        throw new AssertionError("Native operation did not become idle: " + state);
    }
    private static void clickPickerButton(UiDevice device, androidx.test.uiautomator.BySelector selector, String message) {
        for (int attempt = 0; ; ++attempt) {
            UiObject2 button = device.wait(Until.findObject(selector), 10000);
            assertNotNull(message, button);
            try { button.click(); return; }
            catch (androidx.test.uiautomator.StaleObjectException stale) { if (attempt == 4) throw stale; }
        }
    }
    private void start(File root, File downloads, File ca) throws Exception {
        JSONObject result = new JSONObject(NativeBridge.start(root.getAbsolutePath(), downloads.getAbsolutePath(), ca.getAbsolutePath()));
        assertFalse(result.toString(), result.has("error"));
        token = new JSONObject(NativeBridge.request("/session", "{}", "")).getString("token");
    }
    @Test public void nativeTrustDiagnostics() throws Exception {
        Context target = InstrumentationRegistry.getInstrumentation().getTargetContext();
        NativeBridge.initialize(target);
        File root = new File(target.getCacheDir(), "diagnostics_" + UUID.randomUUID());
        assertTrue(root.mkdirs());
        File ca = new File(root, "trust.pem");
        start(root, new File(root, "downloads"), ca);
        try {
            String log = request("/api/diagnostics").getString("log");
            assertTrue(log, log.contains("Cannot load CA certificate bundle"));
            assertFalse(log, log.contains("[tls]"));
        } finally { NativeBridge.stop(token); }
        write(ca, "not a certificate bundle");
        start(root, new File(root, "downloads"), ca);
        try {
            String log = request("/api/diagnostics").getString("log");
            assertTrue(log, log.contains("Cannot load CA certificate bundle"));
            assertFalse(log, log.contains("[tls]"));
        } finally { NativeBridge.stop(token); }
        CatalogService.writeTrust(ca);
        String pem = new String(java.nio.file.Files.readAllBytes(ca.toPath()), StandardCharsets.US_ASCII);
        assertFalse("CA export must not contain blank lines inside PEM blocks", pem.contains("\n\n"));
        write(ca, pem.replace("\n-----END CERTIFICATE-----", "\n\n-----END CERTIFICATE-----"));
        start(root, new File(root, "downloads"), ca);
        try {
            String log = request("/api/diagnostics").getString("log");
            assertTrue(log, log.contains("Cannot load CA certificate bundle"));
        } finally { NativeBridge.stop(token); }
        write(ca, pem);
        start(root, new File(root, "downloads"), ca);
        try {
            String log = request("/api/diagnostics").getString("log");
            assertFalse(log, log.contains("Cannot load CA certificate bundle"));
            assertFalse(log, log.contains("[tls]"));
            assertFalse(log, log.contains("CA readable="));
            assertTrue(log, log.contains("[android] App version=" + NativeBridge.appVersion()));
            assertTrue(command(new JSONObject().put("action", "settings")
                .put("destination", new File(root, "downloads").getAbsolutePath())
                .put("output", new File(root, "exports").getAbsolutePath()).put("ca", ca.getAbsolutePath())
                .put("automatic", false).put("remember", false).put("access", ACCESS).put("secret", SECRET)).getBoolean("ok"));
            log = request("/api/diagnostics").getString("log");
            assertFalse(log.contains(ACCESS)); assertFalse(log.contains(SECRET));
            request("/api/diagnostics/clear");
            assertEquals("", request("/api/diagnostics").getString("log"));
        } finally { NativeBridge.stop(token); }
    }
    // A stand-in console: /session, a password sign-in cookie and the session token, as the PS5 payload does.
    private static void serveConsole(java.net.ServerSocket server, java.util.concurrent.atomic.AtomicBoolean outside, java.util.Map<String, String> devices) {
        while (!server.isClosed()) {
            try (java.net.Socket socket = server.accept()) {
                java.io.BufferedReader reader = new java.io.BufferedReader(new java.io.InputStreamReader(socket.getInputStream(), StandardCharsets.UTF_8));
                String[] start = reader.readLine().split(" ");
                java.util.Map<String, String> headers = new java.util.HashMap<>();
                for (String line = reader.readLine(); line != null && !line.isEmpty(); line = reader.readLine()) {
                    int colon = line.indexOf(':');
                    headers.put(line.substring(0, colon).trim().toLowerCase(java.util.Locale.ROOT), line.substring(colon + 1).trim());
                }
                char[] body = new char[Integer.parseInt(headers.getOrDefault("content-length", "0"))];
                for (int read = 0; read < body.length; ) read += reader.read(body, read, body.length - read);
                String device = headers.getOrDefault("x-archivefetch-device", "");
                boolean paired = device.length() == 81 && device.substring(17).equals(devices.get(device.substring(0, 16)));
                boolean signedIn = "ArchiveFetchAuth=granted".equals(headers.get("cookie")) || paired;
                int status = 200; String cookie = null, reply;
                if (start[1].equals("/session")) {
                    if (outside.get() && !signedIn) { status = 401; reply = "{\"error\":\"Sign in required\",\"login\":true,\"version\":\"0.6.0\",\"pairing\":true}"; }
                    else reply = "{\"token\":\"console-token\",\"version\":\"0.6.0\",\"pairing\":true,\"paired\":" + paired + "}";
                } else if (start[1].equals("/api/login")) {
                    if (new JSONObject(new String(body)).optString("password").equals("console-password")) { cookie = "ArchiveFetchAuth=granted; Path=/; HttpOnly"; reply = "{\"ok\":true}"; }
                    else { status = 401; reply = "{\"error\":\"Wrong password\"}"; }
                } else if (outside.get() && !signedIn) { status = 401; reply = "{\"error\":\"Sign in required\",\"login\":true}"; }
                else if (!"console-token".equals(headers.get("x-archive-session"))) { status = 403; reply = "{\"error\":\"Access denied\"}"; }
                else if (start[1].equals("/api/pair")) {
                    String id = UUID.randomUUID().toString().replace("-", "").substring(0, 16), key = (UUID.randomUUID().toString() + UUID.randomUUID()).replace("-", "");
                    devices.put(id, key);
                    reply = new JSONObject().put("id", id).put("key", key).toString();
                }
                else reply = "{\"busy\":false,\"from\":\"console\",\"host\":\"" + headers.get("host") + "\"}";
                byte[] bytes = reply.getBytes(StandardCharsets.UTF_8);
                String head = "HTTP/1.1 " + status + " X\r\nContent-Type: application/json\r\nContent-Length: " + bytes.length + "\r\nConnection: close\r\n"
                    + (cookie == null ? "" : "Set-Cookie: " + cookie + "\r\n") + "\r\n";
                socket.getOutputStream().write(head.getBytes(StandardCharsets.US_ASCII)); socket.getOutputStream().write(bytes);
            } catch (Exception ignored) { }
        }
    }
    private JSONObject consoles(JSONObject value) throws Exception { return new JSONObject(NativeBridge.request("/api/consoles", value.toString(), token)); }
    private JSONObject relay(String id, String path, JSONObject body) throws Exception {
        JSONObject value = new JSONObject().put("console", id).put("path", path);
        if (body != null) value.put("body", body);
        return new JSONObject(NativeBridge.request("/api/relay", value.toString(), token));
    }
    @Test public void nativeConsoleRelay() throws Exception {
        Context target = InstrumentationRegistry.getInstrumentation().getTargetContext();
        NativeBridge.initialize(target);
        File root = new File(target.getCacheDir(), "consoles_" + UUID.randomUUID());
        assertTrue(root.mkdirs());
        java.util.concurrent.atomic.AtomicBoolean outside = new java.util.concurrent.atomic.AtomicBoolean(false);
        java.util.Map<String, String> devices = new java.util.concurrent.ConcurrentHashMap<>();
        try (java.net.ServerSocket server = new java.net.ServerSocket(0, 16, java.net.InetAddress.getByName("127.0.0.1"))) {
            Thread thread = new Thread(() -> serveConsole(server, outside, devices)); thread.setDaemon(true); thread.start();
            start(root, new File(root, "downloads"), new File(root, "trust.pem"));
            try {
                assertTrue(request("/session").getBoolean("consoles"));
                JSONObject added = consoles(new JSONObject().put("action", "add").put("name", "Test").put("address", "127.0.0.1").put("port", server.getLocalPort()));
                assertFalse(added.toString(), added.has("error"));
                String id = added.getJSONArray("consoles").getJSONObject(0).getString("id");
                JSONObject state = relay(id, "/api/state", null);
                assertEquals(state.toString(), "console", state.optString("from"));
                assertEquals("127.0.0.1:" + server.getLocalPort(), state.getString("host"));
                // The first connection pairs; removing the device on the console makes it sign in again.
                assertEquals(1, devices.size());
                devices.clear();
                outside.set(true);
                assertTrue(relay(id, "/api/state", null).optBoolean("login"));
                JSONObject wrong = relay(id, "/api/login", new JSONObject().put("password", "wrong"));
                assertEquals(wrong.toString(), "Wrong password", wrong.optString("error"));
                assertEquals(401, wrong.getInt("status"));
                assertTrue(relay(id, "/api/login", new JSONObject().put("password", "console-password")).optBoolean("ok"));
                assertEquals("console", relay(id, "/api/state", null).optString("from"));
                assertTrue(relay(id, "/status", null).has("error"));
                assertEquals(1, devices.size());
                assertFalse(new String(java.nio.file.Files.readAllBytes(new File(root, "consoles.json").toPath()), StandardCharsets.UTF_8)
                    .contains(devices.values().iterator().next()));
                NativeBridge.stop(token);
                // A restarted app has no sign-in cookie; the Keystore-protected pairing key lets it in.
                start(root, new File(root, "downloads"), new File(root, "trust.pem"));
                assertEquals("console", relay(id, "/api/state", null).optString("from"));
                assertTrue(consoles(new JSONObject()).getJSONArray("consoles").getJSONObject(0).getBoolean("paired"));
                assertEquals(0, consoles(new JSONObject().put("action", "remove").put("id", id)).getJSONArray("consoles").length());
            } finally { NativeBridge.stop(token); }
        }
    }
    @Test public void nativeHttpsTrustAnchors() throws Exception {
        org.junit.Assume.assumeTrue("true".equals(InstrumentationRegistry.getArguments().getString("liveHttps")));
        Context target = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File root = new File(target.getCacheDir(), "tls_" + UUID.randomUUID());
        assertTrue(root.mkdirs());
        File ca = new File(root, "android-trust.pem");
        CatalogService.writeTrust(ca);
        assertTrue(ca.length() > 0);
        NativeBridge.initialize(target);
        start(root, new File(root, "downloads"), ca);
        try {
            JSONObject result = command(new JSONObject().put("action", "addSource")
                .put("url", "https://archive.org/details/opensource").put("related", false));
            assertFalse(result.toString(), result.has("error"));
            JSONObject state = stateWhenIdle();
            assertEquals(state.toString(), "", state.optString("error"));
            assertTrue(state.toString(), state.getJSONArray("sources").length() > 0);
            String log = request("/api/diagnostics").getString("log");
            assertFalse(log, log.contains("Cannot load CA certificate bundle"));
            assertFalse(log, log.contains("[tls]"));
            assertFalse(log, log.contains("[network] Request started"));
            assertFalse(log, log.contains("[network] Request finished"));
            JSONObject updates = command(new JSONObject().put("action", "checkUpdates").put("includeBeta", false));
            assertFalse(updates.toString(), updates.has("error"));
            assertEquals(NativeBridge.appVersion(), updates.getString("currentVersion"));
            assertEquals("android", updates.getString("platform"));
            String release = updates.getJSONObject("release").getString("url");
            assertEquals(release, MainActivity.updateLink(release).toString());
            JSONObject opened = command(new JSONObject().put("action", "openUpdate").put("kind", "release")
                .put("version", updates.getString("latestVersion"))
                .put("url", "https://example.invalid"));
            assertEquals(release, opened.getString("openUpdate"));
            assertTrue(command(new JSONObject().put("action", "openUpdate").put("kind", "https://example.invalid")
                .put("version", updates.getString("latestVersion"))).has("error"));
            for (String invalid : new String[]{"http://github.com/4beemaster/ArchiveFetch/releases/tag/v0.8.0",
                    "https://github.com.evil.invalid/4beemaster/ArchiveFetch/releases/tag/v0.8.0",
                    "https://github.com/other/repo/releases/tag/v0.8.0",
                    "https://github.com/4beemaster/ArchiveFetch/releases/tag/v0.8.0?next=1",
                    "https://github.com/4beemaster/ArchiveFetch/releases/download/v0.8.0/../evil.exe"}) {
                boolean rejected = false;
                try { MainActivity.updateLink(invalid); } catch (IllegalArgumentException expected) { rejected = true; }
                assertTrue("Unexpected release URL accepted", rejected);
            }
        } finally { NativeBridge.stop(token); }
    }
    @Test public void nativeDefaultDownloads() throws Exception {
        Context target = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String prefix = "default_" + UUID.randomUUID();
        Context isolated = new ContextWrapper(target) {
            @Override public Context getApplicationContext() { return this; }
            @Override public SharedPreferences getSharedPreferences(String name, int mode) { return target.getSharedPreferences(prefix + name, mode); }
        };
        NativeBridge.initialize(isolated);
        DownloadStorage storage = new DownloadStorage(isolated);
        String expected = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), DownloadStorage.APP_NAME).getAbsolutePath();
        assertEquals(expected, storage.tree());
        SharedPreferences location = isolated.getSharedPreferences("download-location", Context.MODE_PRIVATE);
        for (String legacy : new String[]{"PS5 ArchiveBridge", "ArchiveBridge"}) {
            location.edit().putString("tree", new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), legacy).getAbsolutePath()).commit();
            assertEquals(expected, storage.tree());
        }
        String custom = "content://com.android.externalstorage.documents/tree/primary%3ADownload%2FArchiveBridge";
        location.edit().putString("tree", custom).commit();
        assertEquals(custom, storage.tree());
        location.edit().remove("tree").commit();
        File root = new File(target.getCacheDir(), prefix);
        File downloads = new File(root, "staging");
        String name = "Alpha-" + prefix + ".chd";
        start(root, downloads, new File(root, "trust.pem"));
        try {
            assertEquals(expected, request("/api/state").getJSONObject("settings").getString("destination"));
            assertTrue(command(new JSONObject().put("action", "settings").put("destination", expected)
                .put("output", new File(root, "exports").getAbsolutePath()).put("ca", new File(root, "trust.pem").getAbsolutePath())
                .put("automatic", false).put("remember", false)).getBoolean("ok"));
            write(new File(root, "import.xml"), MANIFEST.replace("Alpha &#x1F3AE;.chd", name));
            assertTrue(command(new JSONObject().put("action", "import").put("path", new File(root, "import.xml").getAbsolutePath())).getBoolean("ok"));
            long revision = stateWhenIdle().getLong("catalogRevision");
            for (int attempt = 0; attempt < 2; ++attempt) {
                write(new File(stagingDirectory(downloads, expected), name + ".part"), "abc");
                assertTrue(command(new JSONObject().put("action", "enqueue").put("indices", new JSONArray().put(0))
                    .put("revision", revision).put("overwrite", attempt > 0)).getBoolean("ok"));
                assertTrue(command(new JSONObject().put("action", "start")).getBoolean("ok"));
                JSONObject state = stateWhenIdle();
                assertEquals(state.toString(), 0, state.getJSONArray("queue").length());
                assertFalse(new File(downloads, name).exists());
                assertTrue(request("/api/library").getJSONArray("files").getJSONObject(0).getBoolean("downloaded"));
                JSONObject share = command(new JSONObject().put("action", "share").put("indices", new JSONArray().put(0)).put("revision", revision));
                android.net.Uri uri = android.net.Uri.parse(share.getJSONArray("shareFiles").getString(0));
                assertEquals("media", uri.getAuthority());
                try (android.database.Cursor cursor = target.getContentResolver().query(uri,
                    new String[]{android.provider.MediaStore.Downloads.RELATIVE_PATH, android.provider.MediaStore.Downloads.DISPLAY_NAME, android.provider.MediaStore.Downloads.IS_PENDING}, null, null, null)) {
                    assertNotNull(cursor); assertTrue(cursor.moveToFirst());
                    assertEquals("Download/ArchiveFetch/", cursor.getString(0));
                    assertEquals(name, cursor.getString(1)); assertEquals(0, cursor.getInt(2));
                }
                try (java.io.InputStream input = target.getContentResolver().openInputStream(uri)) {
                    assertNotNull(input); assertEquals('a', input.read()); assertEquals('b', input.read()); assertEquals('c', input.read()); assertEquals(-1, input.read());
                }
                assertTrue(command(new JSONObject().put("action", "enqueue").put("indices", new JSONArray().put(0)).put("revision", revision)).has("confirmOverwrite"));
            }
            assertEquals(expected, new DownloadStorage(isolated).tree());
        } finally {
            NativeBridge.stop(token);
            JSONArray files = storage.request(new JSONObject().put("action", "list")).getJSONArray("files");
            for (int index = 0; index < files.length(); ++index) {
                JSONObject file = files.getJSONObject(index);
                if (name.equals(file.getString("name"))) target.getContentResolver().delete(android.net.Uri.parse(file.getString("uri")), null, null);
            }
            isolated.getSharedPreferences("download-location", Context.MODE_PRIVATE).edit().clear().commit();
        }
    }
    @Test public void nativePersistenceChecksumsAndWebView() throws Exception {
        Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        Context target = instrumentation.getTargetContext();
        String prefix = "test_" + UUID.randomUUID() + "_";
        Context isolated = new ContextWrapper(target) {
            @Override public Context getApplicationContext() { return this; }
            @Override public SharedPreferences getSharedPreferences(String name, int mode) { return target.getSharedPreferences(prefix + name, mode); }
        };
        NativeBridge.initialize(isolated);
        File root = new File(target.getCacheDir(), prefix);
        File downloads = new File(root, "downloads");
        File ca = new File(root, "trust.pem");
        String betaName = "Beta-" + prefix + ".chd";
        start(root, downloads, ca);
        try {
            assertTrue(new JSONObject(NativeBridge.request("/api/state", "{}", "wrong-token")).has("error"));
            JSONObject settings = new JSONObject().put("action", "settings").put("destination", downloads.getAbsolutePath())
                .put("output", new File(root, "exports").getAbsolutePath()).put("ca", ca.getAbsolutePath()).put("automatic", false)
                .put("remember", true).put("access", ACCESS).put("secret", SECRET);
            assertTrue(command(settings).getBoolean("ok"));
            String sealed = isolated.getSharedPreferences("credentials", Context.MODE_PRIVATE).getString("sealed", "");
            assertFalse(sealed.isEmpty()); assertFalse(sealed.contains(ACCESS)); assertFalse(sealed.contains(SECRET));
            JSONObject state = request("/api/state");
            assertTrue(state.getJSONObject("settings").getBoolean("authenticated"));
            assertFalse(state.toString().contains(ACCESS)); assertFalse(state.toString().contains(SECRET));
            assertTrue(command(new JSONObject().put("action", "import").put("path", "/etc/passwd")).has("error"));
            write(new File(root, "import.xml"), MANIFEST.replace("Beta.chd", betaName));
            assertTrue(command(new JSONObject().put("action", "import").put("path", new File(root, "import.xml").getAbsolutePath())).getBoolean("ok"));
            state = stateWhenIdle();
            JSONArray files = request("/api/library").getJSONArray("files");
            assertEquals(2, files.length()); assertEquals("Alpha \uD83C\uDFAE.chd", files.getJSONObject(0).getString("name"));
            long revision = state.getLong("catalogRevision");
            assertTrue(command(new JSONObject().put("action", "enqueue").put("indices", new JSONArray().put(1)).put("revision", revision + 1)).has("error"));
            assertTrue(command(new JSONObject().put("action", "favorite").put("index", 0).put("revision", revision)).getBoolean("ok"));
            assertTrue(command(new JSONObject().put("action", "enqueue").put("indices", new JSONArray().put(1)).put("revision", revision)).getBoolean("ok"));
            assertEquals(1, request("/api/state").getJSONArray("queue").length());
            write(new File(stagingDirectory(downloads, request("/api/state").getJSONObject("settings").getString("destination")), betaName), "abc");
            assertTrue(command(new JSONObject().put("action", "start")).getBoolean("ok"));
            state = stateWhenIdle();
            assertEquals(state.toString(), 0, state.getJSONArray("queue").length());
            assertTrue(state.getLong("downloadRevision") > 0);
            assertTrue(command(new JSONObject().put("action", "enqueue").put("indices", new JSONArray().put(1)).put("revision", revision)).has("confirmOverwrite"));
            assertTrue(command(new JSONObject().put("action", "export")).getJSONArray("shareFiles").length() == 3);
            String previous = token;
            NativeBridge.stop(token);
            start(root, downloads, ca);
            state = request("/api/state");
            assertTrue(state.getJSONObject("settings").getBoolean("remember"));
            files = request("/api/library").getJSONArray("files");
            assertEquals(2, files.length()); assertTrue(files.getJSONObject(0).getBoolean("favorite"));
            NativeBridge.stop(previous);
            assertTrue(request("/api/state").has("busy"));
            assertTrue(command(new JSONObject().put("action", "forget")).getBoolean("ok"));
            assertFalse(isolated.getSharedPreferences("credentials", Context.MODE_PRIVATE).contains("sealed"));
        } finally {
            NativeBridge.stop(token);
            DownloadStorage cleanup = new DownloadStorage(isolated);
            cleanup.request(new JSONObject().put("action", "configure").put("private", downloads.getAbsolutePath()));
            JSONArray stored = cleanup.request(new JSONObject().put("action", "list")).getJSONArray("files");
            for (int index = 0; index < stored.length(); ++index) {
                JSONObject file = stored.getJSONObject(index);
                if (betaName.equals(file.getString("name"))) {
                    android.net.Uri uri = android.net.Uri.parse(file.getString("uri"));
                    if ("file".equals(uri.getScheme())) java.nio.file.Files.delete(new File(uri.getPath()).toPath());
                    else target.getContentResolver().delete(uri, null, null);
                }
            }
            isolated.getSharedPreferences("credentials", Context.MODE_PRIVATE).edit().clear().commit();
        }

        UiDevice device = UiDevice.getInstance(instrumentation);
        device.executeShellCommand("mkdir -p /sdcard/Download/ArchiveCatalogTest");
        device.executeShellCommand("pm grant org.archivecatalog.app android.permission.POST_NOTIFICATIONS");
        assertFalse(Environment.isExternalStorageManager());
        write(new File(target.getFilesDir(), "import.xml"), MANIFEST);
        MainActivity activity = (MainActivity) instrumentation.startActivitySync(new Intent(target, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        try {
            AtomicReference<WebView> web = new AtomicReference<>();
            instrumentation.runOnMainSync(() -> web.set(findWeb(activity.getWindow().getDecorView())));
            assertNotNull(web.get());
            waitScript(instrumentation, web.get(), "typeof state !== 'undefined' && !!state.snapshot && !busy() && android", 30000);
            assertEquals("true", script(instrumentation, web.get(), "document.title === 'ArchiveFetch'"));
            assertEquals("true", script(instrumentation, web.get(), "(() => { try { localStorage.setItem('archivefetch-probe', '1'); const kept = localStorage.getItem('archivefetch-probe') === '1'; localStorage.removeItem('archivefetch-probe'); return kept; } catch { return false; } })()"));
            // The bundled console payload is unpacked for Install/Update when the APK carries one.
            if (java.util.Arrays.asList(target.getAssets().list("payload")).contains("ArchiveFetch.elf")) {
                File payload = new File(target.getFilesDir(), "payload/ArchiveFetch.version");
                assertTrue(payload.exists());
                assertEquals(NativeBridge.appVersion(), new String(java.nio.file.Files.readAllBytes(payload.toPath()), StandardCharsets.UTF_8).trim());
            }
            assertEquals("true", script(instrumentation, web.get(), "state.view === 'library' && !document.querySelector('#library-view').hidden"));
            script(instrumentation, web.get(), "view('downloads'); window.lifecycleMarker = 'retained'; window.lifecycleSession = state.token; true");
            try {
                device.setOrientationLeft();
                device.waitForIdle();
                waitScript(instrumentation, web.get(), "innerWidth > innerHeight", 10000);
                instrumentation.runOnMainSync(() -> assertSame(web.get(), findWeb(activity.getWindow().getDecorView())));
                assertEquals("true", script(instrumentation, web.get(), "state.view === 'downloads' && window.lifecycleMarker === 'retained'"));
                device.setOrientationNatural();
                device.waitForIdle();
            } finally { device.unfreezeRotation(); }
            script(instrumentation, web.get(), "window.lifecycleBackgroundStart = performance.now(); true");
            device.pressHome();
            device.waitForIdle();
            waitScript(instrumentation, web.get(), "performance.now() - window.lifecycleBackgroundStart >= 2000 && state.token === window.lifecycleSession && !!state.snapshot && !busy()", 10000);
            target.startActivity(new Intent(target, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT));
            device.waitForIdle();
            waitScript(instrumentation, web.get(), "state.view === 'downloads' && window.lifecycleMarker === 'retained' && state.token === window.lifecycleSession && !!state.snapshot && !busy() && document.querySelector('#connection').textContent === 'Local service'", 10000);
            script(instrumentation, web.get(), "view('library'); true");
            script(instrumentation, web.get(), "document.querySelector('#debug-button').click(); true");
            waitScript(instrumentation, web.get(), "document.querySelector('#diagnostics').textContent.includes('[android] App version=')", 10000);
            assertEquals("true", script(instrumentation, web.get(), "!document.querySelector('#diagnostics').textContent.includes('[tls]') && !document.querySelector('#diagnostics').textContent.includes('CA readable=') && !document.querySelector('#diagnostics').textContent.includes('Cannot load CA certificate bundle')"));
            Bitmap diagnostics = instrumentation.getUiAutomation().takeScreenshot();
            assertNotNull(diagnostics);
            try (FileOutputStream image = new FileOutputStream(new File(target.getExternalFilesDir(null), "android-diagnostics.png"))) { diagnostics.compress(Bitmap.CompressFormat.PNG, 100, image); }
            diagnostics.recycle();
            script(instrumentation, web.get(), "document.querySelector('#debug-dialog').close(); true");
            assertEquals("true", script(instrumentation, web.get(), "document.querySelector('#destination-input').readOnly && !document.querySelector('.browse[data-input=\"destination-input\"]').hidden && document.querySelector('.browse[data-input=\"output-input\"]').hidden"));
            script(instrumentation, web.get(), "run('import', {path: " + JSONObject.quote(new File(target.getFilesDir(), "import.xml").getAbsolutePath()) + "}); true");
            waitScript(instrumentation, web.get(), "!!state.snapshot && state.snapshot.count === 2 && !busy()", 15000);
            script(instrumentation, web.get(), "view('library'); true");
            waitScript(instrumentation, web.get(), "document.querySelectorAll('#library-body tr').length === 2", 10000);
            assertEquals("true", script(instrumentation, web.get(), "document.body.textContent.includes('Alpha \\uD83C\\uDFAE.chd') && !document.querySelector('#share-downloads').hidden"));
            script(instrumentation, web.get(), "document.querySelector('[data-filters=\"library\"]').click(); document.querySelector('[name=\"sort-direction\"][value=\"descending\"]').click(); true");
            assertEquals("true", script(instrumentation, web.get(), "document.querySelector('[name=\"sort-direction\"][value=\"ascending\"]').parentElement.textContent === 'A-Z' && document.querySelector('[name=\"sort-direction\"][value=\"descending\"]').parentElement.textContent === 'Z-A' && document.querySelector('#library-body tr').textContent.includes('Beta.chd')"));
            script(instrumentation, web.get(), "window.accessDropdown = document.querySelector('[aria-controls=\"options-library-access\"]'); accessDropdown.value='restr'; accessDropdown.dispatchEvent(new Event('input')); true");
            assertEquals("true", script(instrumentation, web.get(), "accessDropdown.getAttribute('aria-expanded') === 'true' && [...document.querySelectorAll('#options-library-access [role=\"option\"]')].filter(option => !option.hidden).length === 1"));
            script(instrumentation, web.get(), "[...document.querySelectorAll('#options-library-access [role=\"option\"]')].find(option => !option.hidden).click(); document.querySelector('#filters-dialog .primary').click(); true");
            assertEquals("true", script(instrumentation, web.get(), "document.querySelectorAll('#library-body tr').length===1 && document.querySelector('#library-body').textContent.includes('Beta.chd')"));
            assertEquals("true", script(instrumentation, web.get(), "document.documentElement.scrollWidth <= innerWidth"));
            Bitmap screenshot = instrumentation.getUiAutomation().takeScreenshot();
            assertNotNull(screenshot);
            try (FileOutputStream image = new FileOutputStream(new File(target.getExternalFilesDir(null), "android-ui.png"))) { screenshot.compress(Bitmap.CompressFormat.PNG, 100, image); }
            screenshot.recycle();
            script(instrumentation, web.get(), "document.querySelector('#settings-button').click(); document.querySelector('.browse[data-input=\"destination-input\"]').click(); true");
            UiObject2 folder = device.wait(Until.findObject(By.text("ArchiveCatalogTest")), 15000);
            assertNotNull("Download folder was not offered by Android's picker", folder);
            // The picker redraws its list while loading, which can invalidate a found row before the click.
            for (int attempt = 0; ; ++attempt) {
                try { folder.click(); break; }
                catch (androidx.test.uiautomator.StaleObjectException stale) {
                    if (attempt == 4) throw stale;
                    folder = device.wait(Until.findObject(By.text("ArchiveCatalogTest")), 5000);
                    assertNotNull("Download folder disappeared from Android's picker", folder);
                }
            }
            clickPickerButton(device, By.text(java.util.regex.Pattern.compile("(?i)use this folder")), "Folder picker has no selection button");
            clickPickerButton(device, By.res("android", "button1"), "Folder grant was not requested");
            waitScript(instrumentation, web.get(), "!state.pending && document.querySelector('#destination-input').value.startsWith('content://')", 15000);
            script(instrumentation, web.get(), "document.querySelector('#automatic').checked=false; document.querySelector('#settings-form').requestSubmit(); true");
            waitScript(instrumentation, web.get(), "!state.pending && state.snapshot.settings.destination.startsWith('content://')", 15000);
            String selected = new DownloadStorage(target).tree();
            assertTrue(selected.startsWith("content://"));
            String filename = "Alpha \uD83C\uDFAE.chd";
            write(new File(stagingDirectory(target.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS), selected), filename + ".part"), "abc");
            script(instrumentation, web.get(), "run('enqueue', {indices:[0], revision:state.snapshot.catalogRevision}); true");
            waitScript(instrumentation, web.get(), "!state.pending && state.snapshot.queue.length===1", 10000);
            script(instrumentation, web.get(), "run('start'); true");
            waitScript(instrumentation, web.get(), "!busy() && state.snapshot.queue.length===0", 15000);
            JSONArray saved = new DownloadStorage(target).request(new JSONObject().put("action", "list")).getJSONArray("files");
            JSONObject copied = null;
            for (int index = 0; index < saved.length(); ++index) if (filename.equals(saved.getJSONObject(index).getString("name"))) copied = saved.getJSONObject(index);
            assertNotNull("Verified file was not saved to the chosen folder", copied);
            assertEquals(3, copied.getLong("size"));
            try (java.io.InputStream input = target.getContentResolver().openInputStream(android.net.Uri.parse(copied.getString("uri")))) {
                assertNotNull(input); assertEquals('a', input.read()); assertEquals('b', input.read()); assertEquals('c', input.read()); assertEquals(-1, input.read());
            }
            assertFalse(new File(target.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS), filename).exists());
            script(instrumentation, web.get(), "loadLibrary(true); true");
            waitScript(instrumentation, web.get(), "state.files[0].downloaded", 10000);
            JSONObject overwrite = new JSONObject(NativeBridge.request("/api/command", new JSONObject().put("action", "enqueue").put("indices", new JSONArray().put(0))
                .put("revision", new JSONObject(NativeBridge.request("/api/state", "{}", stateToken(instrumentation, web.get()))).getLong("catalogRevision")).toString(), stateToken(instrumentation, web.get())));
            assertTrue(overwrite.toString(), overwrite.has("confirmOverwrite"));
            assertEquals(selected, new DownloadStorage(target).tree());
            write(new File(stagingDirectory(target.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS), selected), filename + ".part"), "bad");
            script(instrumentation, web.get(), "run('enqueue', {indices:[0], revision:state.snapshot.catalogRevision, overwrite:true}); true");
            waitScript(instrumentation, web.get(), "!state.pending && state.snapshot.queue.length===1", 10000);
            script(instrumentation, web.get(), "run('start'); true");
            waitScript(instrumentation, web.get(), "!busy() && state.snapshot.queue[0]?.status==='Failed'", 10000);
            try (java.io.InputStream input = target.getContentResolver().openInputStream(android.net.Uri.parse(copied.getString("uri")))) {
                assertNotNull(input); assertEquals('a', input.read()); assertEquals('b', input.read()); assertEquals('c', input.read()); assertEquals(-1, input.read());
            }
            write(new File(stagingDirectory(target.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS), selected), filename + ".part"), "abc");
            script(instrumentation, web.get(), "run('start'); true");
            waitScript(instrumentation, web.get(), "!busy() && state.snapshot.queue.length===0", 10000);
            JSONArray replaced = new DownloadStorage(target).request(new JSONObject().put("action", "list")).getJSONArray("files");
            copied = null;
            for (int index = 0; index < replaced.length(); ++index) {
                JSONObject file = replaced.getJSONObject(index);
                assertFalse(file.getString("name").startsWith(".archive-"));
                if (filename.equals(file.getString("name"))) copied = file;
            }
            assertNotNull(copied); assertEquals(3, copied.getLong("size"));
            JSONObject share = new JSONObject(NativeBridge.request("/api/command", new JSONObject().put("action", "share").put("indices", new JSONArray().put(0))
                .put("revision", new JSONObject(NativeBridge.request("/api/state", "{}", stateToken(instrumentation, web.get()))).getLong("catalogRevision")).toString(), stateToken(instrumentation, web.get())));
            assertTrue(share.toString(), share.getJSONArray("shareFiles").getString(0).startsWith("content://"));
            script(instrumentation, web.get(), "document.querySelector('#settings-button').click(); true");
            instrumentation.waitForIdleSync();
            Bitmap folderScreenshot = instrumentation.getUiAutomation().takeScreenshot();
            assertNotNull(folderScreenshot);
            try (FileOutputStream image = new FileOutputStream(new File(target.getExternalFilesDir(null), "android-folder.png"))) { folderScreenshot.compress(Bitmap.CompressFormat.PNG, 100, image); }
            folderScreenshot.recycle();
            assertTrue(android.provider.DocumentsContract.deleteDocument(target.getContentResolver(), android.net.Uri.parse(copied.getString("uri"))));
            target.getContentResolver().releasePersistableUriPermission(android.net.Uri.parse(selected), Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            JSONObject revoked = new JSONObject(NativeBridge.request("/api/library", "{}", stateToken(instrumentation, web.get())));
            assertFalse(revoked.toString(), revoked.has("error"));
            JSONObject unavailable = revoked.getJSONArray("files").getJSONObject(0).getJSONArray("downloadCopies").getJSONObject(0);
            assertEquals("Location unavailable", unavailable.getString("status"));
            assertTrue(revoked.toString(), unavailable.optString("error").contains("revoked"));
            assertTrue(new JSONObject(NativeBridge.storageRequest("{\"action\":\"select\",\"tree\":\"\"}")).getBoolean("ok"));
        } finally { instrumentation.runOnMainSync(activity::finish); }
    }
    private static String stateToken(Instrumentation instrumentation, WebView web) throws Exception {
        return new org.json.JSONTokener(script(instrumentation, web, "state.token")).nextValue().toString();
    }
    private static WebView findWeb(View view) {
        if (view instanceof WebView) return (WebView) view;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int index = 0; index < group.getChildCount(); ++index) { WebView found = findWeb(group.getChildAt(index)); if (found != null) return found; }
        }
        return null;
    }
    private static String script(Instrumentation instrumentation, WebView web, String code) throws Exception {
        AtomicReference<String> result = new AtomicReference<>(); CountDownLatch completed = new CountDownLatch(1);
        instrumentation.runOnMainSync(() -> web.evaluateJavascript(code, value -> { result.set(value); completed.countDown(); }));
        assertTrue("WebView evaluation timed out", completed.await(10, TimeUnit.SECONDS));
        return result.get();
    }
    private static void waitScript(Instrumentation instrumentation, WebView web, String code, long timeout) throws Exception {
        long deadline = SystemClock.elapsedRealtime() + timeout;
        do { if ("true".equals(script(instrumentation, web, code))) return; Thread.sleep(100); }
        while (SystemClock.elapsedRealtime() < deadline);
        throw new AssertionError("WebView did not satisfy: " + code + "; state=" + script(instrumentation, web, "document.body.innerText"));
    }
}