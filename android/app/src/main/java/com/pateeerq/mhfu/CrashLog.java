package com.pateeerq.mhfu;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;

/**
 * The last crash's log (crash-latest.log, written by the game: host/main.c,
 * keep_crash_log), for a player to paste into a report: copied to the
 * clipboard or handed to another app. Offered once after a crash
 * (SetupActivity) and any time from the menu ("Copy crash log").
 */
final class CrashLog {
    /** What a paste can carry: the end of the log (where the crash is). */
    private static final int MAX_CHARS = 120_000;

    private CrashLog() { }

    static File file(Context c) { return new File(c.getExternalFilesDir(null), "crash-latest.log"); }

    /** The crash's header and as much of the log's end as fits; null when there is none. */
    static String text(Context c) {
        File f = file(c);
        if (!f.isFile() || f.length() == 0) return null;
        try (InputStream in = new FileInputStream(f)) {
            byte[] all = new byte[(int) Math.min(f.length(), 64L << 20)];
            int got = 0;
            for (int n; got < all.length && (n = in.read(all, got, all.length - got)) > 0;) got += n;
            String s = new String(all, 0, got, "UTF-8");
            if (s.length() <= MAX_CHARS) return s;
            int head = s.indexOf("\n\n");                 // the header: version, phone, what crashed
            String top = head > 0 && head < 1000 ? s.substring(0, head + 2) : "";
            return top + "[... the start of the log is left out ...]\n" + s.substring(s.length() - (MAX_CHARS - top.length()));
        } catch (Exception e) {
            return null;
        }
    }

    static void copy(Context c, String text) {
        ClipboardManager cm = (ClipboardManager) c.getSystemService(Context.CLIPBOARD_SERVICE);
        cm.setPrimaryClip(ClipData.newPlainText("MHFU crash log", text));
        Toast.makeText(c, "Crash log copied: paste it into your report.", Toast.LENGTH_LONG).show();
    }

    static void share(Activity a, String text) {
        Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain")
                .putExtra(Intent.EXTRA_SUBJECT, "MHFU Recomp crash log")
                .putExtra(Intent.EXTRA_TEXT, text.length() > 90_000 ? text.substring(text.length() - 90_000) : text);
        a.startActivity(Intent.createChooser(send, "Send the crash log"));
    }

    /** The dialog: Copy, Share, or Close; `then` runs once it is gone (null: nothing). */
    static void offer(Activity a, String title, String message, Runnable then) {
        String text = text(a);
        if (text == null) {
            Toast.makeText(a, "No crash log yet.", Toast.LENGTH_SHORT).show();
            if (then != null) then.run();
            return;
        }
        new AlertDialog.Builder(a, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(title)
                .setMessage(message)
                .setPositiveButton("Copy", (d, w) -> copy(a, text))
                .setNeutralButton("Share", (d, w) -> share(a, text))
                .setNegativeButton("Close", null)
                .setOnDismissListener(d -> { if (then != null) then.run(); })
                .show();
    }
}
