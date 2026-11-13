package org.archivecatalog.app;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;
import androidx.core.content.FileProvider;
import androidx.webkit.JavaScriptReplyProxy;
import androidx.webkit.WebViewAssetLoader;
import androidx.webkit.WebViewCompat;
import androidx.webkit.WebViewFeature;
import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;
import org.json.JSONArray;
import org.json.JSONObject;

public final class MainActivity extends Activity {
    private static final String ORIGIN = "https://appassets.androidplatform.net";
    private WebView web;
    private CatalogService service;
    private boolean bound;
    private boolean pageLoaded;
    private Bundle savedWebState;
    private boolean resumed;
    private boolean notificationRequested;
    private JavaScriptReplyProxy pickerReply;
    private long pickerId;
    private FrameLayout content;
    private final ServiceConnection connection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            service = ((CatalogService.LocalBinder) binder).service();
            service.ready.whenComplete((unused, failure) -> runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) return;
                if (failure != null) { showError("Cannot start native catalog: " + failure.getMessage()); return; }
                if (!pageLoaded) {
                    pageLoaded = true;
                    if (savedWebState == null || web.restoreState(savedWebState) == null)
                        web.loadUrl(ORIGIN + "/assets/index.html");
                    savedWebState = null;
                } else web.evaluateJavascript("reconnectNativeSession()", null);
            }));
        }
        @Override public void onServiceDisconnected(ComponentName name) { service = null; }
    };
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        savedWebState = saved == null ? null : saved.getBundle("webState");
        content = new FrameLayout(this);
        setContentView(content);
        content.setOnApplyWindowInsetsListener((view, insets) -> {
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets bars = insets.getInsets(android.view.WindowInsets.Type.systemBars() | android.view.WindowInsets.Type.displayCutout() | android.view.WindowInsets.Type.ime());
                view.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            } else view.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(), insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            return insets;
        });
        if (!WebViewFeature.isFeatureSupported(WebViewFeature.WEB_MESSAGE_LISTENER)) { showError("Update Android System WebView to use ArchiveFetch."); return; }
        WebView.setWebContentsDebuggingEnabled(false);
        web = new WebView(this);
        web.setBackgroundColor(0xff171c1b);
        WebSettings settings = web.getSettings();
        settings.setJavaScriptEnabled(true);
        // The page keeps the chosen target (this device or a console) in localStorage across reloads.
        settings.setDomStorageEnabled(true);
        settings.setAllowFileAccess(false);
        settings.setAllowContentAccess(false);
        settings.setBlockNetworkLoads(true);
        settings.setMixedContentMode(WebSettings.MIXED_CONTENT_NEVER_ALLOW);
        settings.setSupportMultipleWindows(false);
        WebViewAssetLoader assets = new WebViewAssetLoader.Builder().addPathHandler("/assets/", new WebViewAssetLoader.AssetsPathHandler(this)).build();
        web.setWebViewClient(new WebViewClient() {
            @Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) { return !trusted(request.getUrl()); }
            @Override public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                if (!trusted(request.getUrl())) return new WebResourceResponse("text/plain", "UTF-8", 403, "Forbidden", Collections.emptyMap(), new ByteArrayInputStream(new byte[0]));
                WebResourceResponse response = assets.shouldInterceptRequest(request.getUrl());
                if (response != null) {
                    Map<String, String> headers = new HashMap<>();
                    headers.put("Content-Security-Policy", "default-src 'none'; script-src 'self'; style-src 'self'; font-src 'self'; img-src 'self' data:; connect-src 'none'; frame-src 'none'; base-uri 'none'; form-action 'none'");
                    headers.put("X-Content-Type-Options", "nosniff");
                    headers.put("Cache-Control", "no-store");
                    response.setResponseHeaders(headers);
                    return response;
                }
                return new WebResourceResponse("text/plain", "UTF-8", 404, "Not found", Collections.emptyMap(), new ByteArrayInputStream(new byte[0]));
            }
        });
        WebViewCompat.addWebMessageListener(web, "ArchiveTransport", Collections.singleton(ORIGIN), (view, message, origin, mainFrame, reply) -> {
            if (!mainFrame || !ORIGIN.equals(origin.toString())) return;
            try {
                String text = message.getData();
                if (text == null || text.length() > 1024 * 1024) throw new IllegalArgumentException("App message is too large");
                JSONObject value = new JSONObject(text);
                long id = value.getLong("id");
                if (service == null) { respond(reply, id, null, "Native service disconnected"); return; }
                JSONObject data = value.optJSONObject("data");
                if ("/api/clipboard".equals(value.getString("path"))) {
                    getSystemService(android.content.ClipboardManager.class).setPrimaryClip(ClipData.newPlainText("ArchiveFetch diagnostics", data == null ? "" : data.optString("text")));
                    respond(reply, id, new JSONObject().put("ok", true), null); return;
                }
                String action = data == null ? "" : data.optString("action");
                if ("/api/command".equals(value.getString("path")) && "chooseDirectory".equals(action)) {
                    String purpose = data.optString("purpose", "destination-input");
                    if (!"destination-input".equals(purpose) && !"location-path".equals(purpose)) {
                        respond(reply, id, null, "Android export folders are managed privately by the app"); return;
                    }
                    JSONObject currentState = new JSONObject(NativeBridge.request("/api/state", "{}", value.optString("token")));
                    if ("destination-input".equals(purpose) && currentState.optJSONArray("queue") != null && currentState.getJSONArray("queue").length() > 0) {
                        respond(reply, id, null, "Finish or remove queued downloads before changing the destination folder"); return;
                    }
                    if (!currentState.optBoolean("busy", true)) {
                        if (pickerReply != null) { respond(reply, id, null, "A document picker is already open"); return; }
                        pickerReply = reply; pickerId = id;
                        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                            Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
                        String tree = new JSONObject(NativeBridge.storageRequest("{\"action\":\"tree\"}")).optString("tree");
                        if (!tree.isEmpty() && android.provider.DocumentsContract.isTreeUri(Uri.parse(tree))) picker.putExtra(android.provider.DocumentsContract.EXTRA_INITIAL_URI, Uri.parse(tree));
                        else picker.putExtra(android.provider.DocumentsContract.EXTRA_INITIAL_URI,
                            android.provider.DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:Download"));
                        startActivityForResult(picker, 42);
                    } else respond(reply, id, null, "Stop downloads before choosing another folder");
                    return;
                }
                if ("/api/command".equals(value.getString("path")) && "chooseCatalog".equals(action)) {
                    if (pickerReply != null) { respond(reply, id, null, "A document picker is already open"); return; }
                    pickerReply = reply; pickerId = id;
                    startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*").addCategory(Intent.CATEGORY_OPENABLE)
                        .putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/xml", "text/xml"}), 41);
                    return;
                }
                if (("scan".equals(action) || "start".equals(action) || "enqueue".equals(action) || "addSource".equals(action)) && !resumed) {
                    respond(reply, id, null, "Open the app before starting new work"); return;
                }
                if ("scan".equals(action) || "start".equals(action) || "enqueue".equals(action) || "addSource".equals(action))
                    startForegroundService(new Intent(this, CatalogService.class));
                CatalogService current = service;
                current.worker.execute(() -> {
                    try {
                        JSONObject result = new JSONObject(NativeBridge.request(value.getString("path"), data == null ? "{}" : data.toString(), value.optString("token")));
                        if (result.has("installUpdate")) {
                            if (!"/api/command".equals(value.getString("path")) || !"installUpdate".equals(action) ||
                                !result.optBoolean("ok"))
                                throw new IllegalStateException("Unexpected native update installation response");
                            JSONObject update = result.getJSONObject("installUpdate");
                            File verified = VerifiedUpdateInstaller.verify(this, update);
                            result.remove("installUpdate");
                            runOnUiThread(() -> installVerifiedUpdate(current, reply, id, result, verified,
                                update.optString("version"), value.optString("token")));
                            return;
                        }
                        if (result.optBoolean("openS3Keys")) {
                            result.remove("openS3Keys");
                            runOnUiThread(() -> {
                                try {
                                    startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse("https://archive.org/login?referer=%2Faccount%2Fs3.php")));
                                    respond(reply, id, result, null);
                                } catch (android.content.ActivityNotFoundException failure) {
                                    respond(reply, id, null, "No browser is available to open Internet Archive S3 keys");
                                }
                            });
                            return;
                        }
                        if (result.has("openUpdate")) {
                            Uri uri = updateLink(result.getString("openUpdate"));
                            result.remove("openUpdate");
                            runOnUiThread(() -> {
                                try {
                                    startActivity(new Intent(Intent.ACTION_VIEW, uri));
                                    respond(reply, id, result, null);
                                } catch (android.content.ActivityNotFoundException failure) {
                                    respond(reply, id, null, "No browser is available to open the ArchiveFetch release");
                                }
                            });
                            return;
                        }
                        if (result.has("shareFiles")) {
                            JSONArray files = result.getJSONArray("shareFiles"); result.remove("shareFiles");
                            runOnUiThread(() -> share(files));
                        }
                        respond(reply, id, result, null);
                    } catch (Exception failure) { respond(reply, id, null, failure.getMessage()); }
                });
            } catch (Exception failure) { showError(failure.getMessage()); }
        });
        content.addView(web, new FrameLayout.LayoutParams(-1, -1));
        if (Build.VERSION.SDK_INT >= 33)
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, this::handleBack);
    }
    static Uri updateLink(String url) {
        Uri uri = Uri.parse(url);
        String path = uri.getEncodedPath();
        String version = "v[0-9]+\\.[0-9]+\\.[0-9]+(?:-beta\\.[1-9][0-9]*)?";
        if (!"https".equals(uri.getScheme()) || !"github.com".equals(uri.getHost()) || uri.getPort() != -1 ||
            uri.getUserInfo() != null || uri.getQuery() != null || uri.getFragment() != null || path == null ||
            path.contains("..") || !path.matches("/4beemaster/ArchiveFetch/releases/(?:tag/" + version +
                "|download/" + version + "/[A-Za-z0-9.-]+)"))
            throw new IllegalArgumentException("Only official ArchiveFetch release links can be opened");
        return uri;
    }
    private void installVerifiedUpdate(CatalogService current, JavaScriptReplyProxy reply, long id,
            JSONObject result, File verified, String version, String token) {
        if (!resumed || isFinishing() || isDestroyed()) {
            respond(reply, id, null, "Open ArchiveFetch and retry Install update");
            return;
        }
        if (!getPackageManager().canRequestPackageInstalls()) {
            respond(reply, id, null, "Allow ArchiveFetch to install updates in Android settings, then retry Install update");
            new AlertDialog.Builder(this).setTitle("Allow ArchiveFetch updates")
                .setMessage("Android requires permission to open this verified APK in the package installer. Allow from this source, return here, then tap Install update again.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Open settings", (dialog, which) -> {
                    try {
                        startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                            Uri.parse("package:" + getPackageName())));
                    } catch (android.content.ActivityNotFoundException | SecurityException failure) {
                        showError("Cannot open Android install permission settings: " + failure.getMessage());
                    }
                }).show();
            return;
        }
        try {
            startActivity(VerifiedUpdateInstaller.installerIntent(this, verified));
        } catch (android.content.ActivityNotFoundException | SecurityException | IllegalArgumentException failure) {
            respond(reply, id, null, "Cannot open the Android package installer: " + failure.getMessage());
            return;
        }
        current.worker.execute(() -> {
            try {
                JSONObject started = new JSONObject(NativeBridge.request("/api/command",
                    new JSONObject().put("action", "updateInstallStarted").put("version", version).toString(), token));
                if (started.has("error") || !"installing".equals(started.optString("phase")) ||
                    !version.equals(started.optString("version")))
                    throw new IllegalStateException(started.optString("error", "Native update state was not acknowledged"));
                respond(reply, id, result, null);
            } catch (Exception failure) {
                respond(reply, id, null, "Android installer opened, but update state could not be saved: " + failure.getMessage());
            }
        });
    }
    private static boolean trusted(Uri uri) { return "https".equals(uri.getScheme()) && "appassets.androidplatform.net".equals(uri.getHost()) && uri.getPort() == -1 && uri.getPath() != null && uri.getPath().startsWith("/assets/"); }
    private void respond(JavaScriptReplyProxy reply, long id, JSONObject result, String failure) {
        runOnUiThread(() -> {
            if (isFinishing() || isDestroyed()) return;
            try {
                JSONObject response = new JSONObject().put("id", id).put("result", result == null ? new JSONObject().put("error", failure) : result);
                reply.postMessage(response.toString());
            } catch (Exception ignored) { }
        });
    }
    private void share(JSONArray files) {
        try {
            ArrayList<Uri> uris = new ArrayList<>();
            for (int index = 0; index < files.length(); ++index) {
                String path = files.getString(index);
                uris.add(path.startsWith("content://") ? Uri.parse(path) : FileProvider.getUriForFile(this, getPackageName() + ".files", new File(path)));
            }
            Intent intent = new Intent(Intent.ACTION_SEND_MULTIPLE).setType("application/octet-stream").putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            ClipData clip = ClipData.newUri(getContentResolver(), "Downloads", uris.get(0));
            for (int index = 1; index < uris.size(); ++index) clip.addItem(new ClipData.Item(uris.get(index)));
            intent.setClipData(clip);
            startActivity(Intent.createChooser(intent, "Share downloads"));
        } catch (Exception failure) { showError("Cannot share files: " + failure.getMessage()); }
    }
    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if ((requestCode != 41 && requestCode != 42) || pickerReply == null) return;
        JavaScriptReplyProxy reply = pickerReply; long id = pickerId; pickerReply = null;
        if (resultCode != RESULT_OK || data == null || data.getData() == null) { respond(reply, id, new JSONObject(), null); return; }
        if (service == null) { respond(reply, id, null, "Native service disconnected"); return; }
        Uri uri = data.getData();
        if (requestCode == 42) {
            try {
                int flags = data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                if (flags != (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION))
                    throw new IllegalStateException("The selected folder must grant read and write access");
                getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            } catch (Exception failure) { respond(reply, id, null, "Cannot retain folder access: " + failure.getMessage()); return; }
            service.worker.execute(() -> {
                try {
                    JSONObject checked = new JSONObject(NativeBridge.storageRequest(new JSONObject().put("action", "validate").put("tree", uri.toString()).toString()));
                    if (checked.has("error")) throw new IllegalStateException(checked.getString("error"));
                    respond(reply, id, new JSONObject().put("path", uri.toString()), null);
                } catch (Exception failure) { respond(reply, id, null, failure.getMessage()); }
            });
            return;
        }
        service.worker.execute(() -> {
            File destination = new File(getFilesDir(), "import.xml");
            android.util.AtomicFile atomic = new android.util.AtomicFile(destination);
            FileOutputStream output = null;
            try (InputStream input = getContentResolver().openInputStream(uri)) {
                if (input == null) throw new IllegalStateException("Cannot open selected document");
                output = atomic.startWrite();
                byte[] buffer = new byte[32768]; int count; long total = 0;
                while ((count = input.read(buffer)) != -1) {
                    total += count;
                    if (total > 64L * 1024 * 1024) throw new IllegalArgumentException("Catalog exceeds 64 MiB");
                    output.write(buffer, 0, count);
                }
                atomic.finishWrite(output); output = null;
                respond(reply, id, new JSONObject().put("path", destination.getAbsolutePath()), null);
            } catch (Exception failure) {
                if (output != null) atomic.failWrite(output);
                respond(reply, id, null, failure.getMessage());
            }
        });
    }
    private void showError(String message) { if (!isFinishing() && !isDestroyed()) new AlertDialog.Builder(this).setTitle("ArchiveFetch").setMessage(message).setPositiveButton("OK", null).show(); }
    private void bindCatalog() {
        if (web != null && !bound) {
            startForegroundService(new Intent(this, CatalogService.class));
            bound = bindService(new Intent(this, CatalogService.class), connection, Context.BIND_AUTO_CREATE);
            if (Build.VERSION.SDK_INT >= 33 && !notificationRequested && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                notificationRequested = true;
                requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, 7);
            }
        }
    }
    @Override protected void onSaveInstanceState(Bundle saved) {
        if (web != null && pageLoaded) {
            Bundle webState = new Bundle();
            web.saveState(webState);
            saved.putBundle("webState", webState);
        }
        super.onSaveInstanceState(saved);
    }
    @Override protected void onStart() {
        super.onStart(); resumed = true;
        bindCatalog();
    }
    @Override protected void onStop() {
        resumed = false;
        super.onStop();
    }
    @Override public void onBackPressed() {
        handleBack();
    }
    private void handleBack() {
        if (web == null) { finish(); return; }
        web.evaluateJavascript("(() => { const dialog = document.querySelector('dialog[open]'); if (!dialog) return false; if (dialog.id === 'confirm-dialog') answerConfirmation(false); else dialog.close(); return true; })()",
            result -> { if (!"true".equals(result)) finish(); });
    }
    @Override protected void onDestroy() {
        if (bound) { unbindService(connection); bound = false; }
        if (web != null) { content.removeView(web); web.destroy(); }
        super.onDestroy();
    }
}