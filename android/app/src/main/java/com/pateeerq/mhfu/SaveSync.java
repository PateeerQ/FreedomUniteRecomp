package com.pateeerq.mhfu;

import android.content.ContentResolver;
import android.content.Context;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.HashMap;
import java.util.Map;

/**
 * The saves in a folder the player can see (the way PPSSPP keeps its memory
 * stick in a folder of the player's choosing), as well as in the app's own.
 *
 * The game reads and writes its memory stick by path, in the app's folder
 * (root/ms, ms0:). A folder chosen through Android's picker has no path, only
 * documents, so the two are kept the same instead:
 *   at start, file by file, the newer side wins (so the saves come back after
 *   Clear data or a reinstall, once the folder is chosen again);
 *   while playing, what the game wrote goes to the folder every few seconds,
 *   and when the app leaves the foreground.
 * Nothing is ever deleted in the chosen folder: a save the game removes stays
 * there (a backup, never a loss).
 *
 * The folder can be any of a memory stick's levels, told by what is in it: a
 * memory stick (it has PSP/, or is empty: PSP/SAVEDATA/... is made there), a
 * SAVEDATA folder (it has ULES01213... / ULUS10391... folders: only those are used), or one
 * save itself (PARAM.SFO, MHP2NDG.BIN: only its files). The disc image, the
 * EBOOT and fonts kept in the same folder are never copied.
 */
final class SaveSync {
    private static final String TAG = "mhfu";
    static final String PREF_TREE = "saves_tree";        // in the "setup" prefs
    static final String PREF_ASKED = "saves_asked";

    private SaveSync() { }

    static SharedPreferences prefs(Context c) { return c.getSharedPreferences("setup", Context.MODE_PRIVATE); }

    /** The app's memory stick: root/ms. */
    static File local(Context c) { return new File(c.getExternalFilesDir(null), "root/ms"); }

    private static final int ALL = 0, DIRS = 1, FILES = 2;   // what of the folder's top level is used

    /** Where the chosen folder sits on the memory stick: the app's folder that matches it, and what of it counts. */
    private static final class Where {
        final File dir;
        final int only;
        Where(File dir, int only) { this.dir = dir; this.only = only; }
    }

    private static Where where(Context c, ContentResolver cr, Uri t) {
        Map<String, String[]> top = children(cr, t, DocumentsContract.getTreeDocumentId(t));
        File ms = local(c);
        for (Map.Entry<String, String[]> e : top.entrySet())
            if (Document.MIME_TYPE_DIR.equals(e.getValue()[1]) && e.getKey().equalsIgnoreCase("PSP")) return new Where(ms, ALL);
        for (Map.Entry<String, String[]> e : top.entrySet())
            if (Document.MIME_TYPE_DIR.equals(e.getValue()[1]) && e.getKey().toUpperCase().startsWith(Region.now().gameId))
                return new Where(new File(ms, "PSP/SAVEDATA"), DIRS);
        for (String k : top.keySet())
            if (k.equalsIgnoreCase("PARAM.SFO") || k.equalsIgnoreCase("MHP2NDG.BIN"))
                return new Where(new File(ms, "PSP/SAVEDATA/" + Region.now().gameId), FILES);
        return new Where(ms, ALL);
    }

    /** Never a save: the disc, the EBOOT, fonts, logs that may sit in the same folder. */
    private static boolean junk(String name) {
        String n = name.toLowerCase();
        return n.endsWith(".iso") || n.endsWith(".cso") || n.endsWith(".pgf") || n.endsWith(".txt") || n.endsWith(".log")
                || n.endsWith(".part") || n.contains("eboot");
    }

    private static boolean wanted(String name, boolean isDir, int only) {
        if (junk(name)) return false;
        return only == ALL || (only == DIRS) == isDir;
    }

    /** What an earlier version copied to the wrong level (anything in root/ms but PSP/) goes: the game never
     *  writes there, and it is all still in the chosen folder. */
    private static void tidy(Context c) {
        File[] list = local(c).listFiles();
        if (list == null) return;
        for (File f : list) if (!f.getName().equals("PSP")) remove(f);
    }

