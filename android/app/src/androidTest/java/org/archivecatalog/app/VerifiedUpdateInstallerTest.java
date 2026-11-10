package org.archivecatalog.app;

import static org.junit.Assert.*;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Build;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.UUID;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class VerifiedUpdateInstallerTest {
    private interface Checked { void run() throws Exception; }
    private static void refuses(Checked action) throws Exception {
        try { action.run(); fail("Expected explicit update rejection"); }
        catch (IllegalArgumentException expected) { assertNotNull(expected.getMessage()); }
    }

    @Test public void privateStagingDigestAndContentGrant() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File root = new File(context.getFilesDir(), "updates");
        assertTrue(root.isDirectory() || root.mkdir());
        File apk = new File(root, "adapter-test-" + UUID.randomUUID() + ".apk");
        File outside = new File(context.getFilesDir(), "adapter-test-" + UUID.randomUUID() + ".apk");
        File link = new File(root, "adapter-link-" + UUID.randomUUID() + ".apk");
        try {
            Files.write(apk.toPath(), "abc".getBytes(StandardCharsets.US_ASCII));
            Files.write(outside.toPath(), "abc".getBytes(StandardCharsets.US_ASCII));
            assertEquals(apk.getCanonicalFile(), VerifiedUpdateInstaller.stagedFile(context, apk.getAbsolutePath()));
            refuses(() -> VerifiedUpdateInstaller.stagedFile(context, outside.getAbsolutePath()));
            refuses(() -> VerifiedUpdateInstaller.stagedFile(context, new File(root, "../" + outside.getName()).getAbsolutePath()));
            refuses(() -> VerifiedUpdateInstaller.stagedFile(context, "relative.apk"));
            android.system.Os.symlink(apk.getAbsolutePath(), link.getAbsolutePath());
            refuses(() -> VerifiedUpdateInstaller.stagedFile(context, link.getAbsolutePath()));
            VerifiedUpdateInstaller.verifyDigest(apk, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
            refuses(() -> VerifiedUpdateInstaller.verifyDigest(apk, "0000000000000000000000000000000000000000000000000000000000000000"));
            refuses(() -> VerifiedUpdateInstaller.verifyDigest(apk, "not-a-hash"));
            Intent intent = VerifiedUpdateInstaller.installerIntent(context, apk);
            assertEquals(Intent.ACTION_VIEW, intent.getAction());
            assertEquals("content", intent.getData().getScheme());
            assertEquals(context.getPackageName() + ".updates", intent.getData().getAuthority());
            assertEquals("application/vnd.android.package-archive", intent.getType());
            assertEquals(Intent.FLAG_GRANT_READ_URI_PERMISSION, intent.getFlags());
            assertEquals(intent.getData(), intent.getClipData().getItemAt(0).getUri());
            refuses(() -> VerifiedUpdateInstaller.installerIntent(context, outside));
        } finally {
            assertTrue(link.delete() || !link.exists());
            assertTrue(apk.delete() || !apk.exists());
            assertTrue(outside.delete() || !outside.exists());
        }
    }

    @SuppressWarnings("deprecation")
    @Test public void apkIdentityVersionAndSignerContinuity() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        PackageManager manager = context.getPackageManager();
        int flags = Build.VERSION.SDK_INT >= 28 ? PackageManager.GET_SIGNING_CERTIFICATES : PackageManager.GET_SIGNATURES;
        PackageInfo installed = manager.getPackageInfo(context.getPackageName(), flags);
        android.os.Parcel parcel = android.os.Parcel.obtain();
        PackageInfo candidate;
        try {
            installed.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            candidate = PackageInfo.CREATOR.createFromParcel(parcel);
        } finally { parcel.recycle(); }
        candidate.versionName = "999.0.0";
        long oldCode = Build.VERSION.SDK_INT >= 28 ? installed.getLongVersionCode() : installed.versionCode;
        if (Build.VERSION.SDK_INT >= 28) candidate.setLongVersionCode(oldCode + 1);
        else candidate.versionCode = (int) oldCode + 1;
        VerifiedUpdateInstaller.verifyIdentity(candidate, installed, context.getPackageName(), "999.0.0");
        refuses(() -> VerifiedUpdateInstaller.verifyIdentity(candidate, installed, context.getPackageName(), "v999.0.0"));
        refuses(() -> VerifiedUpdateInstaller.verifyIdentity(candidate, installed, "foreign.package", "999.0.0"));
        refuses(() -> VerifiedUpdateInstaller.verifyIdentity(candidate, installed, context.getPackageName(), "999.0.1"));
        if (Build.VERSION.SDK_INT >= 28) candidate.setLongVersionCode(oldCode);
        else candidate.versionCode = (int) oldCode;
        refuses(() -> VerifiedUpdateInstaller.verifyIdentity(candidate, installed, context.getPackageName(), "999.0.0"));
        if (Build.VERSION.SDK_INT >= 28) {
            candidate.setLongVersionCode(oldCode + 1);
            candidate.signingInfo = manager.getPackageInfo("android", flags).signingInfo;
        } else {
            candidate.versionCode = (int) oldCode + 1;
            candidate.signatures = manager.getPackageInfo("android", flags).signatures;
        }
        refuses(() -> VerifiedUpdateInstaller.verifyIdentity(candidate, installed, context.getPackageName(), "999.0.0"));
        assertFalse(VerifiedUpdateInstaller.canonicalVersion("00.8.0"));
        assertFalse(VerifiedUpdateInstaller.canonicalVersion("0.8.0-beta.0"));
        assertTrue(VerifiedUpdateInstaller.canonicalVersion("0.8.0-beta.4"));
    }

    @Test public void updateMustContainInstalledNativeAbi() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String abi = VerifiedUpdateInstaller.installedAbi(context);
        assertTrue(java.util.Arrays.asList(Build.SUPPORTED_ABIS).contains(abi));
        File candidate = new File(context.getCacheDir(), "abi-test-" + UUID.randomUUID() + ".apk");
        try {
            try (ZipOutputStream zip = new ZipOutputStream(Files.newOutputStream(candidate.toPath()))) {
                zip.putNextEntry(new ZipEntry("lib/" + abi + "/libarchive_android.so"));
                zip.write(1);
                zip.closeEntry();
            }
            VerifiedUpdateInstaller.verifyAbi(candidate, abi);
            refuses(() -> VerifiedUpdateInstaller.verifyAbi(candidate, "unsupported-test-abi"));
        } finally { assertTrue(candidate.delete() || !candidate.exists()); }
    }
}
