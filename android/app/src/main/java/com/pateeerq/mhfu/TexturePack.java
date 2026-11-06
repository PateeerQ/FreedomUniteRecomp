package com.pateeerq.mhfu;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * The drawer's "Install texture pack": a PPSSPP texture pack (a folder with a
 * textures.ini, often inside MAIN/) copied from a folder the player picks into
 * root/textures/<game id>, where host/texpack.c reads it at start. Copied, not
 * read where it is: the game reads files by path, and a picked folder has none.
 */
final class TexturePack {
    private TexturePack() { }

    /** Where the game looks: root/textures/<game id>. */
    static File dir(Activity a) {
        return new File(a.getExternalFilesDir(null), "root/textures/" + Region.get(a).gameId);
    }

    private static boolean installed(Activity a) {
        File d = dir(a);
        return new File(d, "textures.ini").isFile() || new File(d, "MAIN/textures.ini").isFile();
    }

    private static AlertDialog.Builder dialog(Activity a, String title) {
        return new AlertDialog.Builder(a, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle(title);
    }

    /** The first dialog: what is there, and Choose folder / Remove. */
    static void offer(Activity a, int requestCode) {
        a.runOnUiThread(() -> {
            boolean have = installed(a);
            dialog(a, "Texture pack")
                    .setMessage((have ? "A texture pack is installed.\n\n" : "No texture pack is installed: the game's own textures show.\n\n")
                            + "Choose the folder of a texture pack made for PPSSPP (the one with textures.ini, or "
                            + "the folder above its MAIN). It is copied into the app, which can take a while for a "
                            + "big pack, and used from the next start. \"Texture pack\" in the menu turns it off.")
                    .setPositiveButton(have ? "Replace" : "Choose folder", (d, w) -> a.startActivityForResult(
                            new android.content.Intent(android.content.Intent.ACTION_OPEN_DOCUMENT_TREE), requestCode))
                    .setNeutralButton(have ? "Remove" : null, have ? (d, w) -> {
                        deleteTree(dir(a));
                        toast(a, "Texture pack removed; the game's own textures from the next start.");
                    } : null)
                    .setNegativeButton("Close", null)
                    .show();
        });
    }

    /** The picked folder: find the pack in it and copy it in, with a progress dialog. */
    static void install(Activity a, Uri tree) {
        AlertDialog wait = dialog(a, "Texture pack").setMessage("Looking for textures.ini...").setCancelable(false).show();
        new Thread(() -> {
            String result;
            try {
                result = copyPack(a, tree, wait);
            } catch (Exception e) {
                result = "Could not copy the pack: " + e.getMessage();
            }
            final String r = result;
            a.runOnUiThread(() -> {
                wait.dismiss();
                dialog(a, "Texture pack").setMessage(r).setPositiveButton("OK", null).show();
            });
        }).start();
    }

    // ---- the copy ---------------------------------------------------------------------

    private static final class Doc {
        final String id, name;
        final boolean folder;
        final long size;
        Doc(String id, String name, boolean folder, long size) { this.id = id; this.name = name; this.folder = folder; this.size = size; }
    }

    private static List<Doc> children(ContentResolver cr, Uri tree, String parentId) {
        List<Doc> out = new ArrayList<>();
        Uri u = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        String[] cols = { DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE };
        try (Cursor c = cr.query(u, cols, null, null, null)) {
            while (c != null && c.moveToNext())
                out.add(new Doc(c.getString(0), c.getString(1),
                        DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2)), c.isNull(3) ? 0 : c.getLong(3)));
        }
        return out;
    }

    private static boolean hasIni(List<Doc> kids) {
        for (Doc d : kids) if (!d.folder && d.name.equalsIgnoreCase("textures.ini")) return true;
        return false;
    }

    /** The folder to copy: the one with textures.ini, or its parent when that is MAIN (packs keep ADDONS beside it). */
    private static String findPack(ContentResolver cr, Uri tree, String id, String name, String parent, int depth) {
        List<Doc> kids = children(cr, tree, id);
        if (hasIni(kids)) return name.equalsIgnoreCase("MAIN") && parent != null ? parent : id;
        if (depth >= 3) return null;
        for (Doc d : kids)
            if (d.folder) {
                String f = findPack(cr, tree, d.id, d.name, id, depth + 1);
                if (f != null) return f;
            }
        return null;
    }

    private static String copyPack(Activity a, Uri tree, AlertDialog wait) throws Exception {
        ContentResolver cr = a.getContentResolver();
        String top = DocumentsContract.getTreeDocumentId(tree);
        String pack = findPack(cr, tree, top, "", null, 0);
        if (pack == null) return "No textures.ini in that folder (looked three levels down). Choose the texture pack's own folder.";

        // First the list, for the progress and the space check.
        List<String[]> files = new ArrayList<>();       // { document id, relative path }
        long[] total = { 0 };
        list(cr, tree, pack, "", files, total);
        File dest = dir(a), tmp = new File(dest.getPath() + ".new");
        long free = a.getExternalFilesDir(null).getUsableSpace();
        if (total[0] > free - (64L << 20))
            return String.format("Not enough space: the pack is %d MB, %d MB free.", total[0] >> 20, free >> 20);

        deleteTree(tmp);
        byte[] buf = new byte[1 << 20];
        long done = 0, shown = 0;
        int n = 0;
        for (String[] f : files) {
            File out = new File(tmp, f[1]);
            out.getParentFile().mkdirs();
            try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, f[0]));
                 OutputStream o = new FileOutputStream(out)) {
                int k;
                while ((k = in.read(buf)) > 0) { o.write(buf, 0, k); done += k; }
            }
            n++;
            if (done - shown > (8L << 20) || n == files.size()) {
                shown = done;
                final String msg = String.format("Copying the texture pack...\n%d of %d files, %d of %d MB",
                        n, files.size(), done >> 20, total[0] >> 20);
                a.runOnUiThread(() -> wait.setMessage(msg));
            }
        }
        deleteTree(dest);
        if (!tmp.renameTo(dest)) return "Copied, but could not move it into place.";
        return String.format("Texture pack installed: %d files, %d MB. It is used from the next start "
                + "(Exit game and open it again).", files.size(), total[0] >> 20);
    }

    private static void list(ContentResolver cr, Uri tree, String id, String rel, List<String[]> out, long[] total) {
        for (Doc d : children(cr, tree, id)) {
            String path = rel.isEmpty() ? d.name : rel + "/" + d.name;
            if (d.folder) list(cr, tree, d.id, path, out, total);
            else { out.add(new String[] { d.id, path }); total[0] += d.size; }
        }
    }

    static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }

    private static void toast(Activity a, String s) { Toast.makeText(a, s, Toast.LENGTH_LONG).show(); }
}
