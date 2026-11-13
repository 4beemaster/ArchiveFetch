package org.archivecatalog.app;

import static org.junit.Assert.*;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.UUID;
import java.util.function.Predicate;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Assume;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class LiveDownloadPauseTest {
    private static final String ITEM = "alicesadventuresinwonderland_1902_librivox";
    private static final String LARGE = "AliceWonderland6_librivox.m4b";
    private static final String SMALL = "__ia_thumb.jpg";
    private static final String NEXT = "alicesadventuresinwonderland_01_carroll_64kb.mp3";
    private String token;

    private JSONObject request(String path) throws Exception {
        JSONObject value = new JSONObject(NativeBridge.request(path, "{}", token));
        assertFalse(value.toString(), value.has("error") && !value.has("queue"));
        assertEquals(value.toString(), "", value.optString("error"));
        return value;
    }
    private JSONObject command(JSONObject value) throws Exception {
        JSONObject result = new JSONObject(NativeBridge.request("/api/command", value.toString(), token));
        assertFalse(result.toString(), result.has("error"));
        return result;
    }
    private JSONObject action(String name, long id) throws Exception {
        return command(new JSONObject().put("action", name).put("id", id));
    }
    private JSONObject await(Predicate<JSONObject> condition, long milliseconds) throws Exception {
        long deadline = SystemClock.elapsedRealtime() + milliseconds;
        JSONObject state;
        do {
            state = request("/api/state");
            JSONArray queue = state.getJSONArray("queue");
            for (int index = 0; index < queue.length(); index++)
                assertNotEquals(queue.getJSONObject(index).toString(), "Failed", queue.getJSONObject(index).getString("status"));
            if (condition.test(state)) return state;
            SystemClock.sleep(25);
        } while (SystemClock.elapsedRealtime() < deadline);
        throw new AssertionError("Live download state timed out: " + state);
    }
    private static JSONObject entry(JSONObject state, String name) {
        JSONArray queue = state.optJSONArray("queue");
        if (queue != null) for (int index = 0; index < queue.length(); index++) {
            JSONObject entry = queue.optJSONObject(index);
            if (entry != null && name.equals(entry.optString("name"))) return entry;
        }
        return new JSONObject();
    }
    private static long partialBytes(File folder) {
        long bytes = 0;
        File[] files = folder.listFiles();
        if (files != null) for (File file : files)
            if (file.getName().startsWith(LARGE + ".part")) bytes += file.length();
        return bytes;
    }
    private static File partialDirectory(File downloads, String destination) throws Exception {
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(destination.getBytes(StandardCharsets.UTF_8));
        StringBuilder name = new StringBuilder();
        for (byte value : digest) name.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        return new File(new File(downloads, ".staging"), name.toString());
    }
    private static void removeFixture(File file) throws Exception {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) removeFixture(child);
        Files.deleteIfExists(file.toPath());
    }
    private static String hash(Context context, String uri) throws Exception {
        MessageDigest sha = MessageDigest.getInstance("SHA-1");
        try (InputStream input = context.getContentResolver().openInputStream(Uri.parse(uri))) {
            assertNotNull(input);
            byte[] buffer = new byte[65536];
            for (int count; (count = input.read(buffer)) != -1;) sha.update(buffer, 0, count);
        }
        StringBuilder text = new StringBuilder();
        for (byte value : sha.digest()) text.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        return text.toString();
    }
    @Test public void livePausePreservesPartialsAndContinuesQueue() throws Exception {
        Assume.assumeTrue("Run only on the owned disposable emulator with live download approval",
            "true".equals(InstrumentationRegistry.getArguments().getString("allowLivePause")));
        int connections = Integer.parseInt(InstrumentationRegistry.getArguments().getString("connections", "4"));
        Context target = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String prefix = "pause_" + UUID.randomUUID() + "_";
        Context isolated = new ContextWrapper(target) {
            @Override public Context getApplicationContext() { return this; }
            @Override public SharedPreferences getSharedPreferences(String name, int mode) {
                return target.getSharedPreferences(prefix + name, mode);
            }
        };
        NativeBridge.initialize(isolated);
        File root = new File(target.getCacheDir(), prefix);
        assertTrue(root.mkdirs());
        File downloads = new File(root, "downloads"), ca = new File(root, "trust.pem");
        CatalogService.writeTrust(ca);
        JSONArray existing = new JSONObject(NativeBridge.storageRequest("{\"action\":\"list\"}")).getJSONArray("files");
        for (int index = 0; index < existing.length(); index++) {
            String name = existing.getJSONObject(index).getString("name");
            assertFalse("Refuse to replace a pre-existing public download", name.equals(LARGE) || name.equals(SMALL) || name.equals(NEXT));
        }
        JSONObject started = new JSONObject(NativeBridge.start(root.getAbsolutePath(), downloads.getAbsolutePath(), ca.getAbsolutePath()));
        assertFalse(started.toString(), started.has("error"));
        token = new JSONObject(NativeBridge.request("/session", "{}", "")).getString("token");
        try {
            String destination = request("/api/state").getJSONObject("settings").getString("destination");
            File partials = partialDirectory(downloads, destination);
            command(new JSONObject().put("action", "settings").put("destination", destination)
                .put("output", new File(root, "exports").getAbsolutePath()).put("automatic", false)
                .put("connections", connections).put("remember", false).put("excludedExtensions", ""));
            String xml = "<catalog complete=\"true\"><item id=\"" + ITEM + "\"/>"
                + "<file item=\"" + ITEM + "\" name=\"" + LARGE + "\" size=\"80501772\" sha1=\"d49b74d719953fab5bc31e80bf8232271ca9f6ee\" restricted=\"false\"/>"
                + "<file item=\"" + ITEM + "\" name=\"" + SMALL + "\" size=\"18211\" sha1=\"62eaef5b1bf1d9b6ef0f86c29f78bfc4ec6c164c\" restricted=\"false\"/>"
                + "<file item=\"" + ITEM + "\" name=\"" + NEXT + "\" size=\"5837855\" sha1=\"a446c101397896516176f7415f3ff6afb3b837bc\" restricted=\"false\"/></catalog>";
            File catalog = new File(root, "import.xml");
            Files.write(catalog.toPath(), xml.getBytes(StandardCharsets.UTF_8));
            command(new JSONObject().put("action", "import").put("path", catalog.getAbsolutePath()));
            JSONObject state = await(value -> !value.optBoolean("busy"), 10000);
            JSONArray files = request("/api/library").getJSONArray("files"), indices = new JSONArray();
            for (String name : new String[]{LARGE, SMALL, NEXT})
                for (int index = 0; index < files.length(); index++)
                    if (name.equals(files.getJSONObject(index).getString("name"))) indices.put(files.getJSONObject(index).getInt("index"));
            assertEquals(3, indices.length());
            command(new JSONObject().put("action", "enqueue").put("indices", indices).put("revision", state.getLong("catalogRevision")));
            state = request("/api/state");
            long largeId = entry(state, LARGE).getLong("id"), smallId = entry(state, SMALL).getLong("id");
            action("pauseQueue", smallId);
            action("start", 0);
            await(value -> entry(value, LARGE).optLong("received") >= 65536, 120000);
            action("pauseQueue", largeId);
            state = await(value -> !value.optBoolean("downloading"), 180000);
            assertEquals("Paused", entry(state, LARGE).getString("status"));
            assertEquals("Paused", entry(state, SMALL).getString("status"));
            assertEquals(2, state.getJSONArray("queue").length());
            long saved = partialBytes(partials);
            assertTrue("Real network bytes must remain on disk", saved > 0 && saved < 80501772);
            android.util.Log.i("LivePause", "connections=" + connections + " held partial bytes=" + saved);
            action("start", 0);
            await(value -> !value.optBoolean("downloading"), 10000);
            assertEquals("Start must not release individual holds", saved, partialBytes(partials));
            action("resumeQueue", largeId);
            state = await(value -> !value.optBoolean("downloading"), 600000);
            assertEquals(1, state.getJSONArray("queue").length());
            assertEquals("Paused", entry(state, SMALL).getString("status"));
            action("resumeQueue", smallId);
            state = await(value -> !value.optBoolean("downloading"), 120000);
            assertEquals(0, state.getJSONArray("queue").length());
            JSONArray published = new JSONObject(NativeBridge.storageRequest("{\"action\":\"list\"}")).getJSONArray("files");
            int checked = 0;
            for (int index = 0; index < published.length(); index++) {
                JSONObject file = published.getJSONObject(index);
                String name = file.getString("name");
                String expected = name.equals(LARGE) ? "d49b74d719953fab5bc31e80bf8232271ca9f6ee"
                    : name.equals(SMALL) ? "62eaef5b1bf1d9b6ef0f86c29f78bfc4ec6c164c"
                    : name.equals(NEXT) ? "a446c101397896516176f7415f3ff6afb3b837bc" : "";
                if (!expected.isEmpty()) {
                    String actual = hash(target, file.getString("uri"));
                    assertEquals(expected, actual);
                    android.util.Log.i("LivePause", "connections=" + connections + " public file=" + name + " SHA1=" + actual);
                    checked++;
                }
            }
            assertEquals("All verified files must be published through Android storage", 3, checked);
        } finally {
            NativeBridge.stop(token);
            JSONArray files = new JSONObject(NativeBridge.storageRequest("{\"action\":\"list\"}")).getJSONArray("files");
            for (int index = 0; index < files.length(); index++) {
                JSONObject file = files.getJSONObject(index);
                String name = file.getString("name");
                if (name.equals(LARGE) || name.equals(SMALL) || name.equals(NEXT))
                    assertEquals(1, target.getContentResolver().delete(Uri.parse(file.getString("uri")), null, null));
            }
            removeFixture(root);
            isolated.getSharedPreferences("download-location", Context.MODE_PRIVATE).edit().clear().commit();
        }
    }
}
