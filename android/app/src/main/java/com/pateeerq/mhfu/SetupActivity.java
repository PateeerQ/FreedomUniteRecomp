package com.pateeerq.mhfu;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.UriPermission;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.view.View;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;

/**
 * The app's entry: no screen of its own. If the game's files are there it
 * starts the game at once; if not -- no disc yet, or the disc is not a valid
 * PSP dump, or it was moved -- it asks for the folder that holds the .iso
 * (Android's file manager, "Use this folder"), keeps the permission to it, and
 * hands the game an open file, /proc/self/fd/N, in iso.txt.
 *
 * The same folder may hold the decrypted EBOOT (ULES01213_EBOOT.BIN, or ULUS10391_ in the USA build) and the
 * PSP's fonts (.pgf, there or in a font/ folder below it); those are copied
 * into the app's folder when it has none yet. Files put there by hand (adb
 * push) count too. The fonts usually come from PPSSPP itself: on Android they
 * are inside its APK (assets/flash0/font), which any app may read, and the
 * player has PPSSPP anyway, for dumping the EBOOT.
 *
 * The game's "Change game files" (MhfuActivity) sets ASK and restarts here:
 * then the folder is asked for straight away, and what it holds replaces the
 * EBOOT and fonts the app has (the saves in root/ms are never touched).
 */
public class SetupActivity extends Activity {
    private static final int PICK_FOLDER = 1, PICK_SAVES = 2;
    static final String ASK = "ask";             // in the "setup" prefs: ask for the folder at the next start
    private static final int WOOD = Color.rgb(26, 20, 15);

