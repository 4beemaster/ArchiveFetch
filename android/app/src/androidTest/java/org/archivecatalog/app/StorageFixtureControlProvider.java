package org.archivecatalog.app;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.Process;

public final class StorageFixtureControlProvider extends ContentProvider {
    @Override public boolean onCreate() { return true; }
    @Override public Bundle call(String method, String argument, Bundle extras) {
        int caller = Binder.getCallingUid();
        try {
            int target = getContext().getPackageManager().getApplicationInfo("org.archivecatalog.app", 0).uid;
            if (caller != Process.myUid() && (caller != target ||
                getContext().getPackageManager().checkSignatures(caller, Process.myUid()) != PackageManager.SIGNATURE_MATCH))
                throw new SecurityException("Only the signed instrumentation target may control storage fixtures");
        } catch (PackageManager.NameNotFoundException failure) {
            throw new IllegalStateException("Instrumentation target is not installed", failure);
        }
        switch (method) {
            case "fixtureCreate": case "fixtureWrite": case "fixtureFailures": case "fixtureRevoke": case "fixtureGrant": case "fixtureRemove":
                break;
            default: throw new IllegalArgumentException("Unknown fixture control operation");
        }
        long identity = Binder.clearCallingIdentity();
        try {
            Uri documents = Uri.parse("content://" + getContext().getPackageName() + ".storage-fixture");
            Bundle result = getContext().getContentResolver().call(documents, method, argument, extras);
            if (result == null) throw new IllegalStateException("Fixture owner returned no response");
            return result;
        } finally { Binder.restoreCallingIdentity(identity); }
    }
    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
        throw new UnsupportedOperationException("Fixture control supports calls only");
    }
    @Override public String getType(Uri uri) {
        throw new UnsupportedOperationException("Fixture control supports calls only");
    }
    @Override public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("Fixture control supports calls only");
    }
    @Override public int delete(Uri uri, String selection, String[] args) {
        throw new UnsupportedOperationException("Fixture control supports calls only");
    }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) {
        throw new UnsupportedOperationException("Fixture control supports calls only");
    }
}