    private static void remove(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) remove(k);
        f.delete();
    }

    static Uri tree(Context c) {
        String t = prefs(c).getString(PREF_TREE, null);
        return t == null ? null : Uri.parse(t);
    }

    /** The chosen folder as the player knows it ("Documents/MHFU"), for showing. */
    static String describe(Context c) {
        Uri t = tree(c);
        if (t == null) return null;
        String id = DocumentsContract.getTreeDocumentId(t);
        int colon = id.indexOf(':');
        String path = colon >= 0 ? id.substring(colon + 1) : id;
        return path.isEmpty() ? "the storage's top folder" : path;
    }

    /** A new folder: kept, with read and write access that lasts. */
    static void choose(Context c, Uri tree) {
        ContentResolver cr = c.getContentResolver();
        cr.takePersistableUriPermission(tree, android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION
                | android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        String old = prefs(c).getString(PREF_TREE, null);
        if (old != null && !old.equals(tree.toString())) {
            try {
                cr.releasePersistableUriPermission(Uri.parse(old), android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION
                        | android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            } catch (Exception ignored) { }
        }
        prefs(c).edit().putString(PREF_TREE, tree.toString()).putBoolean(PREF_ASKED, true).apply();
        synchronized (pushed) { pushed.clear(); }
    }

    // ---- both ways, at start ---------------------------------------------------------------

    /** Newer wins, file by file; returns how many files were copied, or -1 if the folder is gone. */
    static int syncBoth(Context c) {
        Uri t = tree(c);
        if (t == null) return 0;
        try {
            ContentResolver cr = c.getContentResolver();
            tidy(c);
            Where w = where(c, cr, t);
            w.dir.mkdirs();
            int n = sync(cr, t, DocumentsContract.getTreeDocumentId(t), w.dir, w.only);
            snapshot(local(c), "");                       // what is there now counts as pushed
            return n;
        } catch (Exception e) {
            Log.w(TAG, "saves: sync with the folder failed: " + e);
            return -1;
        }
    }

    private static int sync(ContentResolver cr, Uri t, String dirDoc, File dir, int only) throws Exception {
        int n = 0;
        Map<String, String[]> remote = children(cr, t, dirDoc);   // name -> {docId, mime, lastModified}
        remote.entrySet().removeIf(e -> !wanted(e.getKey(), Document.MIME_TYPE_DIR.equals(e.getValue()[1]), only));
        File[] mine = dir.listFiles();
        if (mine != null) {
            for (File f : mine) {
                if (!wanted(f.getName(), f.isDirectory(), only)) continue;
                String[] r = remote.remove(f.getName());
                if (f.isDirectory()) {
                    String sub = r != null ? r[0] : create(cr, t, dirDoc, Document.MIME_TYPE_DIR, f.getName());
                    if (sub != null) n += sync(cr, t, sub, f, ALL);
                } else if (r == null) {
                    String doc = create(cr, t, dirDoc, "application/octet-stream", f.getName());
                    if (doc != null) { write(cr, DocumentsContract.buildDocumentUriUsingTree(t, doc), f); n++; }
                } else if (!Document.MIME_TYPE_DIR.equals(r[1])) {
                    long theirs = Long.parseLong(r[2]);
                    if (theirs > f.lastModified() + 2000) { read(cr, DocumentsContract.buildDocumentUriUsingTree(t, r[0]), f, theirs); n++; }
                    else if (f.lastModified() > theirs + 2000) { write(cr, DocumentsContract.buildDocumentUriUsingTree(t, r[0]), f); n++; }
                }
            }
        }
        // Only in the folder: into the app's.
        for (Map.Entry<String, String[]> e : remote.entrySet()) {
            String[] r = e.getValue();
            File f = new File(dir, e.getKey());
            if (Document.MIME_TYPE_DIR.equals(r[1])) {
                f.mkdirs();
                n += sync(cr, t, r[0], f, ALL);
            } else {
                read(cr, DocumentsContract.buildDocumentUriUsingTree(t, r[0]), f, Long.parseLong(r[2]));
                n++;
            }
        }
        return n;
    }

    // ---- the folder's, over the app's (Load from folder) ----------------------------------------

    /** Every file in the folder into the app, whatever their times; how many, or -1 if it is gone. */
    static int pullAll(Context c) {
        Uri t = tree(c);
        if (t == null) return -1;
        try {
            ContentResolver cr = c.getContentResolver();
            tidy(c);
            Where w = where(c, cr, t);
            w.dir.mkdirs();
            int n = pullAll(cr, t, DocumentsContract.getTreeDocumentId(t), w.dir, w.only);
            synchronized (pushed) { pushed.clear(); }
            snapshot(local(c), "");                       // now the same on both sides
            return n;
        } catch (Exception e) {
            Log.w(TAG, "saves: loading from the folder failed: " + e);
            return -1;
        }
    }

    private static int pullAll(ContentResolver cr, Uri t, String dirDoc, File dir, int only) throws Exception {
        int n = 0;
        for (Map.Entry<String, String[]> e : children(cr, t, dirDoc).entrySet()) {
            String[] r = e.getValue();
            if (!wanted(e.getKey(), Document.MIME_TYPE_DIR.equals(r[1]), only)) continue;
            File f = new File(dir, e.getKey());
            if (Document.MIME_TYPE_DIR.equals(r[1])) {
                f.mkdirs();
                n += pullAll(cr, t, r[0], f, ALL);
            } else {
                read(cr, DocumentsContract.buildDocumentUriUsingTree(t, r[0]), f, Long.parseLong(r[2]));
                n++;
            }
        }
        return n;
    }

    // ---- this way, while playing ---------------------------------------------------------------

    /** Each file as last pushed: "size:mtime", by its path under root/ms. */
    private static final Map<String, String> pushed = new HashMap<>();

    private static void snapshot(File dir, String rel) {
        File[] list = dir.listFiles();
        if (list == null) return;
        for (File f : list) {
            String r = rel + "/" + f.getName();
            if (f.isDirectory()) snapshot(f, r);
            else synchronized (pushed) { pushed.put(r, f.length() + ":" + f.lastModified()); }
        }
    }

    /** What the game has written since the last push, to the folder; how many files. */
    static int push(Context c) {
        Uri t = tree(c);
        if (t == null) return 0;
        try {
            ContentResolver cr = c.getContentResolver();
            Where w = where(c, cr, t);
            if (!w.dir.isDirectory()) return 0;
            String rel = w.dir.getAbsolutePath().substring(local(c).getAbsolutePath().length()).replace('\\', '/');
            return push(cr, t, DocumentsContract.getTreeDocumentId(t), w.dir, rel, w.only);
        } catch (Exception e) {
            Log.w(TAG, "saves: copying to the folder failed: " + e);
            return 0;
        }
    }

    private static int push(ContentResolver cr, Uri t, String dirDoc, File dir, String rel, int only) throws Exception {
        int n = 0;
        File[] list = dir.listFiles();
        if (list == null) return 0;
        Map<String, String[]> remote = null;                   // only asked for when something changed
        for (File f : list) {
            if (!wanted(f.getName(), f.isDirectory(), only)) continue;
            String r = rel + "/" + f.getName();
            if (f.isDirectory()) {
                if (!changedUnder(f, r)) continue;
                if (remote == null) remote = children(cr, t, dirDoc);
                String[] e = remote.get(f.getName());
                String sub = e != null ? e[0] : create(cr, t, dirDoc, Document.MIME_TYPE_DIR, f.getName());
                if (sub != null) n += push(cr, t, sub, f, r, ALL);
                continue;
            }
            String now = f.length() + ":" + f.lastModified();
            synchronized (pushed) { if (now.equals(pushed.get(r))) continue; }
            if (System.currentTimeMillis() - f.lastModified() < 1500) continue;   // still being written
            if (remote == null) remote = children(cr, t, dirDoc);
            String[] e = remote.get(f.getName());
            String doc = e != null ? e[0] : create(cr, t, dirDoc, "application/octet-stream", f.getName());
            if (doc == null) continue;
            write(cr, DocumentsContract.buildDocumentUriUsingTree(t, doc), f);
            synchronized (pushed) { pushed.put(r, now); }
            n++;
        }
        return n;
    }

    private static boolean changedUnder(File dir, String rel) {
        File[] list = dir.listFiles();
        if (list == null) return false;
        for (File f : list) {
            String r = rel + "/" + f.getName();
            if (f.isDirectory()) { if (changedUnder(f, r)) return true; continue; }
            synchronized (pushed) { if (!(f.length() + ":" + f.lastModified()).equals(pushed.get(r))) return true; }
        }
        return false;
    }

    // ---- documents -------------------------------------------------------------------------------

    private static Map<String, String[]> children(ContentResolver cr, Uri t, String dirDoc) {
        Map<String, String[]> out = new HashMap<>();
        Uri kids = DocumentsContract.buildChildDocumentsUriUsingTree(t, dirDoc);
        try (Cursor c = cr.query(kids, new String[] { Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME,
                Document.COLUMN_MIME_TYPE, Document.COLUMN_LAST_MODIFIED }, null, null, null)) {
            while (c != null && c.moveToNext())
                if (c.getString(1) != null) out.put(c.getString(1), new String[] { c.getString(0), c.getString(2), String.valueOf(c.getLong(3)) });
        }
        return out;
    }

    private static String create(ContentResolver cr, Uri t, String dirDoc, String mime, String name) throws Exception {
        Uri made = DocumentsContract.createDocument(cr, DocumentsContract.buildDocumentUriUsingTree(t, dirDoc), mime, name);
        return made == null ? null : DocumentsContract.getDocumentId(made);
    }

    private static void write(ContentResolver cr, Uri doc, File from) throws Exception {
        try (InputStream in = new FileInputStream(from); OutputStream out = cr.openOutputStream(doc, "wt")) {
            byte[] buf = new byte[1 << 16];
            for (int k; (k = in.read(buf)) > 0;) out.write(buf, 0, k);
        }
    }

    /** Through a temporary file: an interrupted copy never leaves half a save. It keeps the
     *  folder's time, so that the next start does not copy it back the other way. */
    private static void read(ContentResolver cr, Uri doc, File to, long mtime) throws Exception {
        to.getParentFile().mkdirs();
        File tmp = new File(to.getParentFile(), to.getName() + ".part");
        try (InputStream in = cr.openInputStream(doc); OutputStream out = new FileOutputStream(tmp)) {
            byte[] buf = new byte[1 << 16];
            for (int k; (k = in.read(buf)) > 0;) out.write(buf, 0, k);
        }
        if (!tmp.renameTo(to)) { to.delete(); tmp.renameTo(to); }
        if (mtime > 0) to.setLastModified(mtime);
    }
}
