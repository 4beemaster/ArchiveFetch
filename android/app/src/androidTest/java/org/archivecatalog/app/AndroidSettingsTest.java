package org.archivecatalog.app;

import static org.junit.Assert.*;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.webkit.WebView;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONObject;
import org.junit.Assume;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class AndroidSettingsTest {
    @Test public void fullPathsLocationDialogAndVerboseLogs() throws Exception {
        Assume.assumeTrue("Requires a disposable emulator",
            "true".equals(InstrumentationRegistry.getArguments().getString("allowSettingsTest")));
        assertEquals("primary:", DocumentsContract.getDocumentId(MainActivity.initialFolderUri("")));
        File folder = new File(Environment.getExternalStorageDirectory(), "Download/ArchiveFetch");
        assertEquals("primary:Download/ArchiveFetch", DocumentsContract.getDocumentId(MainActivity.initialFolderUri(folder.getAbsolutePath())));
        Uri tree = DocumentsContract.buildTreeDocumentUri("fixture.provider", "selected-folder");
        assertEquals(tree, MainActivity.initialFolderUri(tree.toString()));
        Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        Context target = instrumentation.getTargetContext();
        MainActivity activity = (MainActivity) instrumentation.startActivitySync(
            new Intent(target, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        AtomicReference<WebView> web = new AtomicReference<>();
        instrumentation.runOnMainSync(() -> web.set(NativeIntegrationTest.findWeb(activity.getWindow().getDecorView())));
        assertNotNull(web.get());
        try {
            NativeIntegrationTest.waitScript(instrumentation, web.get(),
                "typeof state !== 'undefined' && !!state.snapshot && !busy() && android", 30000);
            assertEquals("true", NativeIntegrationTest.script(instrumentation, web.get(),
                "document.querySelector('.sidebar-bottom #debug-button') === null"));
            NativeIntegrationTest.script(instrumentation, web.get(), "document.querySelector('#settings-button').click(); true");
            assertEquals("true", NativeIntegrationTest.script(instrumentation, web.get(),
                "document.querySelector('#output-input').value === state.snapshot.settings.output && " +
                "document.querySelector('#output-input').value.startsWith('/') && " +
                "document.querySelector('#output-input').readOnly && !document.querySelector('#android-export-note').hidden"));
            NativeIntegrationTest.script(instrumentation, web.get(), "document.querySelector('#configure-download-locations').click(); true");
            assertEquals("true", NativeIntegrationTest.script(instrumentation, web.get(), "document.querySelector('#locations-dialog').open"));
            assertEquals("true", NativeIntegrationTest.script(instrumentation, web.get(),
                "[...document.querySelectorAll('#location-list .download-path')].every((node, index) => " +
                "node.textContent === state.snapshot.settings.locations[index].path && getComputedStyle(node).whiteSpace === 'normal')"));
            NativeIntegrationTest.script(instrumentation, web.get(),
                "document.querySelector('#add-location').click(); setFolderPath('location-path', '/storage/emulated/0/' + 'long-folder-name/'.repeat(60)); true");
            NativeIntegrationTest.waitScript(instrumentation, web.get(),
                "document.querySelector('#location-path').scrollHeight <= document.querySelector('#location-path').clientHeight && " +
                "document.querySelector('#location-path').scrollWidth <= document.querySelector('#location-path').clientWidth", 5000);
            NativeIntegrationTest.script(instrumentation, web.get(), "document.querySelector('#location-dialog').close(); true");
            NativeIntegrationTest.script(instrumentation, web.get(),
                "document.querySelector('#locations-dialog').close(); document.querySelector('#host-log-level').value = 'DEBUG'; document.querySelector('#save-settings').click(); true");
            NativeIntegrationTest.waitScript(instrumentation, web.get(), "state.snapshot.settings.debugLogging === true && !document.querySelector('#settings-dialog').open", 15000);
            NativeIntegrationTest.script(instrumentation, web.get(), "document.querySelector('#settings-button').click(); document.querySelector('#debug-button').click(); true");
            NativeIntegrationTest.waitScript(instrumentation, web.get(),
                "document.querySelector('#diagnostics').textContent.includes('[debug]')", 15000);
            String session = new JSONObject(NativeBridge.request("/session", "{}", "")).getString("token");
            JSONObject state = new JSONObject(NativeBridge.request("/api/state", "{}", session));
            assertTrue(state.getJSONObject("settings").getBoolean("debugLogging"));
        } finally { instrumentation.runOnMainSync(activity::finish); }
    }
}
