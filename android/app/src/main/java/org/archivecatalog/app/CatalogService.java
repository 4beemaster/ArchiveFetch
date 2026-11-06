package org.archivecatalog.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.Environment;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.AtomicFile;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.security.cert.Certificate;
import java.util.Base64;
import java.util.Enumeration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import org.json.JSONArray;
import org.json.JSONObject;

public final class CatalogService extends Service {
    final ScheduledExecutorService worker = Executors.newSingleThreadScheduledExecutor();
    final CompletableFuture<Void> ready = new CompletableFuture<>();
    private final IBinder binder = new LocalBinder();
    private PowerManager.WakeLock wake;
    private String token = "";
    private boolean foreground;
    private boolean stopping;
    final class LocalBinder extends Binder { CatalogService service() { return CatalogService.this; } }

    @Override public void onCreate() {
        super.onCreate();
        NotificationManager notifications = getSystemService(NotificationManager.class);
        notifications.createNotificationChannel(new NotificationChannel("downloads", "Downloads", NotificationManager.IMPORTANCE_LOW));
        wake = getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "ArchiveCatalog:Downloads");
        wake.setReferenceCounted(false);
        promote("Starting local catalog");
        worker.execute(() -> {
            try {
                NativeBridge.initialize(this);
                File ca = new File(getFilesDir(), "android-trust.pem");
                writeTrust(ca);
                extractPayload(new File(getFilesDir(), "payload"));
                File downloads = getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS);
                if (downloads == null) downloads = new File(getFilesDir(), "downloads");
                JSONObject result = new JSONObject(NativeBridge.start(getFilesDir().getAbsolutePath(), downloads.getAbsolutePath(), ca.getAbsolutePath()));
                if (result.has("error")) throw new IllegalStateException(result.getString("error"));
                token = new JSONObject(NativeBridge.request("/session", "{}", "")).getString("token");
                ready.complete(null);
            } catch (Exception failure) { ready.completeExceptionally(failure); }
        });
        worker.scheduleWithFixedDelay(this::poll, 1, 1, TimeUnit.SECONDS);
    }
    // The native side sends this file to a console's loader; it is copied again only when the bundled version changes.
    void extractPayload(File destination) throws Exception {
        String bundled;
        try (java.io.InputStream input = getAssets().open("payload/ArchiveFetch.version")) {
            java.io.ByteArrayOutputStream text = new java.io.ByteArrayOutputStream();
            copy(input, text);
            bundled = text.toString("UTF-8");
        } catch (java.io.FileNotFoundException missing) { return; }
        File version = new File(destination, "ArchiveFetch.version");
        if (version.exists() && new String(java.nio.file.Files.readAllBytes(version.toPath()), StandardCharsets.UTF_8).equals(bundled)) return;
        if (!destination.isDirectory() && !destination.mkdirs()) throw new IllegalStateException("Cannot prepare the console payload");
        version.delete();
        try (java.io.InputStream input = getAssets().open("payload/ArchiveFetch.elf");
             FileOutputStream output = new FileOutputStream(new File(destination, "ArchiveFetch.elf"))) { copy(input, output); }
        try (FileOutputStream output = new FileOutputStream(version)) { output.write(bundled.getBytes(StandardCharsets.UTF_8)); }
    }
    private static void copy(java.io.InputStream input, java.io.OutputStream output) throws java.io.IOException {
        byte[] buffer = new byte[65536];
        for (int read; (read = input.read(buffer)) > 0; ) output.write(buffer, 0, read);
    }
    static void writeTrust(File destination) throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidCAStore");
        store.load(null);
        StringBuilder pem = new StringBuilder();
        Enumeration<String> aliases = store.aliases();
        while (aliases.hasMoreElements()) {
            Certificate certificate = store.getCertificate(aliases.nextElement());
            if (certificate != null) pem.append("-----BEGIN CERTIFICATE-----\n")
                .append(Base64.getMimeEncoder(64, new byte[]{'\n'}).encodeToString(certificate.getEncoded()))
                .append("\n-----END CERTIFICATE-----\n");
        }
        if (pem.length() == 0) throw new IllegalStateException("Android trust store is empty");
        AtomicFile atomic = new AtomicFile(destination);
        FileOutputStream output = atomic.startWrite();
        try { output.write(pem.toString().getBytes(StandardCharsets.US_ASCII)); atomic.finishWrite(output); }
        catch (Exception failure) { atomic.failWrite(output); throw failure; }
    }
    private Notification notification(String text) {
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class), PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        PendingIntent stop = PendingIntent.getService(this, 1, new Intent(this, CatalogService.class).setAction("stop"), PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        return new Notification.Builder(this, "downloads").setSmallIcon(R.drawable.ic_archive).setContentTitle(DownloadStorage.APP_NAME)
            .setContentText(text).setContentIntent(open).setOngoing(true).setOnlyAlertOnce(true)
            .addAction(new Notification.Action.Builder(null, "Stop", stop).build()).build();
    }
    void promote(String text) { startForeground(1, notification(text)); foreground = true; }
    private void poll() {
        if (stopping || !ready.isDone()) return;
        try {
            ready.join();
            JSONObject state = new JSONObject(NativeBridge.request("/api/state", "{}", token));
            if (state.optBoolean("busy")) {
                wake.acquire(TimeUnit.MINUTES.toMillis(10));
                String text = state.optString("activity", "Working");
                JSONArray queue = state.optJSONArray("queue");
                if (queue != null) for (int index = 0; index < queue.length(); ++index) {
                    JSONObject entry = queue.getJSONObject(index);
                    if (entry.getString("status").startsWith("Transferring")) {
                        long total = entry.optLong("size");
                        text = entry.getString("name") + " " + (total > 0 ? Math.min(100, entry.optLong("received") * 100 / total) : 0) + "%";
                        break;
                    }
                }
                if (foreground) getSystemService(NotificationManager.class).notify(1, notification(text));
            } else idle();
        } catch (Exception failure) { idle(); }
    }
    private void idle() {
        if (wake.isHeld()) wake.release();
        if (foreground) { stopForeground(STOP_FOREGROUND_REMOVE); foreground = false; stopSelf(); }
    }
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        promote("Local catalog active");
        if (intent != null && "stop".equals(intent.getAction())) worker.execute(() -> NativeBridge.request("/api/command", "{\"action\":\"stop\"}", token));
        return START_NOT_STICKY;
    }
    @Override public IBinder onBind(Intent intent) { return binder; }
    @Override public void onTimeout(int startId, int foregroundServiceType) {
        worker.execute(() -> NativeBridge.request("/api/command", "{\"action\":\"stop\"}", token));
        stopForeground(STOP_FOREGROUND_REMOVE); foreground = false; stopSelf();
    }
    @Override public void onDestroy() {
        stopping = true;
        if (wake.isHeld()) wake.release();
        worker.execute(() -> NativeBridge.stop(token));
        worker.shutdown();
        super.onDestroy();
    }
}