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
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import org.json.JSONArray;
import org.json.JSONObject;

final class DownloadStorage {
    private static final java.util.concurrent.ScheduledExecutorService STORAGE_WATCHDOG =
        java.util.concurrent.Executors.newSingleThreadScheduledExecutor(task -> {
            Thread worker = new Thread(task, "archive-storage-watchdog"); worker.setDaemon(true); return worker;
        });
    private static final class IoTimeout implements AutoCloseable {
        private volatile long lastProgress = android.os.SystemClock.elapsedRealtime();
        private volatile boolean timedOut;
        private final java.util.concurrent.ScheduledFuture<?> timer;
        IoTimeout(java.io.Closeable... streams) {
            timer = STORAGE_WATCHDOG.scheduleWithFixedDelay(() -> {
                if (!timedOut && android.os.SystemClock.elapsedRealtime() - lastProgress > 120000) {
                    timedOut = true;
                    for (java.io.Closeable stream : streams) {
                        try { stream.close(); }
                        catch (java.io.IOException failure) { android.util.Log.e(APP_NAME, "Cannot close stalled storage stream", failure); }
                    }
                }
            }, 1, 1, java.util.concurrent.TimeUnit.SECONDS);
        }
        void progressed() throws java.io.InterruptedIOException {
            if (timedOut) throw new java.io.InterruptedIOException("Storage provider stalled for two minutes");
            lastProgress = android.os.SystemClock.elapsedRealtime();
        }
        @Override public void close() throws java.io.InterruptedIOException {
            timer.cancel(false);
            if (timedOut) throw new java.io.InterruptedIOException("Storage provider stalled for two minutes");
        }
    }
    private static final class LocationUnavailable extends java.io.IOException {
        private static final long serialVersionUID = 1L;
        LocationUnavailable(String message) { super(message); }
    }
    static final String APP_NAME = "ArchiveFetch";
    private static final String DEFAULT_PATH = Environment.DIRECTORY_DOWNLOADS + "/" + APP_NAME + "/";
    private final ContentResolver resolver;
    private final SharedPreferences preferences;
    private final String packageName;
    private File privateDirectory;
    DownloadStorage(Context context) {
        resolver = context.getContentResolver();
        preferences = context.getSharedPreferences("download-location", Context.MODE_PRIVATE);
        packageName = context.getPackageName();
        privateDirectory = new File(context.getFilesDir(), "downloads");
    }
    String defaultLocation() {
        return Build.VERSION.SDK_INT >= 29
            ? new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), APP_NAME).getAbsolutePath()
            : privateDirectory.getAbsolutePath();
    }
    String tree() {
        String selected = preferences.getString("tree", "");
        File downloads = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        if (selected.equals(new File(downloads, "PS5 ArchiveBridge").getAbsolutePath()) ||
            selected.equals(new File(downloads, "ArchiveBridge").getAbsolutePath())) return defaultLocation();
        return selected.isEmpty() ? defaultLocation() : selected;
    }
    static void safeName(String name) {
        if (name.isEmpty() || name.equals(".") || name.equals("..") || name.contains("/") || name.contains("\\")
            || name.chars().anyMatch(c -> c < 32 || c == 127))
            throw new IllegalArgumentException("Invalid storage filename");
    }
    private Uri folder(String value) throws Exception {
        Uri tree = Uri.parse(value);
        if (!"content".equals(tree.getScheme()) || !DocumentsContract.isTreeUri(tree))
            throw new IllegalArgumentException("Choose a folder with the Android folder picker");
        boolean granted = false;
        for (UriPermission permission : resolver.getPersistedUriPermissions())
            if (tree.equals(permission.getUri()) && permission.isReadPermission() && permission.isWritePermission()) granted = true;
        if (!granted) throw new LocationUnavailable("Folder access was revoked; choose the folder again");
        Uri directory = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
        try (Cursor cursor = resolver.query(directory, new String[]{DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_FLAGS}, null, null, null)) {
            if (cursor == null || !cursor.moveToFirst() || !DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(0))
                || (cursor.getLong(1) & DocumentsContract.Document.FLAG_DIR_SUPPORTS_CREATE) == 0)
                throw new IllegalStateException("The selected folder does not allow creating files");
        }
        return directory;
    }
    private final class Destination {
        final String value;
        @androidx.annotation.ChecksSdkIntAtLeast(api = 29)
        final boolean media;
        final boolean local;
        final Uri root;
        Destination(String value) throws Exception {
            this.value = value;
            media = Build.VERSION.SDK_INT >= 29 && value.equals(defaultLocation());
            local = !media && Uri.parse(value).getScheme() == null && new File(value).getCanonicalFile().equals(privateDirectory.getCanonicalFile());
            if (local) {
                if (!privateDirectory.isDirectory() && !privateDirectory.mkdirs()) throw new IllegalStateException("Cannot create private downloads");
                root = Uri.fromFile(privateDirectory);
            } else if (Build.VERSION.SDK_INT >= 29 && media) root = MediaStore.Downloads.EXTERNAL_CONTENT_URI;
            else root = folder(value);
        }
        List<JSONObject> children(Uri parent, String relative) throws Exception {
            List<JSONObject> result = new ArrayList<>();
            if (local) {
                File[] files = new File(parent.getPath()).listFiles();
                if (files == null) throw new IllegalStateException("Cannot list private downloads");
                for (File file : files) {
                    if (Files.isSymbolicLink(file.toPath())) throw new IllegalStateException("Links are not supported in downloads");
                    result.add(new JSONObject().put("name", file.getName()).put("uri", Uri.fromFile(file).toString())
                        .put("size", file.length()).put("directory", file.isDirectory()));
                }
            } else if (media) {
                String path = DEFAULT_PATH + relative;
                try (Cursor cursor = resolver.query(root, new String[]{MediaStore.Downloads._ID, MediaStore.Downloads.DISPLAY_NAME,
                        MediaStore.Downloads.SIZE, MediaStore.Downloads.RELATIVE_PATH, MediaStore.Downloads.OWNER_PACKAGE_NAME},
                        MediaStore.Downloads.RELATIVE_PATH + " LIKE ? ESCAPE '\\'",
                        new String[]{path.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_") + "%"}, null)) {
                    if (cursor == null) throw new IllegalStateException("Cannot list public Downloads");
                    while (cursor.moveToNext()) result.add(new JSONObject().put("name", cursor.getString(1))
                        .put("uri", ContentUris.withAppendedId(root, cursor.getLong(0)).toString()).put("size", cursor.getLong(2))
                        .put("relative", cursor.getString(3).substring(DEFAULT_PATH.length()))
                        .put("owned", packageName.equals(cursor.getString(4))).put("directory", false));
                }
            } else {
                Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(Uri.parse(value), DocumentsContract.getDocumentId(parent));
                try (Cursor cursor = resolver.query(children, new String[]{DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                        DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_SIZE, DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
                    if (cursor == null) throw new IllegalStateException("Cannot list the selected folder");
                    while (cursor.moveToNext()) result.add(new JSONObject().put("name", cursor.getString(1)).put("size", cursor.getLong(2))
                        .put("uri", DocumentsContract.buildDocumentUriUsingTree(Uri.parse(value), cursor.getString(0)).toString())
                        .put("directory", DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(3))));
                }
            }
            return result;
        }
        JSONObject find(String name) throws Exception {
            safeName(name);
            for (JSONObject file : children(root, "")) {
                if (media && !file.getString("relative").isEmpty()) {
                    if (file.getString("relative").startsWith(name + "/")) return file;
                } else if (name.equals(file.getString("name"))) return file;
            }
            return null;
        }
        JSONObject metadata(Uri uri) throws Exception {
            if (local) {
                File file = new File(uri.getPath());
                if (!file.getCanonicalFile().toPath().startsWith(privateDirectory.toPath()) || Files.isSymbolicLink(file.toPath()))
                    throw new IllegalStateException("Owned download path changed; it was kept");
                if (!file.exists()) return null;
                return new JSONObject().put("uri", uri.toString()).put("name", file.getName())
                    .put("size", file.length()).put("directory", file.isDirectory());
            }
            String[] columns = media
                ? new String[]{MediaStore.Downloads.DISPLAY_NAME, MediaStore.Downloads.SIZE, MediaStore.Downloads.OWNER_PACKAGE_NAME}
                : new String[]{DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_SIZE, DocumentsContract.Document.COLUMN_MIME_TYPE};
            try (Cursor cursor = resolver.query(uri, columns, null, null, null)) {
                if (cursor == null) throw new IllegalStateException("Cannot inspect owned download");
                if (!cursor.moveToFirst()) return null;
                JSONObject result = new JSONObject().put("uri", uri.toString()).put("name", cursor.getString(0))
                    .put("size", cursor.getLong(1)).put("directory", !media && DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2)));
                if (media) result.put("owned", packageName.equals(cursor.getString(2)));
                return result;
            }
        }
        JSONObject findArchive(String name) throws Exception {
            safeName(name);
            if (local) return metadata(Uri.fromFile(new File(privateDirectory, name)));
            if (!media) return find(name);
            try (Cursor cursor = resolver.query(root, new String[]{MediaStore.Downloads._ID, MediaStore.Downloads.DISPLAY_NAME,
                    MediaStore.Downloads.SIZE, MediaStore.Downloads.OWNER_PACKAGE_NAME},
                    MediaStore.Downloads.RELATIVE_PATH + " = ? AND " + MediaStore.Downloads.DISPLAY_NAME + " = ?",
                    new String[]{DEFAULT_PATH, name}, null)) {
                if (cursor == null) throw new IllegalStateException("Cannot inspect public download");
                if (!cursor.moveToFirst()) return null;
                JSONObject result = new JSONObject().put("uri", ContentUris.withAppendedId(root, cursor.getLong(0)).toString())
                    .put("name", cursor.getString(1)).put("size", cursor.getLong(2)).put("owned", packageName.equals(cursor.getString(3)))
                    .put("directory", false).put("relative", "");
                if (cursor.moveToNext()) throw new IllegalStateException("Ambiguous duplicate destination names");
                return result;
            }
        }
        Uri create(Uri parent, String name, boolean directory, String relative) throws Exception {
            safeName(name);
            Uri result;
            if (local) {
                File file = new File(parent.getPath(), name);
                boolean created = directory ? file.mkdir() : file.createNewFile();
                if (!created) throw new IllegalStateException("Destination already exists or cannot be created");
                result = Uri.fromFile(file);
            } else if (media) {
                if (directory) throw new IllegalArgumentException("MediaStore directories are represented by relative paths");
                ContentValues values = new ContentValues();
                values.put(MediaStore.Downloads.DISPLAY_NAME, name);
                values.put(MediaStore.Downloads.MIME_TYPE, "application/octet-stream");
                values.put(MediaStore.Downloads.RELATIVE_PATH, DEFAULT_PATH + relative);
                values.put(MediaStore.Downloads.IS_PENDING, 1);
                result = resolver.insert(root, values);
            } else result = DocumentsContract.createDocument(resolver, parent,
                directory ? DocumentsContract.Document.MIME_TYPE_DIR : "application/octet-stream", name);
            if (result == null) throw new IllegalStateException("Cannot create destination");
            return result;
        }
        Uri rename(Uri uri, String name, String relative) throws Exception {
            safeName(name);
            Uri result;
            if (local) {
                File before = new File(uri.getPath()), after = new File(before.getParentFile(), name);
                if (after.exists() || !before.renameTo(after)) throw new IllegalStateException("Cannot finalize destination");
                result = Uri.fromFile(after);
            } else if (media) {
                ContentValues values = new ContentValues();
                values.put(MediaStore.Downloads.DISPLAY_NAME, name);
                values.put(MediaStore.Downloads.RELATIVE_PATH, DEFAULT_PATH + relative);
                values.put(MediaStore.Downloads.IS_PENDING, 0);
                if (resolver.update(uri, values, null, null) != 1) throw new IllegalStateException("Cannot finalize public download");
                result = uri;
            } else result = DocumentsContract.renameDocument(resolver, uri, name);
            if (result == null) throw new IllegalStateException("This provider cannot finalize downloads");
            return result;
        }
        void delete(Uri uri) throws Exception {
            if (!local && !media) {
                try (Cursor cursor = resolver.query(uri, new String[]{DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
                    if (cursor == null || !cursor.moveToFirst()) throw new IllegalStateException("Cannot verify owned deletion target");
                    if (DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(0)) && !children(uri, "").isEmpty())
                        throw new IllegalStateException("Folder still contains files; it was kept");
                }
            }
            boolean deleted = local ? new File(uri.getPath()).delete()
                : media ? resolver.delete(uri, null, null) == 1 : DocumentsContract.deleteDocument(resolver, uri);
            if (!deleted) throw new IllegalStateException("Cannot delete owned download: " + uri);
        }
        InputStream read(Uri uri) throws Exception {
            InputStream input = local ? new FileInputStream(new File(uri.getPath())) : resolver.openInputStream(uri);
            if (input == null) throw new IllegalStateException("Cannot read owned download");
            return input;
        }
        OutputStream write(Uri uri) throws Exception {
            OutputStream output = local ? Files.newOutputStream(new File(uri.getPath()).toPath()) : resolver.openOutputStream(uri, "w");
            if (output == null) throw new IllegalStateException("Cannot write destination");
            return output;
        }
    }
    private static String digest(InputStream input) throws Exception { return digest(input, "SHA-256"); }
    private static String digest(InputStream input, String algorithm) throws Exception {
        MessageDigest hash = MessageDigest.getInstance(algorithm);
        byte[] buffer = new byte[262144];
        int count;
        try (IoTimeout timeout = new IoTimeout(input)) {
            while ((count = input.read(buffer)) != -1) {
                if (NativeBridge.copyCancelled()) throw new java.io.InterruptedIOException("Operation stopped during verification");
                hash.update(buffer, 0, count); timeout.progressed();
            }
        }
        StringBuilder text = new StringBuilder();
        for (byte value : hash.digest()) text.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        return text.toString();
    }
    private JSONObject copy(Destination destination, File source, Uri uri, String name) throws Exception {
        if (!source.isFile() || Files.isSymbolicLink(source.toPath())) throw new IllegalStateException("Verified staging file is unavailable");
        long expected = source.length(), total = 0;
        try (InputStream input = new FileInputStream(source); OutputStream output = destination.write(uri); IoTimeout timeout = new IoTimeout(input, output)) {
            byte[] buffer = new byte[262144]; int count;
            while ((count = input.read(buffer)) != -1) {
                if (NativeBridge.copyCancelled()) throw new java.io.InterruptedIOException("Operation stopped during publication");
                output.write(buffer, 0, count);
                timeout.progressed();
                total += count; NativeBridge.copyProgress(total, expected);
            }
            output.flush();
        }
        if (total != expected) throw new IllegalStateException("Staging file changed during publication");
        String hash;
        try (InputStream input = new FileInputStream(source)) { hash = digest(input); }
        try (InputStream input = destination.read(uri)) {
            if (!hash.equals(digest(input))) throw new IllegalStateException("Published file verification failed");
        }
        return new JSONObject().put("uri", uri.toString()).put("name", name).put("size", expected).put("sha256", hash);
    }
    private File staging(String path) throws Exception {
        File source = new File(path);
        if (!source.getCanonicalPath().startsWith(new File(privateDirectory, ".staging").getCanonicalPath() + File.separator))
            throw new IllegalArgumentException("Publication source must be private download staging");
        return source;
    }
    private void rollback(Destination destination, List<Uri> created, Exception original) {
        for (int index = created.size() - 1; index >= 0; --index) {
            try { destination.delete(created.get(index)); }
            catch (Exception failure) { original.addSuppressed(failure); }
        }
    }
    private JSONObject publish(JSONObject request) throws Exception {
        Destination destination = new Destination(request.optString("tree", tree()));
        String name = request.getString("name"); safeName(name);
        File source = staging(request.getString("path"));
        if (source.length() != request.getLong("size")) throw new IllegalStateException("Verified staging size changed");
        JSONObject existing = destination.find(name);
        JSONObject previous = request.optJSONObject("previous");
        if (existing != null) {
            if (!request.optBoolean("overwrite") || previous == null || !destination.value.equals(previous.getJSONObject("storage").getString("tree")))
                throw new IllegalStateException("Destination already exists and is not an approved owned copy");
            if (previous.getJSONObject("storage").has("extracted")) throw new IllegalStateException("Remove the recorded extracted copy before replacing its archive");
            verify(destination, previous.getJSONObject("storage").getJSONObject("archive"), existing);
        }
        List<Uri> created = new ArrayList<>();
        Uri backup = null;
        boolean committed = false;
        try {
            Uri temporary = destination.create(destination.root, ".archive-" + UUID.randomUUID() + ".part", false, "");
            created.add(temporary);
            JSONObject file = copy(destination, source, temporary, name);
            if (NativeBridge.copyCancelled()) throw new java.io.InterruptedIOException("Publication stopped");
            if (existing != null) {
                verify(destination, previous.getJSONObject("storage").getJSONObject("archive"), destination.find(name));
                backup = destination.rename(Uri.parse(existing.getString("uri")), ".archive-backup-" + UUID.randomUUID(), "");
            } else if (destination.find(name) != null) throw new IllegalStateException("Destination appeared during publication");
            Uri finalUri = destination.rename(temporary, name, "");
            created.set(0, finalUri); file.put("uri", finalUri.toString());
            JSONObject found = destination.find(name);
            if (found == null || !finalUri.toString().equals(found.getString("uri"))) throw new IllegalStateException("Provider changed the destination filename");
            if (backup != null) { destination.delete(backup); backup = null; }
            committed = true;
            return new JSONObject().put("storage", new JSONObject().put("tree", destination.value).put("archive", file))
                .put("path", finalUri.getScheme().equals("file") ? finalUri.getPath() : finalUri.toString()).put("modified", 0);
        } catch (Exception failure) {
            if (!committed) rollback(destination, created, failure);
            if (backup != null) {
                try { destination.rename(backup, name, ""); }
                catch (Exception restore) { failure.addSuppressed(restore); }
            }
            throw failure;
        }
    }
    private void walkSource(Destination destination, File source, Uri parent, String relative, JSONArray entries,
            JSONArray directoryNames, List<Uri> created) throws Exception {
        File[] files = source.listFiles();
        if (files == null) throw new IllegalStateException("Cannot list extracted staging");
        for (File file : files) {
            safeName(file.getName());
            if (Files.isSymbolicLink(file.toPath())) throw new IllegalStateException("Extracted links cannot be published");
            if (file.isDirectory()) {
                directoryNames.put(relative + file.getName() + "/");
                Uri directory = destination.media ? parent : destination.create(parent, file.getName(), true, relative);
                if (!destination.media) created.add(directory);
                walkSource(destination, file, directory, relative + file.getName() + "/", entries, directoryNames, created);
            } else {
                Uri uri = destination.create(parent, file.getName(), false, relative); created.add(uri);
                entries.put(copy(destination, file, uri, relative + file.getName()));
            }
        }
    }
    private void walkDestination(Destination destination, Uri parent, String relative, Map<String, JSONObject> found,
            List<Uri> directories, Set<String> directoryNames) throws Exception {
        for (JSONObject file : destination.children(parent, relative)) {
            String name = file.getString("name"); safeName(name);
            Uri uri = Uri.parse(file.getString("uri"));
            if (file.getBoolean("directory")) {
                directories.add(uri);
                directoryNames.add(relative + name + "/");
                walkDestination(destination, uri, relative + name + "/", found, directories, directoryNames);
            } else {
                String key = destination.media ? file.getString("relative") + name : relative + name;
                if (found.put(key, file) != null) throw new IllegalStateException("Ambiguous duplicate destination names");
            }
        }
    }
    private JSONObject publishExtracted(JSONObject request) throws Exception {
        JSONObject record = request.getJSONObject("record");
        JSONObject storage = new JSONObject(record.getJSONObject("storage").toString());
        Destination destination = new Destination(storage.getString("tree"));
        String name = request.getString("name"); safeName(name);
        File source = staging(request.getString("path"));
        if (!source.isDirectory()) throw new IllegalStateException("Extracted staging is unavailable");
        if (destination.find(name) != null) throw new IllegalStateException("Extracted destination already exists; it was left unchanged");
        String temporaryName = ".archive-extract-" + UUID.randomUUID();
        List<Uri> created = new ArrayList<>();
        JSONArray entries = new JSONArray();
        JSONArray directoryNames = new JSONArray();
        Uri directory = destination.root;
        try {
            if (!destination.media) { directory = destination.create(destination.root, temporaryName, true, ""); created.add(directory); }
            walkSource(destination, source, directory, destination.media ? temporaryName + "/" : "", entries, directoryNames, created);
            if (entries.length() == 0) throw new IllegalStateException("Archive contains no publishable files");
            if (NativeBridge.copyCancelled()) throw new java.io.InterruptedIOException("Extraction publication stopped");
            if (destination.find(name) != null) throw new IllegalStateException("Extracted destination appeared during publication");
            if (destination.media) {
                for (int index = 0; index < entries.length(); ++index) {
                    JSONObject entry = entries.getJSONObject(index);
                    String relativeName = entry.getString("name").substring(temporaryName.length() + 1);
                    int slash = relativeName.lastIndexOf('/');
                    destination.rename(Uri.parse(entry.getString("uri")), slash < 0 ? relativeName : relativeName.substring(slash + 1),
                        name + "/" + (slash < 0 ? "" : relativeName.substring(0, slash + 1)));
                    entry.put("name", name + "/" + relativeName);
                }
            } else {
                directory = destination.rename(directory, name, ""); created.set(0, directory);
                // Providers may change document IDs when renaming a directory.
                Map<String, JSONObject> found = new HashMap<>();
                List<Uri> foundDirectories = new ArrayList<>();
                walkDestination(destination, directory, "", found, foundDirectories, new HashSet<>());
                created.clear(); created.add(directory); created.addAll(foundDirectories);
                for (int index = 0; index < entries.length(); ++index) {
                    JSONObject entry = entries.getJSONObject(index), file = found.get(entry.getString("name"));
                    if (file == null) throw new IllegalStateException("Published extracted file disappeared");
                    entry.put("uri", file.getString("uri"));
                    created.add(Uri.parse(file.getString("uri")));
                }
            }
            storage.put("extracted", new JSONObject().put("uri", directory.toString()).put("folder", name).put("entries", entries).put("directories", directoryNames));
            record.put("storage", storage).put("extracted", destination.media ? destination.value + "/" + name
                : destination.local ? directory.getPath() : directory.toString());
            if (!inspect(record, false).has("extracted")) throw new IllegalStateException("Provider changed the extracted folder name; archive was kept");
            return record;
        } catch (Exception failure) {
            rollback(destination, created, failure);
            throw failure;
        }
    }
    private void verifyMetadata(Destination destination, JSONObject expected, JSONObject actual) throws Exception {
        if (actual == null || actual.getBoolean("directory") || !actual.getString("uri").equals(expected.getString("uri"))
            || (destination.media && !actual.getBoolean("owned")) || actual.getLong("size") != expected.getLong("size"))
            throw new IllegalStateException("Owned download changed or is missing; it was kept");
    }
    private void verify(Destination destination, JSONObject expected, JSONObject actual) throws Exception {
        verifyMetadata(destination, expected, actual);
        try (InputStream input = destination.read(Uri.parse(actual.getString("uri")))) {
            if (!digest(input).equals(expected.getString("sha256"))) throw new IllegalStateException("Owned download contents changed; it was kept");
        }
    }
    private JSONObject metadataStatus(JSONObject record) throws Exception {
        JSONObject storage = record.getJSONObject("storage");
        Destination destination = new Destination(storage.getString("tree"));
        boolean archivePresent = false, extractedPresent = false;
        if (storage.has("archive")) {
            JSONObject archive = storage.getJSONObject("archive");
            JSONObject actual = destination.metadata(Uri.parse(archive.getString("uri")));
            if (actual != null) {
                verifyMetadata(destination, archive, actual);
                if (!actual.getString("name").equals(archive.getString("name")))
                    throw new IllegalStateException("Owned archive was renamed; it was kept");
                archivePresent = true;
            }
        }
        if (storage.has("extracted")) {
            JSONObject extracted = storage.getJSONObject("extracted");
            if (destination.media) {
                JSONArray entries = extracted.getJSONArray("entries");
                if (entries.length() > 0) {
                    JSONObject entry = entries.getJSONObject(0), actual = destination.metadata(Uri.parse(entry.getString("uri")));
                    if (actual != null) { verifyMetadata(destination, entry, actual); extractedPresent = true; }
                }
            } else {
                JSONObject actual = destination.metadata(Uri.parse(extracted.getString("uri")));
                if (actual != null) {
                    if (!actual.getBoolean("directory") || !actual.getString("name").equals(extracted.getString("folder")))
                        throw new IllegalStateException("Extracted folder changed; it was kept");
                    extractedPresent = true;
                }
            }
        }
        JSONObject result = new JSONObject().put("path", record.getString("path"))
            .put("status", archivePresent ? "Downloaded" : extractedPresent ? "Extracted" : "Missing");
        if (extractedPresent) {
            long bytes = 0;
            JSONArray entries = storage.getJSONObject("extracted").getJSONArray("entries");
            for (int index = 0; index < entries.length(); ++index) bytes += entries.getJSONObject(index).getLong("size");
            result.put("extracted", record.getString("extracted")).put("extractedBytes", bytes);
        }
        return result;
    }
    private JSONObject inspect(JSONObject record, boolean deleting) throws Exception {
        JSONObject storage = record.getJSONObject("storage");
        Destination destination = new Destination(storage.getString("tree"));
        long bytes = 0, extractedBytes = 0;
        List<Uri> files = new ArrayList<>(), directories = new ArrayList<>();
        boolean archivePresent = false, extractedPresent = false;
        if (storage.has("archive")) {
            JSONObject archive = storage.getJSONObject("archive"), actual = destination.find(archive.getString("name"));
            if (actual != null) {
                verify(destination, archive, actual); archivePresent = true; bytes += archive.getLong("size");
                files.add(Uri.parse(archive.getString("uri")));
            }
        }
        if (storage.has("extracted")) {
            JSONObject extracted = storage.getJSONObject("extracted");
            String name = extracted.getString("folder"); safeName(name);
            JSONObject actual = destination.find(name);
            if (actual != null) {
                Uri directory = destination.media ? destination.root : Uri.parse(actual.getString("uri"));
                if (!destination.media && (!actual.getBoolean("directory") || !directory.toString().equals(extracted.getString("uri"))))
                    throw new IllegalStateException("Extracted folder changed; it was kept");
                Map<String, JSONObject> found = new HashMap<>();
                Set<String> directoryNames = new HashSet<>();
                walkDestination(destination, directory, destination.media ? name + "/" : "", found, directories, directoryNames);
                if (!destination.media) {
                    Set<String> expectedDirectories = new HashSet<>();
                    JSONArray names = extracted.getJSONArray("directories");
                    for (int index = 0; index < names.length(); ++index) expectedDirectories.add(names.getString(index));
                    if (!directoryNames.equals(expectedDirectories)) throw new IllegalStateException("Extracted folder contains unowned directories; it was kept");
                }
                JSONArray entries = extracted.getJSONArray("entries");
                if (found.size() != entries.length()) throw new IllegalStateException("Extracted folder contains changed or unowned files; it was kept");
                Set<String> unique = new HashSet<>();
                for (int index = 0; index < entries.length(); ++index) {
                    JSONObject entry = entries.getJSONObject(index);
                    if (!unique.add(entry.getString("name"))) throw new IllegalStateException("Invalid extraction ownership manifest");
                    verify(destination, entry, found.get(entry.getString("name")));
                    files.add(Uri.parse(entry.getString("uri"))); extractedBytes += entry.getLong("size");
                }
                if (!destination.media) directories.add(0, directory);
                extractedPresent = true;
            }
        }
        bytes += extractedBytes;
        if (deleting) {
            for (Uri file : files) destination.delete(file);
            for (int index = directories.size() - 1; index >= 0; --index) destination.delete(directories.get(index));
        }
        JSONObject result = new JSONObject().put("path", record.getString("path")).put("status",
            archivePresent ? "Downloaded" : extractedPresent ? "Extracted" : "Missing").put("bytes", bytes);
        if (extractedPresent) result.put("extracted", record.getString("extracted")).put("extractedBytes", extractedBytes);
        return result;
    }
    JSONObject request(JSONObject request) throws Exception {
        String action = request.getString("action");
        if (action.equals("configure")) {
            privateDirectory = new File(request.getString("private")).getCanonicalFile();
            return new JSONObject().put("tree", tree());
        }
        if (action.equals("tree")) return new JSONObject().put("tree", tree());
        if (action.equals("publish")) return publish(request);
        if (action.equals("matchesStaging")) {
            File source = staging(request.getString("path"));
            JSONObject archive = request.getJSONObject("record").getJSONObject("storage").getJSONObject("archive");
            try (InputStream input = new FileInputStream(source)) {
                return new JSONObject().put("matches", source.length() == archive.getLong("size") && digest(input).equals(archive.getString("sha256")));
            }
        }
        if (action.equals("publishExtracted")) return publishExtracted(request);
        if (action.equals("status")) {
            JSONObject record = request.getJSONObject("record");
            try { return request.optBoolean("metadataOnly") ? metadataStatus(record) : inspect(record, false); }
            catch (LocationUnavailable failure) { return new JSONObject().put("path", record.getString("path")).put("status", "Location unavailable").put("error", failure.getMessage()); }
            catch (java.io.FileNotFoundException failure) { return new JSONObject().put("path", record.getString("path")).put("status", "Missing").put("error", failure.getMessage()); }
            catch (SecurityException failure) { return new JSONObject().put("path", record.getString("path")).put("status", "Location unavailable").put("error", failure.getMessage()); }
            catch (IllegalStateException failure) { return new JSONObject().put("path", record.getString("path")).put("status", "Changed").put("error", failure.getMessage()); }
        }
        if (action.equals("deleteCopy")) return inspect(request.getJSONObject("record"), true);
        if (action.equals("deleteExtraction")) {
            JSONObject record = new JSONObject(request.getJSONObject("record").toString());
            record.getJSONObject("storage").remove("archive");
            return inspect(record, true);
        }
        if (action.equals("deleteArchive")) {
            JSONObject record = request.getJSONObject("record");
            JSONObject storage = record.getJSONObject("storage");
            Destination destination = new Destination(storage.getString("tree"));
            JSONObject current = inspect(record, false);
            if (!current.has("extracted")) throw new IllegalStateException("Verified published extraction is missing; archive was kept");
            JSONObject archive = storage.getJSONObject("archive");
            verify(destination, archive, destination.find(archive.getString("name")));
            destination.delete(Uri.parse(archive.getString("uri")));
            storage.remove("archive");
            return record;
        }
        String chosen = request.optString("tree", tree());
        Destination destination = new Destination(chosen.isEmpty() ? defaultLocation() : chosen);
        if (action.equals("discover")) {
            String name = request.getString("name"); safeName(name);
            JSONObject file = destination.findArchive(name);
            if (file == null || file.getBoolean("directory") || !name.equals(file.getString("name")) ||
                (destination.media && !file.getString("relative").isEmpty()) || file.getLong("size") != request.getLong("size"))
                return new JSONObject().put("found", false);
            Uri uri = Uri.parse(file.getString("uri"));
            if (request.optBoolean("metadataOnly"))
                return new JSONObject().put("found", true).put("path", destination.local ? uri.getPath() : uri.toString()).put("status", "Unrecorded");
            String sha1 = request.optString("sha1"), md5 = request.optString("md5");
            if (sha1.isEmpty() && md5.isEmpty()) return new JSONObject().put("found", false);
            try (InputStream input = destination.read(uri)) {
                if (!digest(input, sha1.isEmpty() ? "MD5" : "SHA-1").equalsIgnoreCase(sha1.isEmpty() ? md5 : sha1))
                    return new JSONObject().put("found", false);
            }
            if (!sha1.isEmpty() && !md5.isEmpty()) {
                try (InputStream input = destination.read(uri)) {
                    if (!digest(input, "MD5").equalsIgnoreCase(md5)) return new JSONObject().put("found", false);
                }
            }
            String path = destination.local ? uri.getPath() : uri.toString();
            JSONObject result = new JSONObject().put("found", true).put("path", path).put("status", "Unrecorded");
            // MediaStore supplies ownership for old releases; SAF has no equivalent, so those copies stay unowned.
            if (destination.media && file.getBoolean("owned")) {
                String hash;
                try (InputStream input = destination.read(uri)) { hash = digest(input); }
                JSONObject archive = new JSONObject().put("uri", uri.toString()).put("name", name).put("size", file.getLong("size")).put("sha256", hash);
                result.put("status", "Downloaded").put("record", new JSONObject().put("storage",
                    new JSONObject().put("tree", destination.value).put("archive", archive)).put("path", path).put("modified", 0));
            }
            return result;
        }
        if (action.equals("list")) {
            JSONArray files = new JSONArray();
            for (JSONObject file : destination.children(destination.root, "")) if (!file.getBoolean("directory") &&
                (!destination.media || (file.getString("relative").isEmpty() && file.getBoolean("owned")))) files.put(file);
            return new JSONObject().put("files", files);
        }
        if (action.equals("exists")) {
            String name = request.getString("name"), extracted = request.optString("folder");
            return new JSONObject().put("exists", destination.find(name) != null || (!extracted.isEmpty() && destination.find(extracted) != null));
        }
        if (action.equals("validate") || action.equals("select")) {
            if (action.equals("select") && !preferences.edit().putString("tree", destination.value).commit())
                throw new IllegalStateException("Cannot remember selected download folder");
            return new JSONObject().put("ok", true);
        }
        throw new IllegalArgumentException("Unknown storage operation");
    }
}
