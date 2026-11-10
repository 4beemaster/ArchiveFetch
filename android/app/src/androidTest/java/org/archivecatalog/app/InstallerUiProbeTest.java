package org.archivecatalog.app;

import static org.junit.Assert.*;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.uiautomator.By;
import androidx.test.uiautomator.UiDevice;
import androidx.test.uiautomator.UiObject2;
import androidx.test.uiautomator.Until;
import java.io.File;
import java.nio.file.Files;
import java.util.UUID;
import org.junit.Assume;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class InstallerUiProbeTest {
    @Test public void permissionAndInstallerConfirmationCanBeAutomated() throws Exception {
        Assume.assumeTrue("Run only on the owned disposable emulator",
            InstrumentationRegistry.getArguments().getString("allowInstallerProbe", "false").equals("true"));
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        UiDevice device = UiDevice.getInstance(InstrumentationRegistry.getInstrumentation());
        File directory = new File(context.getFilesDir(), "updates");
        assertTrue(directory.isDirectory() || directory.mkdir());
        File apk = new File(directory, "installer-ui-probe-" + UUID.randomUUID() + ".apk");
        boolean permissionWasGranted = context.getPackageManager().canRequestPackageInstalls();
        try {
            Files.copy(new File(context.getApplicationInfo().sourceDir).toPath(), apk.toPath());
            context.startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                Uri.parse("package:" + context.getPackageName())).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
            UiObject2 permission = device.wait(Until.findObject(By.pkg("com.android.settings").checkable(true)), 10000);
            assertNotNull("Unknown-app-install permission switch must be automatable", permission);
            if (!permissionWasGranted) permission.click();
            long deadline = SystemClock.elapsedRealtime() + 5000;
            while (!context.getPackageManager().canRequestPackageInstalls() && SystemClock.elapsedRealtime() < deadline)
                SystemClock.sleep(100);
            assertTrue("Unknown-app-install permission was not granted", context.getPackageManager().canRequestPackageInstalls());
            context.startActivity(VerifiedUpdateInstaller.installerIntent(context, apk).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            UiObject2 install = device.wait(Until.findObject(By.res("android", "button1")), 10000);
            assertNotNull("Package Installer confirmation must be automatable", install);
            assertTrue("Expected an update/install confirmation, not an installer error",
                "Update".equalsIgnoreCase(install.getText()) || "Install".equalsIgnoreCase(install.getText()));
            UiObject2 cancel = device.findObject(By.res("android", "button2"));
            assertNotNull("Probe must be able to cancel without installing", cancel);
            cancel.click();
        } finally {
            // Revoking this permission kills instrumentation; the disposable test runner uninstalls the fixture app.
            assertTrue(apk.delete() || !apk.exists());
        }
    }
}
