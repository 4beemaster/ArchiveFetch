package org.archivecatalog.app;

import static org.junit.Assert.*;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.webkit.WebView;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.uiautomator.UiDevice;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONObject;
import org.junit.Assume;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class AndroidAutomaticUpdateTest {
    @Test public void startupReloadAndForegroundCheckWithoutInstalling() throws Exception {
        Assume.assumeTrue("Requires a disposable emulator and live GitHub access",
            "true".equals(InstrumentationRegistry.getArguments().getString("allowAutomaticUpdateTest")));
        Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        Context target = instrumentation.getTargetContext();
        MainActivity activity = (MainActivity) instrumentation.startActivitySync(
            new Intent(target, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        AtomicReference<WebView> web = new AtomicReference<>();
        instrumentation.runOnMainSync(() -> web.set(NativeIntegrationTest.findWeb(activity.getWindow().getDecorView())));
        assertNotNull(web.get());
        String ready = "typeof updateCheckPromise !== 'undefined' && updateChannelReady && !updateCheckPromise && " +
            "!!publishedUpdate && !!state.libraryKey && !busy() && !publishedUpdateError";
        try {
            NativeIntegrationTest.waitScript(instrumentation, web.get(), ready, 45000);
            assertEquals("true", NativeIntegrationTest.script(instrumentation, web.get(),
                "publishedUpdate.currentVersion === " + JSONObject.quote(NativeBridge.appVersion()) +
                " && publishedUpdate.platform === 'android' && document.querySelector('#update-beta').checked"));
            NativeIntegrationTest.script(instrumentation, web.get(),
                "window.autoCheckStarts = 0; window.autoCheckDownloads = 0; window.autoCheckInstalls = 0;" +
                "window.originalUpdateRequest = localRequest; localRequest = async (path, data, ...rest) => {" +
                "if (data?.action === 'startUpdateCheck') window.autoCheckStarts++;" +
                "if (data?.action === 'prepareUpdate') window.autoCheckDownloads++;" +
                "if (data?.action === 'installUpdate') window.autoCheckInstalls++;" +
                "return window.originalUpdateRequest(path, data, ...rest); }; true");
            UiDevice device = UiDevice.getInstance(instrumentation);
            device.pressHome();
            Thread.sleep(1000);
            target.startActivity(new Intent(target, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT));
            NativeIntegrationTest.waitScript(instrumentation, web.get(),
                ready + " && window.autoCheckStarts === 1", 45000);
            assertEquals("true", NativeIntegrationTest.script(instrumentation, web.get(),
                "window.autoCheckDownloads === 0 && window.autoCheckInstalls === 0"));
            NativeIntegrationTest.script(instrumentation, web.get(),
                "document.querySelector('#update-beta').checked = false;" +
                "document.querySelector('#update-beta').dispatchEvent(new Event('change')); true");
            NativeIntegrationTest.waitScript(instrumentation, web.get(),
                "!updateChannelSaving && !document.querySelector('#update-beta').checked && !updateChannelError", 15000);
            NativeIntegrationTest.script(instrumentation, web.get(),
                "window.beforeAutomaticReload = true; document.querySelector('#reload-interface').click(); true");
            NativeIntegrationTest.waitScript(instrumentation, web.get(),
                "typeof window.beforeAutomaticReload === 'undefined' && " + ready +
                " && !document.querySelector('#update-beta').checked", 45000);
            assertEquals("true", NativeIntegrationTest.script(instrumentation, web.get(),
                "publishedUpdate.latestVersion.indexOf('-beta.') === -1"));
            NativeIntegrationTest.script(instrumentation, web.get(),
                "document.querySelector('#update-beta').checked = true;" +
                "document.querySelector('#update-beta').dispatchEvent(new Event('change')); true");
            NativeIntegrationTest.waitScript(instrumentation, web.get(),
                "!updateChannelSaving && document.querySelector('#update-beta').checked && !updateChannelError", 15000);
        } finally { instrumentation.runOnMainSync(activity::finish); }
    }
}
