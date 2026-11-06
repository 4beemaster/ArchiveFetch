package org.archivecatalog.app;

import android.content.ContentResolver;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.UriPermission;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.MediaStore;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.UUID;
import org.json.JSONArray;
import org.json.JSONObject;

final class DownloadStorage {
    static final String APP_NAME = "ArchiveFetch";
    private static final String DEFAULT_PATH = Environment.DIRECTORY_DOWNLOADS + "/" + APP_NAME + "/";
    private final ContentResolver resolver;
    private final SharedPreferences preferences;
    private final String packageName;
    DownloadStorage(Context context) {
        resolver = context.getContentResolver();
        preferences = context.getSharedPreferences("download-location", Context.MODE_PRIVATE);
        packageName = context.getPackageName();
    }
    String defaultLocation() {
        return Build.VERSION.SDK_INT >= 29 ? new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), APP_NAME).getAbsolutePath() : "";
    }
    String tree() {
        String selected = preferences.getString("tree", "");
        if (selected.equals(new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), "PS5 ArchiveBridge").getAbsolutePath()) ||
            selected.equals(new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), "ArchiveBridge").getAbsolutePath()))
            return defaultLocation();
        return selected.isEmpty() ? defaultLocation() : selected;
    }
    @androidx.annotation.ChecksSdkIntAtLeast(api = 29)
    private boolean publicDownloads() { return Build.VERSION.SDK_INT >= 29 && tree().equals(defaultLocation()); }
    private Uri folder(String value) throws Exception {
        Uri tree = Uri.parse(value);
        if (!"content".equals(tree.getScheme()) || !DocumentsContract.isTreeUri(tree))
            throw new IllegalArgumentException("Choose a folder with the Android folder picker");
        boolean granted = false;
        for (UriPermission permission : resolver.getPersistedUriPermissions())
            if (tree.equals(permission.getUri()) && permission.isReadPermission() && permission.isWritePermission()) granted = true;
        if (!granted) throw new IllegalStateException("Folder access was revoked; choose the download folder again");
        Uri directory = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
        try (Cursor cursor = resolver.query(directory, new String[]{DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_FLAGS}, null, null, null)) {
            if (cursor == null || !cursor.moveToFirst() || !DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(0)) ||
                (cursor.getLong(1) & DocumentsContract.Document.FLAG_DIR_SUPPORTS_CREATE) == 0)
                throw new IllegalStateException("The selected folder does not allow creating files");
        }
        return directory;
    }
    private JSONArray files() throws Exception {
        String value = tree();
        JSONArray files = new JSONArray();
        if (value.isEmpty()) return files;
        if (publicDownloads()) {
            Uri collection = MediaStore.Downloads.EXTERNAL_CONTENT_URI;
            String[] columns = {MediaStore.Downloads._ID, MediaStore.Downloads.DISPLAY_NAME, MediaStore.Downloads.SIZE};
            try (Cursor cursor = resolver.query(collection, columns,
                MediaStore.Downloads.RELATIVE_PATH + "=? AND " + MediaStore.Downloads.OWNER_PACKAGE_NAME + "=? AND " + MediaStore.Downloads.IS_PENDING + "=0",
                new String[]{DEFAULT_PATH, packageName}, null)) {
                if (cursor == null) throw new IllegalStateException("Cannot list the default download folder");
                while (cursor.moveToNext()) files.put(new JSONObject().put("name", cursor.getString(1))
                    .put("size", cursor.isNull(2) ? -1 : cursor.getLong(2))
                    .put("uri", ContentUris.withAppendedId(collection, cursor.getLong(0)).toString()));
            }
            return files;
        }
        folder(value);
        Uri tree = Uri.parse(value);
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_SIZE, DocumentsContract.Document.COLUMN_MIME_TYPE};
        try (Cursor cursor = resolver.query(children, columns, null, null, null)) {
            if (cursor == null) throw new IllegalStateException("Cannot list the selected download folder");
            while (cursor.moveToNext()) {
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(3))) continue;
                files.put(new JSONObject().put("name", cursor.getString(1)).put("size", cursor.isNull(2) ? -1 : cursor.getLong(2))
                    .put("uri", DocumentsContract.buildDocumentUriUsingTree(tree, cursor.getString(0)).toString()));
            }
        }
        return files;
    }
    private Uri find(String name) throws Exception {
        JSONArray files = files();
        for (int index = 0; index < files.length(); ++index) {
            JSONObject file = files.getJSONObject(index);
            if (name.equals(file.getString("name"))) return Uri.parse(file.getString("uri"));
        }
        return null;
    }
    private void checkSize(Uri document, long expected) throws Exception {
        if (publicDownloads()) {
            try (android.os.ParcelFileDescriptor descriptor = resolver.openFileDescriptor(document, "r")) {
                if (descriptor == null || descriptor.getStatSize() != expected)
                    throw new IllegalStateException("Destination file size could not be verified");
            }
            return;
        }
        try (Cursor cursor = resolver.query(document, new String[]{DocumentsContract.Document.COLUMN_SIZE}, null, null, null)) {
            if (cursor == null || !cursor.moveToFirst() || cursor.isNull(0) || cursor.getLong(0) != expected)
                throw new IllegalStateException("Destination file size could not be verified");
        }
    }
    private Uri createTemporary() throws Exception {
        String name = ".archive-" + UUID.randomUUID() + ".part";
        if (!publicDownloads()) return DocumentsContract.createDocument(resolver, folder(tree()), "application/octet-stream", name);
        ContentValues values = new ContentValues();
        values.put(MediaStore.Downloads.DISPLAY_NAME, name);
        values.put(MediaStore.Downloads.MIME_TYPE, "application/octet-stream");
        values.put(MediaStore.Downloads.RELATIVE_PATH, DEFAULT_PATH);
        values.put(MediaStore.Downloads.IS_PENDING, 1);
        return resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
    }
    private Uri rename(Uri document, String name) throws Exception {
        if (!publicDownloads()) return DocumentsContract.renameDocument(resolver, document, name);
        ContentValues values = new ContentValues();
        values.put(MediaStore.Downloads.DISPLAY_NAME, name);
        values.put(MediaStore.Downloads.IS_PENDING, 0);
        return resolver.update(document, values, null, null) == 1 ? document : null;
    }
    private void delete(Uri document) throws Exception {
        if (publicDownloads()) resolver.delete(document, null, null);
        else DocumentsContract.deleteDocument(resolver, document);
    }
    private void publish(JSONObject request) throws Exception {
        String name = request.getString("name");
        if (name.isEmpty() || name.contains("/") || name.contains("\\") || name.equals(".") || name.equals(".."))
            throw new IllegalArgumentException("Invalid download filename");
        File source = new File(request.getString("path"));
        long expected = request.getLong("size");
        if (!source.isFile() || source.length() != expected) throw new IllegalStateException("Verified staging file is unavailable");
        if (find(name) != null && !request.getBoolean("overwrite")) throw new IllegalStateException("Destination already exists; approve replacement before retrying");
        Uri temporary = createTemporary();
        if (temporary == null) throw new IllegalStateException("Cannot create a file in the selected folder");
        Uri backup = null;
        boolean committed = false;
        try {
            try (InputStream input = new FileInputStream(source); OutputStream output = resolver.openOutputStream(temporary, "w")) {
                if (output == null) throw new IllegalStateException("Cannot write to the selected folder");
                byte[] buffer = new byte[262144];
                long total = 0;
                int count;
                while ((count = input.read(buffer)) != -1) {
                    if (NativeBridge.copyCancelled()) throw new java.io.InterruptedIOException("Download stopped while saving to the selected folder");
                    output.write(buffer, 0, count); total += count;
                    NativeBridge.copyProgress(total, expected);
                }
                if (total != expected) throw new IllegalStateException("Staging file changed during copy");
                output.flush();
            }
            checkSize(temporary, expected);
            if (NativeBridge.copyCancelled()) throw new java.io.InterruptedIOException("Download stopped while saving to the selected folder");
            Uri existing = find(name);
            if (existing != null) {
                if (!request.getBoolean("overwrite")) throw new IllegalStateException("Destination already exists; approve replacement before retrying");
                backup = rename(existing, ".archive-backup-" + UUID.randomUUID());
                if (backup == null) throw new IllegalStateException("This provider cannot safely replace files; choose another folder");
            }
            Uri renamed = rename(temporary, name);
            if (renamed == null) throw new IllegalStateException("This provider cannot finalize downloads; choose another folder");
            temporary = renamed;
            if (!temporary.equals(find(name))) throw new IllegalStateException("The folder provider changed the destination filename");
            checkSize(temporary, expected);
            committed = true;
        } finally {
            if (!committed) {
                try { delete(temporary); } catch (Exception ignored) { }
                if (backup != null && rename(backup, name) == null)
                    throw new IllegalStateException("Original file remains in the selected folder under its .archive-backup name");
            } else if (backup != null) {
                try { delete(backup); } catch (Exception ignored) { }
            }
        }
    }
    JSONObject request(JSONObject request) throws Exception {
        String action = request.getString("action");
        if (action.equals("tree")) return new JSONObject().put("tree", tree());
        if (action.equals("list")) return new JSONObject().put("files", files());
        if (action.equals("exists")) return new JSONObject().put("exists", find(request.getString("name")) != null);
        if (action.equals("validate") || action.equals("select")) {
            String value = request.getString("tree");
            if (!value.isEmpty() && !value.equals(defaultLocation())) folder(value);
            if (action.equals("select") && !preferences.edit().putString("tree", value).commit())
                throw new IllegalStateException("Cannot remember the selected download folder");
        } else if (action.equals("publish")) publish(request);
        else throw new IllegalArgumentException("Unknown storage operation");
        return new JSONObject().put("ok", true);
    }
}