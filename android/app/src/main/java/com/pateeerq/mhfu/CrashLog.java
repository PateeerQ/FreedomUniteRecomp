package com.pateeerq.mhfu;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * The last crash's log (crash-latest.log, written by the game: host/main.c,
 * keep_crash_log), for a player's report: copied to the clipboard (shortened
 * to fit a GitHub issue, which takes 65536 characters) or shared whole as a
 * .zip to attach. Offered once after a crash (SetupActivity) and any time from
 * the menu ("Copy crash log").
 */
final class CrashLog {
    /** What a paste can carry: under GitHub's 65536 characters per issue, with room for words around it. */
    private static final int MAX_CHARS = 55_000;

    private CrashLog() { }

    static File file(Context c) { return new File(c.getExternalFilesDir(null), "crash-latest.log"); }

    private static String readAll(File f) throws Exception {
        try (InputStream in = new FileInputStream(f)) {
            byte[] all = new byte[(int) Math.min(f.length(), 64L << 20)];
            int got = 0;
            for (int n; got < all.length && (n = in.read(all, got, all.length - got)) > 0;) got += n;
            return new String(all, 0, got, "UTF-8");
        }
    }

    /**
     * Repeats folded: the function trail at a crash is often one loop of two or
     * three functions thousands of times over. A block of 1-4 lines that comes
     * back right after itself is kept twice, then counted.
     */
    static String fold(String s) {
        String[] in = s.split("\n", -1);
        List<String> out = new ArrayList<>(in.length);
        int i = 0;
        while (i < in.length) {
            int best = 0, bestReps = 0;
            for (int p = 1; p <= 4 && i + 2 * p <= in.length; p++) {
                int reps = 1;
                while (i + (reps + 1) * p <= in.length) {
                    boolean same = true;
                    for (int k = 0; k < p && same; k++) same = in[i + k].equals(in[i + reps * p + k]);
                    if (!same) break;
                    reps++;
                }
                if (reps >= 3 && reps * p > bestReps * Math.max(best, 1)) { best = p; bestReps = reps; }
            }
            if (best > 0) {
                for (int r = 0; r < 2; r++) for (int k = 0; k < best; k++) out.add(in[i + r * best + k]);
                out.add("    [... the " + (best == 1 ? "line" : best + " lines") + " above, " + (bestReps - 2) + " more times ...]");
                i += bestReps * best;
            } else {
                out.add(in[i++]);
            }
        }
        return String.join("\n", out);
    }

    /** The crash's header and as much of the (folded) log's end as fits; null when there is none. */
    static String text(Context c) {
        File f = file(c);
        if (!f.isFile() || f.length() == 0) return null;
        try {
            String s = fold(readAll(f));
            if (s.length() <= MAX_CHARS) return s;
            int head = s.indexOf("\n\n");                 // the header: version, phone, what crashed
            String top = head > 0 && head < 1000 ? s.substring(0, head + 2) : "";
            return top + "[... the start of the log is left out: Share sends all of it as a .zip ...]\n"
                    + s.substring(s.length() - (MAX_CHARS - top.length()));
        } catch (Exception e) {
            return null;
        }
    }

    static void copy(Context c, String text) {
        ClipboardManager cm = (ClipboardManager) c.getSystemService(Context.CLIPBOARD_SERVICE);
        cm.setPrimaryClip(ClipData.newPlainText("MHFU crash log", text));
        Toast.makeText(c, "Crash log copied: paste it into your report.", Toast.LENGTH_LONG).show();
    }

    /** The whole log (and the run before it) as a .zip, to attach to a report. */
    static void share(Activity a) {
        try {
            File dir = CrashProvider.dir(a);
            dir.mkdirs();
            File[] old = dir.listFiles();
            if (old != null) for (File o : old) o.delete();
            String stamp = new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US)
                    .format(new java.util.Date(file(a).lastModified()));
            File zip = new File(dir, "mhfu-crash-" + stamp + ".zip");
            try (ZipOutputStream z = new ZipOutputStream(new FileOutputStream(zip))) {
                File base = a.getExternalFilesDir(null);
                for (String name : new String[] { "crash-latest.log", "run-prev.log" }) {
                    File f = new File(base, name);
                    if (!f.isFile()) continue;
                    z.putNextEntry(new ZipEntry(name));
                    try (InputStream in = new FileInputStream(f)) {
                        byte[] buf = new byte[1 << 16];
                        for (int n; (n = in.read(buf)) > 0;) z.write(buf, 0, n);
                    }
                    z.closeEntry();
                }
            }
            Uri uri = Uri.parse("content://" + CrashProvider.authority(a) + "/" + zip.getName());
            Intent send = new Intent(Intent.ACTION_SEND).setType("application/zip")
                    .putExtra(Intent.EXTRA_SUBJECT, "MHFU Recomp crash log")
                    .putExtra(Intent.EXTRA_STREAM, uri)
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            send.setClipData(ClipData.newRawUri("crash log", uri));
            a.startActivity(Intent.createChooser(send, "Send the crash log (.zip)"));
        } catch (Exception e) {
            Toast.makeText(a, "Could not make the .zip: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    /** The dialog: Copy, Share (.zip), or Close; `then` runs once it is gone (null: nothing). */
    static void offer(Activity a, String title, String message, Runnable then) {
        String text = text(a);
        if (text == null) {
            Toast.makeText(a, "No crash log yet.", Toast.LENGTH_SHORT).show();
            if (then != null) then.run();
            return;
        }
        new AlertDialog.Builder(a, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(title)
                .setMessage(message + "\n\nCopy puts a shortened log on the clipboard; Share sends the whole log as a "
                        + ".zip, to attach to a GitHub issue.")
                .setPositiveButton("Copy", (d, w) -> copy(a, text))
                .setNeutralButton("Share .zip", (d, w) -> share(a))
                .setNegativeButton("Close", null)
                .setOnDismissListener(d -> { if (then != null) then.run(); })
                .show();
    }
}
