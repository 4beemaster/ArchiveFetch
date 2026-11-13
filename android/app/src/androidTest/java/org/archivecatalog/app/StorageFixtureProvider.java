package org.archivecatalog.app;

import android.content.Intent;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsProvider;
import java.io.File;
import java.io.FileNotFoundException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.UUID;

public final class StorageFixtureProvider extends DocumentsProvider {
    private File base;
    private volatile boolean failRename, failDelete, failWrite, failRead, failChildren;
    @Override public boolean onCreate() {
        try { base = new File(getContext().getFilesDir(), "storage-fixtures").getCanonicalFile(); }
        catch (java.io.IOException failure) { throw new IllegalStateException("Cannot resolve fixture storage root", failure); }
        return base.isDirectory() || base.mkdirs();
    }
    private File file(String id) throws FileNotFoundException {
        try {
            File value = new File(base, id).getCanonicalFile();
            if (!value.getPath().startsWith(base.getCanonicalPath() + File.separator)) throw new FileNotFoundException("Outside fixture");
            return value;
        } catch (java.io.IOException failure) { throw new FileNotFoundException(failure.getMessage()); }
    }
    private String id(File file) {
        java.nio.file.Path relative = base.toPath().relativize(file.toPath());
        if (relative.isAbsolute() || relative.startsWith("..")) throw new IllegalArgumentException("Fixture document is outside its root");
        return relative.toString().replace(File.separatorChar, '/');
    }
    private static void safeName(String name) {
        if (name.isEmpty() || name.equals(".") || name.equals("..") || name.contains("/") || name.contains("\\"))
            throw new IllegalArgumentException("Invalid fixture filename");
    }
    private static String[] columns(String[] projection) {
        return projection == null ? new String[]{DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE, DocumentsContract.Document.COLUMN_FLAGS} : projection;
    }
    private void row(MatrixCursor cursor, File file) {
        MatrixCursor.RowBuilder row = cursor.newRow();
        for (String column : cursor.getColumnNames()) {
            if (DocumentsContract.Document.COLUMN_DOCUMENT_ID.equals(column)) row.add(id(file));
            else if (DocumentsContract.Document.COLUMN_DISPLAY_NAME.equals(column)) row.add(file.getName());
            else if (DocumentsContract.Document.COLUMN_MIME_TYPE.equals(column)) row.add(file.isDirectory() ? DocumentsContract.Document.MIME_TYPE_DIR : "application/octet-stream");
            else if (DocumentsContract.Document.COLUMN_SIZE.equals(column)) row.add(file.length());
            else if (DocumentsContract.Document.COLUMN_FLAGS.equals(column)) row.add(DocumentsContract.Document.FLAG_SUPPORTS_WRITE |
                DocumentsContract.Document.FLAG_SUPPORTS_DELETE | DocumentsContract.Document.FLAG_SUPPORTS_RENAME |
                (file.isDirectory() ? DocumentsContract.Document.FLAG_DIR_SUPPORTS_CREATE : 0));
            else row.add(null);
        }
    }
    @Override public Cursor queryRoots(String[] projection) { return new MatrixCursor(projection == null ? new String[]{DocumentsContract.Root.COLUMN_ROOT_ID} : projection); }
    @Override public Cursor queryDocument(String documentId, String[] projection) throws FileNotFoundException {
        File file = file(documentId);
        if (!file.exists()) throw new FileNotFoundException(documentId);
        MatrixCursor cursor = new MatrixCursor(columns(projection)); row(cursor, file); return cursor;
    }
    @Override public Cursor queryChildDocuments(String parentDocumentId, String[] projection, String sortOrder) throws FileNotFoundException {
        if (failChildren) throw new FileNotFoundException("Injected fixture directory enumeration failure");
        File[] children = file(parentDocumentId).listFiles();
        if (children == null) throw new FileNotFoundException(parentDocumentId);
        MatrixCursor cursor = new MatrixCursor(columns(projection));
        for (File child : children) row(cursor, child);
        return cursor;
    }
    @Override public ParcelFileDescriptor openDocument(String documentId, String mode, android.os.CancellationSignal signal) throws FileNotFoundException {
        if (failRead && mode.contains("r")) throw new FileNotFoundException("Injected fixture read failure");
        if (failWrite && mode.contains("w")) throw new FileNotFoundException("Injected fixture write failure");
        return ParcelFileDescriptor.open(file(documentId), ParcelFileDescriptor.parseMode(mode));
    }
    @Override public String createDocument(String parentDocumentId, String mimeType, String displayName) throws FileNotFoundException {
        safeName(displayName);
        File created = new File(file(parentDocumentId), displayName);
        try {
            boolean success = DocumentsContract.Document.MIME_TYPE_DIR.equals(mimeType) ? created.mkdir() : created.createNewFile();
            if (!success) throw new FileNotFoundException("Fixture destination exists");
        } catch (java.io.IOException failure) { throw new FileNotFoundException(failure.getMessage()); }
        return id(created);
    }
    @Override public String renameDocument(String documentId, String displayName) throws FileNotFoundException {
        if (failRename) throw new FileNotFoundException("Injected fixture rename failure");
        safeName(displayName);
        File old = file(documentId), next = new File(old.getParentFile(), displayName);
        if (next.exists() || !old.renameTo(next)) throw new FileNotFoundException("Fixture rename refused");
        return id(next);
    }
    @Override public void deleteDocument(String documentId) throws FileNotFoundException {
        if (failDelete) throw new FileNotFoundException("Injected fixture delete failure");
        if (!file(documentId).delete()) throw new FileNotFoundException("Fixture delete refused");
    }
    @Override public boolean isChildDocument(String parentDocumentId, String documentId) {
        try { return file(documentId).getPath().startsWith(file(parentDocumentId).getPath() + File.separator); }
        catch (FileNotFoundException failure) { throw new IllegalArgumentException("Invalid fixture document", failure); }
    }
    @Override public Bundle call(String method, String argument, Bundle extras) {
        if (method.startsWith("android:")) return super.call(method, argument, extras);
        if (android.os.Binder.getCallingUid() != android.os.Process.myUid())
            throw new SecurityException("Fixture controls must run in the provider owner process");
        Bundle result = new Bundle();
        try {
            if ("fixtureCreate".equals(method)) {
                String name = UUID.randomUUID().toString();
                if (!new File(base, name).mkdir()) throw new java.io.IOException("Cannot create fixture");
                Uri tree = DocumentsContract.buildTreeDocumentUri(getContext().getPackageName() + ".storage-fixture", name);
                getContext().grantUriPermission("org.archivecatalog.app", tree, Intent.FLAG_GRANT_READ_URI_PERMISSION |
                    Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
                result.putString("tree", tree.toString()); result.putString("id", name);
            } else if ("fixtureWrite".equals(method)) {
                File target = file(argument);
                Files.write(target.toPath(), extras.getString("text").getBytes(StandardCharsets.UTF_8));
            } else if ("fixtureFailures".equals(method)) {
                failRename = extras.getBoolean("rename"); failDelete = extras.getBoolean("delete"); failWrite = extras.getBoolean("write");
                failRead = extras.getBoolean("read"); failChildren = extras.getBoolean("children");
            } else if ("fixtureRevoke".equals(method)) {
                getContext().revokeUriPermission(Uri.parse(argument), Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            } else if ("fixtureRemove".equals(method)) {
                Uri tree = Uri.parse(argument);
                if (!file(DocumentsContract.getTreeDocumentId(tree)).delete()) throw new java.io.IOException("Fixture folder is not empty or cannot be removed");
                getContext().revokeUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            } else if ("fixtureGrant".equals(method)) {
                Uri tree = Uri.parse(argument);
                if (!file(DocumentsContract.getTreeDocumentId(tree)).isDirectory()) throw new FileNotFoundException("Fixture folder is missing");
                getContext().grantUriPermission("org.archivecatalog.app", tree, Intent.FLAG_GRANT_READ_URI_PERMISSION |
                    Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
            } else throw new IllegalArgumentException("Unknown fixture operation");
        } catch (java.io.IOException failure) { result.putString("error", failure.getMessage()); }
        return result;
    }
}