    private File base;
    private SharedPreferences prefs;
    private static ParcelFileDescriptor isoFd;     // kept open for the game, which reads it by path
    private boolean replace;                       // a new folder's EBOOT and fonts replace the app's

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        base = getExternalFilesDir(null);
        prefs = getSharedPreferences("setup", MODE_PRIVATE);
        View bg = new View(this);
        bg.setBackgroundColor(WOOD);
        setContentView(bg);
        if (Updater.installStatus(this, getIntent())) { finish(); return; }
        if (prefs.getBoolean(ASK, false)) {
            prefs.edit().remove(ASK).apply();
            replace = true;
            askFolder("Choose the folder with your game files: the .iso, and the decrypted EBOOT and the PSP fonts "
                    + "if they changed. Your saves stay as they are.");
            return;
        }
        go();
    }

    // ---- what is there -----------------------------------------------------------------

    private File eboot() { return new File(base, BuildConfig.GAME_ID + "_EBOOT.BIN"); }
    private File fonts() { return new File(base, "root/flash0/font"); }

    private boolean haveEboot() { return eboot().length() > 100000 && elf(eboot()); }
    private boolean haveFonts() {
        String[] f = fonts().list();
        if (f == null) return false;
        for (String n : f) if (n.toLowerCase().endsWith(".pgf")) return true;
        return false;
    }

    /** A PSP disc: ISO 9660 ("CD001") with the system id "PSP GAME". */
    private static boolean pspDisc(FileChannel ch) {
        try {
            ByteBuffer b = ByteBuffer.allocate(16);
            if (ch.read(b, 0x8000) < 16) return false;
            String s = new String(b.array(), "US-ASCII");
            return s.startsWith("CD001", 1) && s.startsWith("PSP GAME", 8);
        } catch (Exception e) {
            return false;
        }
    }

    private static boolean elf(File f) {
        try (InputStream in = new FileInputStream(f)) {
            byte[] h = new byte[4];
            return in.read(h) == 4 && h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F';
        } catch (Exception e) {
            return false;
        }
    }

    private String openDisc() { return openDisc(this); }

    /**
     * The disc to play, written to iso.txt; null if fine, else what is wrong.
     * MhfuActivity calls it too: when Android stops the app in the background
     * and later brings the game's screen back by itself, in a new process, the
     * descriptor in iso.txt was the old process's.
     */
    static String openDisc(android.content.Context ctx) {
        File base = ctx.getExternalFilesDir(null);
        SharedPreferences prefs = ctx.getSharedPreferences("setup", MODE_PRIVATE);
        File txt = new File(base, "iso.txt");
        txt.delete();
        String path = null;
        // A .iso pushed into the app's folder.
        File[] files = base.listFiles();
        for (int i = 0; files != null && i < files.length && path == null; i++) {
            File f = files[i];
            if (!f.getName().toLowerCase().endsWith(".iso")) continue;
            try (FileInputStream in = new FileInputStream(f)) {
                if (pspDisc(in.getChannel())) path = f.getAbsolutePath();
            } catch (Exception ignored) { }
        }
        String problem = null;
        if (path == null) {
            String uri = prefs.getString("iso", null);
            if (uri == null) return "Choose the folder that has your Monster Hunter Freedom Unite disc image (.iso).";
            try {
                if (isoFd != null) isoFd.close();
                isoFd = ctx.getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
                if (pspDisc(new FileInputStream(isoFd.getFileDescriptor()).getChannel()))
                    path = "/proc/self/fd/" + isoFd.getFd();
                else
                    problem = prefs.getString("iso_name", "The disc image") + " is not a valid PSP dump. Choose the folder with a good .iso.";
            } catch (Exception e) {
                problem = prefs.getString("iso_name", "The disc image") + " could not be opened (moved or deleted?). Choose its folder again.";
            }
            if (path == null) { prefs.edit().remove("iso").apply(); return problem; }
        }
        try (OutputStream o = new FileOutputStream(txt)) {
            o.write(path.getBytes("UTF-8"));
        } catch (Exception e) {
            return "Could not write iso.txt: " + e.getMessage();
        }
        return null;
    }

    // ---- going to the game ------------------------------------------------------------------

    /** PPSSPP's packages, whose APKs carry the PSP's fonts. */
    private static final String[] PPSSPP = { "org.ppsspp.ppsspp", "org.ppsspp.ppssppgold" };

    /** The .pgf fonts out of an installed PPSSPP's APK into root/flash0/font; how many. */
    private int fontsFromPpsspp() {
        for (String pkg : PPSSPP) {
            String apk;
            try {
                apk = getPackageManager().getApplicationInfo(pkg, 0).sourceDir;
            } catch (Exception e) {
                continue;                              // not installed
            }
            int n = 0;
            try (java.util.zip.ZipFile z = new java.util.zip.ZipFile(apk)) {
                java.util.Enumeration<? extends java.util.zip.ZipEntry> en = z.entries();
                while (en.hasMoreElements()) {
                    java.util.zip.ZipEntry e = en.nextElement();
                    String name = e.getName();
                    if (!name.startsWith("assets/flash0/font/") || !name.toLowerCase().endsWith(".pgf")) continue;
                    File to = new File(fonts(), name.substring(name.lastIndexOf('/') + 1));
                    to.getParentFile().mkdirs();
                    try (InputStream in = z.getInputStream(e); OutputStream out = new FileOutputStream(to)) {
                        byte[] buf = new byte[1 << 16];
                        for (int k; (k = in.read(buf)) > 0;) out.write(buf, 0, k);
                    }
                    n++;
                }
            } catch (Exception e) {
                android.util.Log.w("mhfu", "fonts from " + pkg + ": " + e);
            }
            if (n > 0) return n;
        }
        return 0;
    }

    private void go() {
        if (!haveFonts()) fontsFromPpsspp();
        String problem = openDisc();
        if (problem == null && !haveEboot())
            problem = "The decrypted EBOOT is missing: put " + BuildConfig.GAME_ID + "_EBOOT.BIN (PPSSPP: Tools, Developer tools, "
                    + "\"Dump decrypted EBOOT.BIN\", then start Monster Hunter Freedom Unite once) in the game's "
                    + "folder and choose it. Only the " + BuildConfig.REGION_NAME + " version's (" + BuildConfig.GAME_ID
                    + ") works in this app; other EBOOTs are ignored.";
        else if (problem == null && !haveFonts())
            problem = "The PSP fonts are missing. Install PPSSPP (from the Play Store): this app takes the fonts "
                    + "from it by itself. Or put the .pgf files (PPSSPP's assets/flash0/font) in the game's folder "
                    + "and choose it.";
        if (problem == null) {
            savesThen(this::crashThenStart);
            return;
        }
        askFolder(problem);
    }

    /** The saves' folder: asked for once; synced both ways at every start. Then `next`. */
    private void savesThen(Runnable next) {
        if (SaveSync.tree(this) == null) {
            if (prefs.getBoolean(SaveSync.PREF_ASKED, false)) { next.run(); return; }
            prefs.edit().putBoolean(SaveSync.PREF_ASKED, true).apply();
            afterSaves = next;
            new AlertDialog.Builder(this)
                    .setTitle("Where to keep your saves")
                    .setMessage("Choose a folder you can see, for example Documents/MHFU (\"Use this folder\"), the "
                            + "way PPSSPP keeps its memory stick. Your saves are copied there as you play, so they stay "
                            + "after Clear data or a reinstall, and you can back them up.\n\nThey are also kept in the "
                            + "app either way. You can choose later in the menu (Save folder).")
                    .setCancelable(false)
                    .setPositiveButton("Choose folder", (d, w) ->
                            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_SAVES))
                    .setNegativeButton("Not now", (d, w) -> next.run())
                    .show();
            return;
        }
        syncSaves(next);
    }

    private Runnable afterSaves;

    private void syncSaves(Runnable next) {
        final AlertDialog wait = new AlertDialog.Builder(this).setMessage("Syncing your saves...").setCancelable(false).show();
        new Thread(() -> {
            int n = SaveSync.syncBoth(this);
            runOnUiThread(() -> {
                wait.dismiss();
                if (n < 0) {
                    new AlertDialog.Builder(this)
                            .setTitle("Save folder")
                            .setMessage("The save folder (" + SaveSync.describe(this) + ") could not be reached: it may "
                                    + "have been moved or deleted. The game uses the saves kept in the app; choose the "
                                    + "folder again from the menu (Save folder).")
                            .setPositiveButton("OK", (d, w) -> next.run())
                            .setCancelable(false)
                            .show();
                } else {
                    next.run();
                }
            });
        }).start();
    }

    /** The game crashed last time: once, the crash log, for a report. Then the game. */
    private void crashThenStart() {
        File crash = CrashLog.file(this);
        if (crash.isFile() && crash.lastModified() > prefs.getLong("crash_seen", 0)) {
            prefs.edit().putLong("crash_seen", crash.lastModified()).apply();
            CrashLog.offer(this, "The game closed because of an error",
                    "Sorry about that. Copy the crash log and send it to the developer (PateeerQ) with your "
                    + "report: what you were doing when it closed helps too.", this::startGame);
            return;
        }
        startGame();
    }

    private void startGame() {
        startActivity(new Intent(this, MhfuActivity.class));
        finish();
        overridePendingTransition(0, 0);
    }

    private void askFolder(String message) {
        new AlertDialog.Builder(this)
                .setTitle("Game files")
                .setMessage(message)
                .setCancelable(false)
                .setPositiveButton("Choose folder", (d, w) ->
                        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER))
                .setNegativeButton("Exit", (d, w) -> finish())
                .show();
    }

    @Override
    protected void onActivityResult(int what, int result, Intent data) {
        if (what == PICK_SAVES) {
            Runnable next = afterSaves != null ? afterSaves : this::crashThenStart;
            if (result == RESULT_OK && data != null && data.getData() != null) {
                try {
                    SaveSync.choose(this, data.getData());
                    syncSaves(next);
                    return;
                } catch (Exception e) {
                    android.widget.Toast.makeText(this, "That folder cannot be used: " + e.getMessage(), android.widget.Toast.LENGTH_LONG).show();
                }
            }
            next.run();
            return;
        }
        if (what != PICK_FOLDER) return;
        if (result != RESULT_OK || data == null || data.getData() == null) { go(); return; }
        final Uri tree = data.getData();
        final ContentResolver cr = getContentResolver();
        try {
            cr.takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) { }
        final AlertDialog wait = new AlertDialog.Builder(this)
                .setMessage("Reading the folder...")
                .setCancelable(false)
                .show();
        new Thread(() -> {
            String msg = scan(cr, tree);
            runOnUiThread(() -> {
                wait.dismiss();
                if (msg != null) {
                    new AlertDialog.Builder(this)
                            .setTitle("Game files")
                            .setMessage(msg)
                            .setCancelable(false)
                            .setPositiveButton("OK", (d, w) -> go())
                            .show();
                } else {
                    go();
                }
            });
        }).start();
    }

    // ---- reading the chosen folder ------------------------------------------------------------

    private Uri foundIso;
    private String foundIsoName;
    private final java.util.List<Uri> ebootCandidates = new java.util.ArrayList<>();
    private final java.util.List<String> ebootNames = new java.util.ArrayList<>();

    /** This build's region's decrypted EBOOT, as PPSSPP dumps it: its size, and its module's name. */
    private static final long EBOOT_SIZE = BuildConfig.EBOOT_SIZE;
    private static final byte[] MODULE_NAME = "MonsterHunterPortable2ndG".getBytes(java.nio.charset.StandardCharsets.US_ASCII);

    /** How well a copied candidate fits: -1 not MHFU's EBOOT at all, higher is better. */
    private static int ebootScore(File f, String name) {
        if (!elf(f) || f.length() < 100000 || f.length() > 64L << 20) return -1;
        byte[] all;
        try (InputStream in = new FileInputStream(f)) {
            all = new byte[(int) f.length()];
            int got = 0;
            for (int n; got < all.length && (n = in.read(all, got, all.length - got)) > 0;) got += n;
        } catch (Exception e) {
            return -1;
        }
        boolean mhfu = false;
        outer:
        for (int i = 0; i + MODULE_NAME.length <= all.length; i++) {
            for (int k = 0; k < MODULE_NAME.length; k++) if (all[i + k] != MODULE_NAME[k]) continue outer;
            mhfu = true;
            break;
        }
        if (!mhfu) return -1;                      // another game's EBOOT
        if (f.length() != EBOOT_SIZE) return -1;   // another region's (the recompiled code is one region's)
        return name.toLowerCase().contains(BuildConfig.GAME_ID.toLowerCase()) ? 1 : 0;
    }

    /** Of the candidates the walk found, the one that is this game's EBOOT, into the app's folder. */
    private void pickEboot(ContentResolver cr) {
        File best = null;
        int bestScore = -1;
        for (int i = 0; i < ebootCandidates.size(); i++) {
            File tmp = new File(base, "eboot" + i + ".tmp");
            try {
                copy(cr, ebootCandidates.get(i), tmp);
            } catch (Exception e) {
                tmp.delete();
                continue;
            }
            int score = ebootScore(tmp, ebootNames.get(i));
            if (score > bestScore) {
                if (best != null) best.delete();
                best = tmp;
                bestScore = score;
            } else {
                tmp.delete();
            }
        }
        // Through a temporary file: a bad one never replaces a good one.
        if (best != null) {
            eboot().delete();
            if (!best.renameTo(eboot())) best.delete();
        }
    }

    /** The .iso (checked), the EBOOT and the fonts in `tree`; null, or why not. */
    private String scan(ContentResolver cr, Uri tree) {
        foundIso = null;
        foundIsoName = null;
        ebootCandidates.clear();
        ebootNames.clear();
        boolean needEboot = replace || !haveEboot(), needFonts = replace || !haveFonts();
        int badIsos = 0;
        try {
            badIsos = walk(cr, tree, DocumentsContract.getTreeDocumentId(tree), 0, needEboot, needFonts);
            if (needEboot) pickEboot(cr);
        } catch (Exception e) {
            return "Could not read that folder: " + e.getMessage();
        }
        if (foundIso == null)
            return badIsos > 0 ? "The .iso in that folder is not a valid PSP dump of the game."
                               : "There is no .iso in that folder.";
        // The new disc's folder replaces the old one.
        String old = prefs.getString("folder", null);
        if (old != null && !old.equals(tree.toString()))
            for (UriPermission p : cr.getPersistedUriPermissions())
                if (p.getUri().toString().equals(old))
                    try { cr.releasePersistableUriPermission(p.getUri(), Intent.FLAG_GRANT_READ_URI_PERMISSION); }
                    catch (Exception ignored) { }
        prefs.edit().putString("folder", tree.toString()).putString("iso", foundIso.toString())
                .putString("iso_name", foundIsoName).apply();
        return null;
    }

    /** One folder (and two below it); returns how many .iso files were not PSP discs. */
    private int walk(ContentResolver cr, Uri tree, String doc, int depth, boolean needEboot, boolean needFonts) throws Exception {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, doc);
        int bad = 0;
        try (Cursor c = cr.query(children, new String[] { DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE }, null, null, null)) {
            while (c != null && c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                if (name == null) continue;
                String low = name.toLowerCase();
                Uri uri = DocumentsContract.buildDocumentUriUsingTree(tree, id);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    if (depth < 2) bad += walk(cr, tree, id, depth + 1, needEboot, needFonts);
                } else if (low.endsWith(".iso") && foundIso == null) {
                    try (ParcelFileDescriptor fd = cr.openFileDescriptor(uri, "r")) {
                        if (pspDisc(new FileInputStream(fd.getFileDescriptor()).getChannel())) { foundIso = uri; foundIsoName = name; }
                        else bad++;
                    } catch (Exception e) {
                        bad++;
                    }
                } else if (needEboot && low.endsWith(".bin") && (low.contains("eboot") || low.contains("ules01213"))) {
                    ebootCandidates.add(uri);              // judged by content once the walk is done
                    ebootNames.add(name);
                } else if (needFonts && low.endsWith(".pgf")) {
                    copy(cr, uri, new File(fonts(), name));
                }
            }
        }
        return bad;
    }

    private static void copy(ContentResolver cr, Uri from, File to) throws Exception {
        to.getParentFile().mkdirs();
        try (InputStream in = cr.openInputStream(from); OutputStream out = new FileOutputStream(to)) {
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
        }
    }
}
