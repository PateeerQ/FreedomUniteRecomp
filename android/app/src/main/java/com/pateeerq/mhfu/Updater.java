package com.pateeerq.mhfu;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.os.Build;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * Updates from this project's GitHub releases (PateeerQ/FreedomUniteRecomp,
 * fixed): the newest release's tag is compared with this app's versionName;
 * if it is newer and has an .apk attached, the player is asked, the APK is
 * downloaded and handed to Android's package installer (which asks too).
 * An update must be signed with the same key as the installed app.
 */
final class Updater {
    static final String REPO = "PateeerQ/FreedomUniteRecomp";
    static final String INSTALL_STATUS = "com.pateeerq.mhfu.INSTALL_STATUS";

    interface Done { void run(String message); }

    static final class Release { String tag, name, notes, apkUrl; long apkSize; }

    private Updater() { }

    static String currentVersion(Activity a) {
        try {
            return a.getPackageManager().getPackageInfo(a.getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "0";
        }
    }

    /** "v0.2.0" newer than "0.1.0"? Numbers compared part by part. */
    static boolean newer(String tag, String than) {
        int[] a = parts(tag), b = parts(than);
        for (int i = 0; i < Math.max(a.length, b.length); i++) {
            int x = i < a.length ? a[i] : 0, y = i < b.length ? b[i] : 0;
            if (x != y) return x > y;
        }
        return false;
    }

    private static int[] parts(String v) {
        v = v.replaceAll("^[vV]", "").replaceAll("[^0-9.].*$", "");
        if (v.isEmpty()) return new int[0];
        String[] p = v.split("\\.");
        int[] out = new int[p.length];
        for (int i = 0; i < p.length; i++) {
            try { out[i] = Integer.parseInt(p[i]); } catch (NumberFormatException e) { out[i] = 0; }
        }
        return out;
    }

    private static HttpURLConnection open(String url, int timeoutMs) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(timeoutMs);
        c.setReadTimeout(timeoutMs);
        c.setRequestProperty("User-Agent", "FreedomUniteRecomp-Android");
        c.setRequestProperty("Accept", "application/vnd.github+json");
        return c;
    }

    /** The newest release, or null; `error` gets why not. On a thread of its own. */
    static Release latest(int timeoutMs, StringBuilder error) {
        try {
            HttpURLConnection c = open("https://api.github.com/repos/" + REPO + "/releases/latest", timeoutMs);
            int code = c.getResponseCode();
            if (code == 404) { error.append("No release published yet (or the repository is private)."); return null; }
            if (code != 200) { error.append("GitHub answered ").append(code).append('.'); return null; }
            ByteArrayOutputStream buf = new ByteArrayOutputStream();
            try (InputStream in = c.getInputStream()) {
                byte[] b = new byte[8192];
                for (int n; (n = in.read(b)) > 0;) buf.write(b, 0, n);
            }
            JSONObject j = new JSONObject(buf.toString("UTF-8"));
            Release r = new Release();
            r.tag = j.optString("tag_name");
            r.name = j.optString("name", r.tag);
            r.notes = j.optString("body");
            JSONArray assets = j.optJSONArray("assets");
            for (int i = 0; assets != null && i < assets.length(); i++) {
                JSONObject as = assets.getJSONObject(i);
                if (as.optString("name").toLowerCase().endsWith(".apk")) {
                    r.apkUrl = as.optString("browser_download_url");
                    r.apkSize = as.optLong("size");
                    break;
                }
            }
            return r;
        } catch (Exception e) {
            error.append("No connection to GitHub.");
            return null;
        }
    }

    /** Checks, and if there is a newer APK asks; `then` runs when nothing more is to be done here. */
    static void check(Activity a, boolean quiet, Done then) {
        new Thread(() -> {
            StringBuilder err = new StringBuilder();
            Release r = latest(quiet ? 4000 : 15000, err);
            final String current = currentVersion(a);
            a.runOnUiThread(() -> {
                if (r == null) { then.run(quiet ? null : err.toString()); return; }
                if (!newer(r.tag, current)) { then.run(quiet ? null : "Up to date (" + current + ")."); return; }
                if (r.apkUrl == null || r.apkUrl.isEmpty()) {
                    then.run(quiet ? null : "Version " + r.tag + " has no APK attached.");
                    return;
                }
                String notes = r.notes == null || r.notes.isEmpty() ? "" : "\n\n" + r.notes;
                new AlertDialog.Builder(a)
                        .setTitle("Version " + r.tag + " is available")
                        .setMessage("You have " + current + "." + notes)
                        .setPositiveButton("Update", (d, w) -> download(a, r, then))
                        .setNegativeButton("Later", (d, w) -> then.run(null))
                        .setOnCancelListener(d -> then.run(null))
                        .show();
            });
        }).start();
    }

    private static void download(Activity a, Release r, Done then) {
        final AlertDialog progress = new AlertDialog.Builder(a)
                .setTitle("Downloading " + r.tag)
                .setMessage("0%")
                .setCancelable(false)
                .show();
        new Thread(() -> {
            File apk = new File(a.getExternalCacheDir(), "update.apk");
            String fail = null;
            try {
                HttpURLConnection c = open(r.apkUrl, 30000);
                c.setInstanceFollowRedirects(true);
                if (c.getResponseCode() != 200) throw new Exception("HTTP " + c.getResponseCode());
                long total = c.getContentLengthLong() > 0 ? c.getContentLengthLong() : r.apkSize, got = 0;
                try (InputStream in = c.getInputStream(); OutputStream out = new FileOutputStream(apk)) {
                    byte[] b = new byte[1 << 16];
                    int last = -1;
                    for (int n; (n = in.read(b)) > 0;) {
                        out.write(b, 0, n);
                        got += n;
                        final int pct = total > 0 ? (int) (got * 100 / total) : 0;
                        if (pct != last) { last = pct; a.runOnUiThread(() -> progress.setMessage(pct + "%")); }
                    }
                }
                install(a, apk);
            } catch (Exception e) {
                fail = "The download failed: " + e.getMessage();
            }
            final String f = fail;
            a.runOnUiThread(() -> { progress.dismiss(); if (f != null) then.run(f); });
        }).start();
    }

    /** The installer's answer (see install): it may need the player's yes. True if it was one. */
    static boolean installStatus(Activity a, Intent intent) {
        if (intent == null || !INSTALL_STATUS.equals(intent.getAction())) return false;
        int st = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        if (st == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirm != null) a.startActivity(confirm);
        } else if (st != PackageInstaller.STATUS_SUCCESS) {
            android.widget.Toast.makeText(a, "The update was not installed: "
                    + intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE), android.widget.Toast.LENGTH_LONG).show();
        }
        return true;
    }

    /** Hands the APK to Android's installer; its answer comes back to the same activity (installStatus). */
    private static void install(Activity a, File apk) throws Exception {
        PackageInstaller pi = a.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        int id = pi.createSession(params);
        try (PackageInstaller.Session s = pi.openSession(id)) {
            try (OutputStream out = s.openWrite("update.apk", 0, apk.length()); InputStream in = new FileInputStream(apk)) {
                byte[] b = new byte[1 << 16];
                for (int n; (n = in.read(b)) > 0;) out.write(b, 0, n);
                s.fsync(out);
            }
            Intent back = new Intent(a, a.getClass()).setAction(INSTALL_STATUS);
            int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
            PendingIntent p = PendingIntent.getActivity(a, id, back, flags);
            s.commit(p.getIntentSender());
        }
    }
}
