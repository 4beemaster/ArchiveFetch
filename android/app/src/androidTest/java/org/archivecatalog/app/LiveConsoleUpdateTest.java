package org.archivecatalog.app;

import static org.junit.Assert.*;
import android.content.Context;
import android.os.Bundle;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.UUID;
import org.json.JSONObject;
import org.junit.Assume;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class LiveConsoleUpdateTest {
    @Test public void connectApprovedConsole() throws Exception {
        Bundle arguments = InstrumentationRegistry.getArguments();
        Assume.assumeTrue("true".equals(arguments.getString("allowLiveRead")));
        String address = arguments.getString("liveConsole", "");
        String password = arguments.getString("livePassword", "");
        arguments.remove("livePassword");
        assertFalse("An explicitly approved console address is required", address.isEmpty());
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File root = new File(context.getCacheDir(), "live_console_" + UUID.randomUUID());
        assertTrue(root.mkdirs());
        File ca = new File(root, "trust.pem");
        CatalogService.writeTrust(ca);
        NativeBridge.initialize(context);
        JSONObject started = new JSONObject(NativeBridge.start(root.getAbsolutePath(),
            new File(root, "downloads").getAbsolutePath(), ca.getAbsolutePath()));
        assertFalse("Native startup failed", started.has("error"));
        String token = new JSONObject(NativeBridge.request("/session", "{}", "")).getString("token");
        String pairedId = "";
        String id = "";
        try {
            JSONObject added = call("/api/consoles", new JSONObject().put("action", "add")
                .put("name", "Approved connection test").put("address", address).put("port", 8890), token);
            assertFalse("Could not add approved console", added.has("error"));
            id = added.getJSONArray("consoles").getJSONObject(0).getString("id");
            JSONObject login = call("/api/relay", new JSONObject().put("console", id)
                .put("path", "/api/login").put("body", new JSONObject().put("password", password)), token);
            password = "";
            assertFalse("Approved console sign-in failed", login.has("error"));
            JSONObject devices = call("/api/relay", new JSONObject().put("console", id)
                .put("path", "/api/paired-devices"), token);
            for (int index = 0; index < devices.getJSONArray("devices").length(); ++index) {
                JSONObject device = devices.getJSONArray("devices").getJSONObject(index);
                if (device.getBoolean("current")) {
                    assertTrue("Multiple current test pairings", pairedId.isEmpty());
                    pairedId = device.getString("id");
                }
            }
            assertFalse("Connection test did not pair", pairedId.isEmpty());
            JSONObject entry = awaitOnline(token);
            assertEquals("online", entry.getString("status"));
            assertTrue("Console was not paired", entry.getBoolean("paired"));
            assertEquals(NativeBridge.appVersion(), entry.getString("version"));
            JSONObject state = call("/api/relay", new JSONObject().put("console", id)
                .put("path", "/api/state"), token);
            assertEquals("Console state relay failed", "", state.optString("error", ""));
            assertTrue("Console state response is incomplete", state.has("settings") && state.has("queue"));
            JSONObject checked = call("/api/relay", new JSONObject().put("console", id)
                .put("path", "/api/command").put("body", new JSONObject()
                    .put("action", "checkUpdates").put("includeBeta", true)), token);
            assertTrue("Console GitHub check failed", checked.getBoolean("ok"));
            assertEquals("ps5", checked.getString("platform"));
            assertEquals(NativeBridge.appVersion(), checked.getString("currentVersion"));
            NativeBridge.stop(token);
            JSONObject restarted = new JSONObject(NativeBridge.start(root.getAbsolutePath(),
                new File(root, "downloads").getAbsolutePath(), ca.getAbsolutePath()));
            assertFalse("Native restart failed", restarted.has("error"));
            token = new JSONObject(NativeBridge.request("/session", "{}", "")).getString("token");
            entry = awaitOnline(token);
            assertEquals("online", entry.getString("status"));
            assertTrue("Saved Android pairing did not survive native restart", entry.getBoolean("paired"));
            assertEquals(NativeBridge.appVersion(), entry.getString("version"));
        } finally {
            try {
                if (!pairedId.isEmpty()) {
                    JSONObject removed = call("/api/relay", new JSONObject().put("console", id)
                        .put("path", "/api/paired-devices/remove")
                        .put("body", new JSONObject().put("id", pairedId)), token);
                    assertTrue("Could not remove the connection test's own pairing", removed.getBoolean("ok"));
                }
            } finally { NativeBridge.stop(token); }
        }
    }

    @Test public void updateApprovedConsole() throws Exception {
        Bundle arguments = InstrumentationRegistry.getArguments();
        Assume.assumeTrue("true".equals(arguments.getString("allowLiveUpdate")));
        String address = arguments.getString("liveConsole", "");
        String password = arguments.getString("livePassword", "");
        String expectedOld = arguments.getString("expectedOldVersion", "");
        assertFalse("An explicitly approved console address is required", address.isEmpty());
        assertFalse("An expected older version is required", expectedOld.isEmpty());
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File root = new File(context.getCacheDir(), "live_console_" + UUID.randomUUID());
        File payload = new File(root, "payload");
        assertTrue(payload.mkdirs());
        for (String name : new String[]{"ArchiveFetch.elf", "ArchiveFetch.version"}) {
            try (InputStream input = context.getAssets().open("payload/" + name);
                 FileOutputStream output = new FileOutputStream(new File(payload, name))) {
                byte[] buffer = new byte[65536];
                for (int count; (count = input.read(buffer)) != -1;) output.write(buffer, 0, count);
            }
        }
        File ca = new File(root, "trust.pem");
        CatalogService.writeTrust(ca);
        NativeBridge.initialize(context);
        JSONObject started = new JSONObject(NativeBridge.start(root.getAbsolutePath(),
            new File(root, "downloads").getAbsolutePath(), ca.getAbsolutePath()));
        assertFalse("Native startup failed", started.has("error"));
        String token = new JSONObject(NativeBridge.request("/session", "{}", "")).getString("token");
        try {
            JSONObject added = call("/api/consoles", new JSONObject().put("action", "add")
                .put("name", "Approved live test").put("address", address).put("port", 8890), token);
            assertFalse("Could not add approved console", added.has("error"));
            assertEquals(1, added.getJSONArray("consoles").length());
            String id = added.getJSONArray("consoles").getJSONObject(0).getString("id");
            JSONObject login = call("/api/relay", new JSONObject().put("console", id)
                .put("path", "/api/login").put("body", new JSONObject().put("password", password)), token);
            assertFalse("Approved console sign-in failed", login.has("error"));
            password = "";
            JSONObject entry = call("/api/consoles", new JSONObject().put("refresh", true), token)
                .getJSONArray("consoles").getJSONObject(0);
            assertEquals("Unexpected pre-update console version", expectedOld, entry.getString("version"));
            assertTrue("Older-version banner data missing", entry.getBoolean("older"));
            assertTrue("Console was not paired", entry.getBoolean("paired"));
            JSONObject begin = call("/api/consoles", new JSONObject().put("action", "install").put("id", id), token);
            assertFalse("Could not start approved update", begin.has("error"));
            long deadline = SystemClock.elapsedRealtime() + 180000;
            JSONObject progress;
            do {
                Thread.sleep(1000);
                entry = call("/api/consoles", new JSONObject(), token).getJSONArray("consoles").getJSONObject(0);
                progress = entry.getJSONObject("install");
                if (!progress.getBoolean("running")) break;
            } while (SystemClock.elapsedRealtime() < deadline);
            assertFalse("Approved update timed out; do not retry automatically", progress.getBoolean("running"));
            assertEquals("Update failed; do not retry automatically", "done", progress.getString("stage"));
            assertEquals("Boot-copy warning; do not retry automatically", "", progress.optString("warning"));
            assertEquals("Payload Manager copy was not updated", "updated", progress.getString("payloadManager"));
            assertEquals("Expected app-driven console route", "console", progress.getString("route"));
            assertEquals(NativeBridge.appVersion(), entry.getString("version"));
            assertTrue("Pairing lost after update", entry.getBoolean("paired"));
            assertFalse("Version mismatch remained after update", entry.getBoolean("older"));
            NativeBridge.stop(token);
            JSONObject restarted = new JSONObject(NativeBridge.start(root.getAbsolutePath(),
                new File(root, "downloads").getAbsolutePath(), ca.getAbsolutePath()));
            assertFalse("Native restart failed", restarted.has("error"));
            token = new JSONObject(NativeBridge.request("/session", "{}", "")).getString("token");
            entry = awaitOnline(token);
            assertEquals("online", entry.getString("status"));
            assertTrue("Saved Android pairing did not survive native restart", entry.getBoolean("paired"));
            assertEquals(NativeBridge.appVersion(), entry.getString("version"));
        } finally { NativeBridge.stop(token); }
    }

    @Test public void verifySavedPairing() throws Exception {
        Bundle arguments = InstrumentationRegistry.getArguments();
        Assume.assumeTrue("true".equals(arguments.getString("allowLiveRead")));
        String fixture = arguments.getString("liveFixture", "");
        assertTrue("An existing live-test fixture basename is required",
            fixture.matches("live_console_[0-9a-f-]{36}"));
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File root = new File(context.getCacheDir(), fixture);
        assertTrue("Saved fixture is missing", new File(root, "consoles.json").isFile());
        NativeBridge.initialize(context);
        JSONObject started = new JSONObject(NativeBridge.start(root.getAbsolutePath(),
            new File(root, "downloads").getAbsolutePath(), new File(root, "trust.pem").getAbsolutePath()));
        assertFalse("Native startup failed", started.has("error"));
        String token = new JSONObject(NativeBridge.request("/session", "{}", "")).getString("token");
        try {
            JSONObject entry = awaitOnline(token);
            assertEquals("online", entry.getString("status"));
            assertTrue("Saved Android pairing did not survive native restart", entry.getBoolean("paired"));
            assertEquals(NativeBridge.appVersion(), entry.getString("version"));
        } finally { NativeBridge.stop(token); }
    }

    private static JSONObject awaitOnline(String token) throws Exception {
        call("/api/consoles", new JSONObject().put("refresh", true), token);
        long deadline = SystemClock.elapsedRealtime() + 30000;
        JSONObject entry;
        String status = "checking";
        do {
            entry = call("/api/consoles", new JSONObject(), token).getJSONArray("consoles").getJSONObject(0);
            status = entry.getString("status");
            if ("online".equals(status)) return entry;
            Thread.sleep(100);
        } while (SystemClock.elapsedRealtime() < deadline);
        throw new AssertionError("Console readiness check timed out (" + status + "); no update was retried");
    }

    private static JSONObject call(String path, JSONObject body, String token) throws Exception {
        return new JSONObject(NativeBridge.request(path, body.toString(), token));
    }
}
