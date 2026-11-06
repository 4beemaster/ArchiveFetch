package org.archivecatalog.app;

import android.content.Context;
import android.content.SharedPreferences;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.util.Arrays;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;
import org.json.JSONObject;

final class NativeBridge {
    static { System.loadLibrary("archive_android"); }
    private static SharedPreferences preferences;
    private static DownloadStorage downloadStorage;
    private static String version = "unknown";
    private static final String ALIAS = "archive_catalog_s3_v1";
    static void initialize(Context value) {
        try { version = value.getPackageManager().getPackageInfo(value.getPackageName(), 0).versionName; }
        catch (android.content.pm.PackageManager.NameNotFoundException failure) { throw new IllegalStateException("Cannot identify installed app version", failure); }
        preferences = value.getApplicationContext().getSharedPreferences("credentials", Context.MODE_PRIVATE);
        downloadStorage = new DownloadStorage(value.getApplicationContext());
    }
    static String appVersion() { return version; }
    static native String start(String root, String downloads, String ca);
    static native String request(String path, String body, String token);
    static native void stop(String token);
    static native boolean copyCancelled();
    static native void copyProgress(long received, long total);
    static String storageRequest(String text) throws Exception {
        try { return downloadStorage.request(new JSONObject(text)).toString(); }
        catch (Exception failure) { return new JSONObject().put("error", failure.getMessage() == null ? "Cannot access the selected folder" : failure.getMessage()).toString(); }
    }

    private static SecretKey key(boolean create) throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        if (store.containsAlias(ALIAS)) return (SecretKey) store.getKey(ALIAS, null);
        if (!create) throw new IllegalStateException("Saved credential key is unavailable");
        KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        generator.init(new KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setKeySize(256).build());
        return generator.generateKey();
    }
    static String readCredentials() throws Exception {
        String saved = preferences.getString("sealed", null);
        if (saved == null) return null;
        JSONObject value = new JSONObject(saved);
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, key(false), new GCMParameterSpec(128, Base64.decode(value.getString("iv"), Base64.NO_WRAP)));
        byte[] plain = cipher.doFinal(Base64.decode(value.getString("data"), Base64.NO_WRAP));
        try { return new String(plain, StandardCharsets.UTF_8); }
        finally { Arrays.fill(plain, (byte) 0); }
    }
    static String writeCredentials(String value) throws Exception {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, key(true));
        byte[] plain = value.getBytes(StandardCharsets.UTF_8);
        byte[] encrypted;
        try { encrypted = cipher.doFinal(plain); }
        finally { Arrays.fill(plain, (byte) 0); }
        String sealed = new JSONObject().put("iv", Base64.encodeToString(cipher.getIV(), Base64.NO_WRAP))
            .put("data", Base64.encodeToString(encrypted, Base64.NO_WRAP)).toString();
        if (!preferences.edit().putString("sealed", sealed).commit())
            throw new IllegalStateException("Cannot save encrypted credentials");
        return null;
    }
    static String forgetCredentials() {
        if (!preferences.edit().remove("sealed").commit())
            throw new IllegalStateException("Cannot remove saved credentials");
        return null;
    }
    // Console pairing keys, sealed with the same Keystore key; the native side saves the result.
    static String protectText(String value) throws Exception {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, key(true));
        byte[] plain = value.getBytes(StandardCharsets.UTF_8);
        try {
            byte[] encrypted = cipher.doFinal(plain);
            return Base64.encodeToString(cipher.getIV(), Base64.NO_WRAP) + "." + Base64.encodeToString(encrypted, Base64.NO_WRAP);
        } finally { Arrays.fill(plain, (byte) 0); }
    }
    static String unprotectText(String sealed) throws Exception {
        String[] parts = sealed.split("\\.", 2);
        if (parts.length != 2) throw new IllegalArgumentException("Invalid saved console key");
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, key(false), new GCMParameterSpec(128, Base64.decode(parts[0], Base64.NO_WRAP)));
        byte[] plain = cipher.doFinal(Base64.decode(parts[1], Base64.NO_WRAP));
        try { return new String(plain, StandardCharsets.UTF_8); }
        finally { Arrays.fill(plain, (byte) 0); }
    }
    static String deviceName() { return "Android: " + android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL; }
}