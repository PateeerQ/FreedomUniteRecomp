package com.pateeerq.mhfu;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

/**
 * Hands the crash log's .zip (CrashLog.share) to the app the player shares it
 * with: files in cache/share only, read-only. A FileProvider of our own, as
 * the app has no AndroidX.
 */
public final class CrashProvider extends ContentProvider {
    static String authority(android.content.Context c) { return c.getPackageName() + ".crashlog"; }

    static File dir(android.content.Context c) { return new File(c.getCacheDir(), "share"); }

    private File fileOf(Uri uri) throws FileNotFoundException {
        String name = uri.getLastPathSegment();
        if (name == null || name.contains("/") || name.startsWith(".")) throw new FileNotFoundException();
        File f = new File(dir(getContext()), name);
        if (!f.isFile()) throw new FileNotFoundException(name);
        return f;
    }

    @Override public boolean onCreate() { return true; }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
        MatrixCursor c = new MatrixCursor(new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE });
        try {
            File f = fileOf(uri);
            c.addRow(new Object[] { f.getName(), f.length() });
        } catch (FileNotFoundException ignored) { }
        return c;
    }

    @Override public String getType(Uri uri) { return "application/zip"; }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        return ParcelFileDescriptor.open(fileOf(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override public Uri insert(Uri uri, ContentValues values) { return null; }
    @Override public int delete(Uri uri, String sel, String[] args) { return 0; }
    @Override public int update(Uri uri, ContentValues v, String sel, String[] args) { return 0; }
}
