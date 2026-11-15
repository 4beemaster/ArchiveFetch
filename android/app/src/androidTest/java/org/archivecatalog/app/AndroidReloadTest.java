package org.archivecatalog.app;

import static org.junit.Assert.*;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.os.SystemClock;
import android.webkit.WebView;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.uiautomator.UiDevice;
import java.nio.file.Files;
import java.io.File;
import java.security.MessageDigest;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Assume;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class AndroidReloadTest {
    private static void waitReady(Instrumentation instrumentation, WebView web, String condition, long timeout) throws Exception {
        long deadline = SystemClock.elapsedRealtime() + timeout;
        do {
            if ("true".equals(NativeIntegrationTest.script(instrumentation, web, condition))) return;
            Thread.sleep(100);
        } while (SystemClock.elapsedRealtime() < deadline);
        throw new AssertionError("Reload readiness failed: " + NativeIntegrationTest.script(instrumentation, web,
            "JSON.stringify({count: typeof state === 'undefined' ? null : state.snapshot?.count," +
            "loaded: typeof state !== 'undefined' && !!state.libraryKey," +
            "activity: document.querySelector('#activity')?.textContent," +
            "version: document.querySelector('#app-version')?.textContent})"));
    }
    @Test public void populatedLibraryReloadAndSessionRecovery() throws Exception {
        Assume.assumeTrue("Requires a disposable seeded emulator",
            "true".equals(InstrumentationRegistry.getArguments().getString("allowReloadTest")));
        int expected = Integer.parseInt(InstrumentationRegistry.getArguments().getString("expectedFiles", "1000"));
        Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        Context target = instrumentation.getTargetContext();
        File library = new File(target.getFilesDir(), "library.xml"), sources = new File(target.getFilesDir(), "sources.xml");
        byte[] libraryHash = MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(library.toPath()));
        byte[] sourceHash = MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(sources.toPath()));
        MainActivity activity = (MainActivity) instrumentation.startActivitySync(
            new Intent(target, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        AtomicReference<WebView> web = new AtomicReference<>();
        instrumentation.runOnMainSync(() -> web.set(NativeIntegrationTest.findWeb(activity.getWindow().getDecorView())));
        assertNotNull(web.get());
        String ready = "typeof state !== 'undefined' && !!state.snapshot && state.snapshot.count === " + expected +
            " && !!state.libraryKey && state.files.length > 0 && !busy() && document.querySelector('#activity').textContent === 'Ready'" +
            " && !document.querySelector('#target-button').hidden && document.querySelector('#app-version').textContent === " +
            org.json.JSONObject.quote("v" + target.getPackageManager().getPackageInfo(target.getPackageName(), 0).versionName);
        try {
            NativeIntegrationTest.script(instrumentation, web.get(),
                "localStorage.removeItem('archivefetch-target'); view('library'); true");
            waitReady(instrumentation, web.get(), ready, 30000);
            for (int cycle = 0; cycle < 5; ++cycle) {
                long started = SystemClock.elapsedRealtime();
                NativeIntegrationTest.script(instrumentation, web.get(),
                    "window.reloadMarker = true; document.querySelector('#reload-interface').click(); true");
                waitReady(instrumentation, web.get(),
                    "typeof window.reloadMarker === 'undefined' && " + ready, 15000);
                long elapsed = SystemClock.elapsedRealtime() - started;
                System.out.println("Populated reload: cycle=" + (cycle + 1) + ", visibleFiles=" + expected + ", elapsedMs=" + elapsed);
                assertTrue("Reload did not finish within 15 seconds", elapsed < 15000);
                assertEquals("false", NativeIntegrationTest.script(instrumentation, web.get(),
                    "!document.querySelector('#alert').hidden && /timed out|Invalid app session/.test(document.querySelector('#alert-text').textContent)"));
            }
            NativeIntegrationTest.script(instrumentation, web.get(), "state.token = 'expired-test-session'; refresh(); true");
            waitReady(instrumentation, web.get(),
                ready + " && state.token !== 'expired-test-session'", 15000);
            UiDevice device = UiDevice.getInstance(instrumentation);
            device.pressHome();
            Thread.sleep(2000);
            target.startActivity(new Intent(target, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT));
            waitReady(instrumentation, web.get(), ready, 15000);
            assertArrayEquals(libraryHash, MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(library.toPath())));
            assertArrayEquals(sourceHash, MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(sources.toPath())));
        } finally { instrumentation.runOnMainSync(activity::finish); }
    }
}
