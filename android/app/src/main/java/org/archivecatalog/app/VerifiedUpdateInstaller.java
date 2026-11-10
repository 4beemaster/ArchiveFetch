package org.archivecatalog.app;

import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.net.Uri;
import android.os.Build;
import androidx.core.content.FileProvider;
import java.io.File;
import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.HashSet;
import java.util.zip.ZipFile;
import org.json.JSONObject;

final class VerifiedUpdateInstaller {
    private VerifiedUpdateInstaller() { }

    static File stagedFile(Context context, String path) throws Exception {
        File requested = new File(path);
        File suppliedRoot = new File(context.getFilesDir(), "updates").getAbsoluteFile();
        File root = new File(context.getFilesDir().getCanonicalFile(), "updates");
        File canonical = requested.getCanonicalFile();
        // Android may alias the app data root; links within update staging are still refused.
        File allowedRoot = requested.toPath().startsWith(suppliedRoot.toPath()) ? suppliedRoot : root;
        if (!requested.isAbsolute() || !requested.toPath().equals(requested.toPath().normalize()) ||
            !requested.toPath().startsWith(allowedRoot.toPath()) ||
            !canonical.toPath().equals(root.toPath().resolve(allowedRoot.toPath().relativize(requested.toPath()))) ||
            !root.equals(root.getCanonicalFile()) ||
            !canonical.toPath().startsWith(root.toPath()) || !canonical.isFile() ||
            !canonical.getName().endsWith(".apk"))
            throw new IllegalArgumentException("The update APK must be a regular file in ArchiveFetch private update staging");
        return canonical;
    }

    static boolean canonicalVersion(String version) {
        return version.matches("(?:0|[1-9][0-9]*)\\.(?:0|[1-9][0-9]*)\\.(?:0|[1-9][0-9]*)(?:-beta\\.[1-9][0-9]*)?")
            && version.length() <= 80;
    }

    static void verifyDigest(File file, String expected) throws Exception {
        if (!expected.matches("[0-9a-fA-F]{64}")) throw new IllegalArgumentException("Invalid verified update SHA-256");
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (FileInputStream input = new FileInputStream(file)) {
            byte[] buffer = new byte[65536];
            for (int count; (count = input.read(buffer)) != -1;) digest.update(buffer, 0, count);
        }
        StringBuilder actual = new StringBuilder(64);
        for (byte value : digest.digest()) actual.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        if (!MessageDigest.isEqual(actual.toString().getBytes(StandardCharsets.US_ASCII),
                expected.toLowerCase(java.util.Locale.ROOT).getBytes(StandardCharsets.US_ASCII)))
            throw new IllegalArgumentException("The staged update APK failed SHA-256 verification; download it again");
    }

    @SuppressWarnings("deprecation")
    static void verifyIdentity(PackageInfo candidate, PackageInfo installed, String packageName, String version) {
        if (!canonicalVersion(version) || candidate == null || !packageName.equals(candidate.packageName) ||
            !version.equals(candidate.versionName))
            throw new IllegalArgumentException("The update APK package or version does not match the verified release");
        long candidateCode = Build.VERSION.SDK_INT >= 28 ? candidate.getLongVersionCode() : candidate.versionCode;
        long installedCode = Build.VERSION.SDK_INT >= 28 ? installed.getLongVersionCode() : installed.versionCode;
        if (candidateCode <= installedCode) throw new IllegalArgumentException("The update APK must be newer than the installed app");
        Signature[] candidateSigners = signers(candidate);
        Signature[] installedSigners = signers(installed);
        if (candidateSigners == null || installedSigners == null || candidateSigners.length == 0 ||
            installedSigners.length == 0 ||
            !new HashSet<>(Arrays.asList(candidateSigners)).equals(new HashSet<>(Arrays.asList(installedSigners))))
            throw new IllegalArgumentException("The update APK signing identity differs from the installed ArchiveFetch app");
    }

    @SuppressWarnings("deprecation")
    private static Signature[] signers(PackageInfo value) {
        if (Build.VERSION.SDK_INT >= 28)
            return value.signingInfo == null ? null : value.signingInfo.getApkContentsSigners();
        return value.signatures;
    }

    static String installedAbi(Context context) throws Exception {
        try (ZipFile installed = new ZipFile(context.getApplicationInfo().sourceDir)) {
            for (String abi : Build.SUPPORTED_ABIS)
                if (installed.getEntry("lib/" + abi + "/libarchive_android.so") != null) return abi;
        }
        throw new IllegalStateException("Cannot identify the installed ArchiveFetch native ABI");
    }

    static void verifyAbi(File file, String abi) throws Exception {
        try (ZipFile apk = new ZipFile(file)) {
            if (apk.getEntry("lib/" + abi + "/libarchive_android.so") == null)
                throw new IllegalArgumentException("The update APK does not contain the installed ArchiveFetch native ABI: " + abi);
        }
    }

    @SuppressWarnings("deprecation")
    static File verify(Context context, JSONObject update) throws Exception {
        File file = stagedFile(context, update.getString("path"));
        String version = update.getString("version");
        verifyDigest(file, update.getString("sha256"));
        PackageManager manager = context.getPackageManager();
        int flags = Build.VERSION.SDK_INT >= 28 ? PackageManager.GET_SIGNING_CERTIFICATES : PackageManager.GET_SIGNATURES;
        PackageInfo candidate = manager.getPackageArchiveInfo(file.getAbsolutePath(), flags);
        PackageInfo installed = manager.getPackageInfo(context.getPackageName(), flags);
        verifyIdentity(candidate, installed, context.getPackageName(), version);
        verifyAbi(file, installedAbi(context));
        return file;
    }

    static Intent installerIntent(Context context, File verified) {
        Uri uri = FileProvider.getUriForFile(context, context.getPackageName() + ".updates", verified);
        Intent intent = new Intent(Intent.ACTION_VIEW).setDataAndType(uri, "application/vnd.android.package-archive")
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        intent.setClipData(ClipData.newUri(context.getContentResolver(), "Verified ArchiveFetch update", uri));
        return intent;
    }
}
