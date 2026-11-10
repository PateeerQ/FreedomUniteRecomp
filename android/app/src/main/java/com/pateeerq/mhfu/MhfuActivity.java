package com.pateeerq.mhfu;

import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.system.Os;
import android.text.InputFilter;
import android.text.InputType;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;

import org.libsdl.app.SDLActivity;

/**
 * The game's activity: SDL's, with SDL linked statically into libmain.so (the
 * recompiled game, psprecomp and SDL3, built from the repository's
 * CMakeLists.txt), so that is the only library to load.
 *
 * SetupActivity starts it once the game's files are there (iso.txt, the
 * EBOOT, the fonts in /sdcard/Android/data/com.pateeerq.mhfu/files). The
 * drawer's Multiplayer, RetroAchievements and Check for updates open dialogs
 * over the game (never another window); they reach the game through the
 * natives in host/android.c.
 */
public class MhfuActivity extends SDLActivity {
    private static boolean checked;     // once per process: the quiet update check at start
    private WifiManager.MulticastLock multicast;
    private static final int PICK_SAVES = 31, PICK_TEXTURES = 32;
    /** What the game writes to its memory stick, to the save folder (SaveSync): every few seconds. */
    private android.os.HandlerThread savesThread;
    private android.os.Handler saves;
    private final Runnable pushSaves = new Runnable() {
        @Override public void run() {
            SaveSync.push(MhfuActivity.this);
            if (saves != null) saves.postDelayed(this, 4000);
        }
    };

    // host/android.c
    static native String lobbyCreate(String passcode, boolean internet);
    static native boolean lobbyHasRelay();
    static native String lobbyJoin(String code, String passcode);
    static native void lobbyLeave();
    static native String[] lobbyList();
    static native String lobbyStatus();
    static native String lobbyCode();
    static native String lobbyNick();
    static native void lobbySetNick(String nick);
    static native void achLogin(String user, String token);
    static native void achLogout();
    static native String achUser();

    @Override
    protected String[] getLibraries() {
        return new String[] { Region.get(this).library() };   // libmain_<release>.so in the universal app
    }

    private SharedPreferences ra() { return getSharedPreferences("retroachievements", MODE_PRIVATE); }

    @Override
    protected void onCreate(Bundle saved) {
        // Before the game starts: a fresh process (Android may stop the app in
        // the background and bring this screen back alone) has no open disc.
        String disc = SetupActivity.openDisc(this);
        if (disc != null) android.util.Log.w("mhfu", "disc: " + disc);
        // The game signs in at start with what the last sign-in kept (achievements.c).
        String user = ra().getString("user", ""), token = ra().getString("token", "");
        try {
            if (!user.isEmpty() && !token.isEmpty()) { Os.setenv("MHFU_RA_USER", user, true); Os.setenv("MHFU_RA_TOKEN", token, true); }
        } catch (Exception ignored) { }
        super.onCreate(saved);
        // Rooms on the LAN are found by UDP broadcast, which Wi-Fi drops without this.
        try {
            WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            multicast = wifi.createMulticastLock("mhfu-rooms");
            multicast.setReferenceCounted(false);
            multicast.acquire();
        } catch (Exception ignored) { }
        if (!checked) { checked = true; Updater.check(this, true, msg -> { }); }
        savesThread = new android.os.HandlerThread("saves");
        savesThread.start();
        saves = new android.os.Handler(savesThread.getLooper());
        saves.postDelayed(pushSaves, 4000);
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (saves != null) saves.post(() -> SaveSync.push(this));     // leaving: what was written so far
    }

    /** "Save folder": where the saves are copied, and changing it. */
    public void chooseSaveFolder() {
        runOnUiThread(() -> {
            String where = SaveSync.describe(this);
            dialog("Save folder")
                    .setMessage((where == null
                            ? "Your saves are only in the app, so Clear data or a reinstall would remove them."
                            : "Your saves are copied to " + where + " as you play.")
                            + "\n\nChoose a folder you can see (for example Documents/MHFU, \"Use this folder\") and they "
                            + "are kept there too, the way PPSSPP keeps its memory stick.")
                    .setPositiveButton(where == null ? "Choose folder" : "Change folder", (d, w) ->
                            startActivityForResult(new android.content.Intent(android.content.Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_SAVES))
                    .setNeutralButton(where == null ? null : "Load from folder", where == null ? null : (d, w) -> loadSavesFromFolder())
                    .setNegativeButton("Close", null)
                    .show();
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, android.content.Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == PICK_TEXTURES && resultCode == RESULT_OK && data != null && data.getData() != null) {
            TexturePack.install(this, data.getData());
            return;
        }
        if (requestCode != PICK_SAVES || resultCode != RESULT_OK || data == null || data.getData() == null) return;
        try {
            // While playing the app's saves are the ones that count: they go to the new folder.
            SaveSync.choose(this, data.getData());
            saves.post(() -> {
                int n = SaveSync.push(this);
                runOnUiThread(() -> toast("Saves now go to " + SaveSync.describe(this) + (n > 0 ? " (" + n + " file(s) copied)." : ".")));
            });
        } catch (Exception e) {
            toast("That folder cannot be used: " + e.getMessage());
        }
    }

    /**
     * Landscape both ways, by the sensor. SDL turns "LandscapeLeft LandscapeRight"
     * into USER_LANDSCAPE, which keeps to one side whenever the phone's auto-rotate
     * is off; a game is held either way round, so it follows the sensor regardless.
     */
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    @Override
    protected void onDestroy() {
        if (saves != null) { SaveSync.push(this); saves.removeCallbacksAndMessages(null); savesThread.quitSafely(); }
        if (multicast != null && multicast.isHeld()) multicast.release();
        super.onDestroy();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        Updater.installStatus(this, intent);
    }

    // ---- small pieces -------------------------------------------------------------------

    private AlertDialog.Builder dialog(String title) {
        return new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle(title);
    }

    private void toast(String s) { Toast.makeText(this, s, Toast.LENGTH_LONG).show(); }

    private LinearLayout column() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setPadding(60, 24, 60, 8);
        return l;
    }

    private TextView line(String s) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(15);
        t.setPadding(0, 12, 0, 4);
        return t;
    }

    private EditText field(String hint, String value, int type) {
        EditText e = new EditText(this);
        e.setHint(hint);
        e.setText(value);
        e.setSingleLine(true);
        e.setInputType(type);
        return e;
    }

    private EditText passcodeField() {
        EditText e = field("Passcode (4 digits, or empty)", "", InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_VARIATION_PASSWORD);
        e.setFilters(new InputFilter[] { new InputFilter.LengthFilter(4) });
        return e;
    }

    interface Work { String run(); }
    interface After { void run(String result); }

    /** `work` off the UI thread with a "please wait" dialog, then `after` on it. */
    private void busy(String message, Work work, After after) {
        AlertDialog wait = dialog("Multiplayer").setMessage(message).setCancelable(false).show();
        new Thread(() -> {
            String r = work.run();
            runOnUiThread(() -> { wait.dismiss(); after.run(r); });
        }).start();
    }

    // ---- the drawer's items (window_sdl.c, over JNI) ---------------------------------------

    /** "Check for updates". */
    /** The drawer's Cheats (over JNI): the cheat list (CheatsDialog). */
    public void openCheats() {
        runOnUiThread(() -> CheatsDialog.show(this));
    }

    /** The drawer's "Install texture pack" (TexturePack). */
    public void installTexturePack() {
        TexturePack.offer(this, PICK_TEXTURES);
    }

    public void checkUpdates() {
        runOnUiThread(() -> {
            Toast.makeText(this, "Checking for updates...", Toast.LENGTH_SHORT).show();
            Updater.check(this, false, msg -> { if (msg != null) toast(msg); });
        });
    }

    /** "About": what this is and who maintains it (the words of host/about.h). */
    public void openAbout() {
        runOnUiThread(() -> dialog(getString(R.string.app_name))
                .setIcon(R.mipmap.ic_launcher)
                .setMessage("Version " + Updater.currentVersion(this) + "\n\n"
                        + "A native port of " + Region.get(this).title + " (PSP, " + Region.get(this).gameId + "), statically recompiled.\n\n"
                        + "Maintained by PateeerQ.\ngithub.com/" + Updater.REPO + "\n\n"
                        + "Free software under the GNU GPL v3. Built on psprecomp (sp00nznet), SDL3, FFmpeg (LGPL), "
                        + "rcheevos, stb, Basis Universal and libkirk (GPL v3, from PPSSPP).\n"
                        + "Touch button icons by Lorc and Delapouite, game-icons.net (CC BY 3.0).\n\n"
                        + "Not affiliated with or endorsed by Capcom. Monster Hunter is a trademark of Capcom. "
                        + "Play with your own copy of the game: no game files come with this app.")
                .setPositiveButton("OK", null)
                .setNeutralButton("GitHub", (d, w) -> {
                    try {
                        startActivity(new Intent(Intent.ACTION_VIEW, android.net.Uri.parse("https://github.com/" + Updater.REPO)));
                    } catch (Exception ignored) { }
                })
                .show());
    }

    /**
     * "Change game files": the game closes and starts again at SetupActivity,
     * which asks for the folder (a new .iso, EBOOT or fonts). A restart, not a
     * window over the game: the game reads its files only when it starts.
     */
    public void changeGameFiles() {
        runOnUiThread(() -> dialog("Change game files")
                .setMessage("The game closes and starts again, then asks for the folder with your game files "
                        + "(the .iso, the decrypted EBOOT, the PSP fonts). Your saves stay as they are.\n\n"
                        + "Save your game first: what is not saved is lost.")
                .setPositiveButton("Close and choose", (d, w) -> {
                    getSharedPreferences("setup", MODE_PRIVATE).edit().putBoolean(SetupActivity.ASK, true).commit();
                    restart();
                })
                .setNegativeButton("Cancel", null)
                .show());
    }

    /** "Copy crash log": the last crash's log, for a report. */
    public void copyCrashLog() {
        runOnUiThread(() -> CrashLog.offer(this, "Crash log",
                "The log of the last time the game closed because of an error ("
                        + java.text.DateFormat.getDateTimeInstance().format(new java.util.Date(CrashLog.file(this).lastModified()))
                        + "). Copy it and paste it into your report.", null));
    }

    /** The game, closed and started again (from SetupActivity) in a moment: it reads its files only at start. */
    private void restart() {
        // RestartActivity (its own process) starts SetupActivity once this process is gone.
        startActivity(new Intent(this, RestartActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        overridePendingTransition(0, 0);
        new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
            finishAffinity();
            System.exit(0);
        }, 200);
    }

    /** "Load from folder": the folder's saves over the app's, then the game again, to read them. */
    private void loadSavesFromFolder() {
        dialog("Load from folder")
                .setMessage("The saves in " + SaveSync.describe(this) + " replace the ones in the app, and the game "
                        + "starts again to read them.\n\nWhat you have not saved in the game now is lost, and the app's "
                        + "own saves are overwritten by the folder's.")
                .setPositiveButton("Load and restart", (d, w) -> {
                    if (saves != null) saves.removeCallbacksAndMessages(null);   // no push over what is loaded
                    new Thread(() -> {
                        int n = SaveSync.pullAll(this);
                        runOnUiThread(() -> {
                            if (n < 0) { toast("The save folder could not be read."); return; }
                            toast(n + " file(s) loaded. Starting again...");
                            restart();
                        });
                    }).start();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    /** "Multiplayer": the room this is in, the hunter's name, Create Server / Join Server. */
    public void openMultiplayer() {
        runOnUiThread(() -> {
            LinearLayout col = column();
            col.addView(line(lobbyStatus()));
            col.addView(line("Your name in rooms"));
            EditText nick = field("Hunter", lobbyNick(), InputType.TYPE_CLASS_TEXT);
            nick.setFilters(new InputFilter[] { new InputFilter.LengthFilter(15) });
            col.addView(nick, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            TextView how = line("Create Server opens a room (over the internet, or on this Wi-Fi or hotspot); the "
                    + "others choose Join Server and pick it. Then go online in the game as usual (Gathering Hall).");
            how.setTextSize(13);
            col.addView(how);
            final boolean inRoom = !lobbyCode().isEmpty();
            AlertDialog.Builder b = dialog("Multiplayer").setView(col)
                    .setPositiveButton("Create Server", (d, w) -> { saveNick(nick); askCreate(); })
                    .setNeutralButton("Join Server", (d, w) -> { saveNick(nick); findRooms(); });
            if (inRoom) b.setNegativeButton("Leave room", (d, w) -> { saveNick(nick); lobbyLeave(); toast("Left the room."); });
            else b.setNegativeButton("Close", (d, w) -> saveNick(nick));
            b.show();
        });
    }

    private void saveNick(EditText e) {
        String n = e.getText().toString().trim();
        if (!n.isEmpty() && !n.equals(lobbyNick())) lobbySetNick(n);
    }

    private void askCreate() {
        LinearLayout col = column();
        final boolean relay = lobbyHasRelay();
        android.widget.RadioGroup where = new android.widget.RadioGroup(this);
        android.widget.RadioButton net = new android.widget.RadioButton(this), lan = new android.widget.RadioButton(this);
        net.setText("Internet: players anywhere");
        lan.setText("This network: same Wi-Fi or hotspot");
        net.setId(View.generateViewId());
        lan.setId(View.generateViewId());
        if (relay) {
            where.addView(net);
            where.addView(lan);
            where.check(net.getId());
            col.addView(where);
        }
        col.addView(line("Anyone may join an open room. With a passcode, only those who know it can."));
        EditText pass = passcodeField();
        col.addView(pass);
        dialog("Create Server").setView(col)
                .setPositiveButton("Create", (d, w) -> {
                    String p = pass.getText().toString();
                    if (!p.isEmpty() && p.length() != 4) { toast("A passcode is 4 digits, or empty."); return; }
                    final boolean internet = relay && where.getCheckedRadioButtonId() == net.getId();
                    busy("Opening a room...", () -> lobbyCreate(p, internet), err -> {
                        if (err != null) { dialog("Create Server").setMessage(err).setPositiveButton("OK", null).show(); return; }
                        dialog("Room " + lobbyCode()).setMessage("The room is open" + (p.isEmpty() ? "" : " (passcode " + p + ")")
                                + ". The others choose Join Server and pick it, or type the code " + lobbyCode() + ".")
                                .setPositiveButton("OK", null).show();
                    });
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    /** Join Server: the rooms in a compact, searchable list with their frame rate and ping (RoomPicker). */
    private void findRooms() {
        runOnUiThread(() -> RoomPicker.show(this, this::askCode, (code, locked) -> {
            if (locked) askPasscode(code);
            else join(code, "");
        }));
    }

    private void askCode() {
        LinearLayout col = column();
        EditText code = field("MH-4K7Q", "", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS);
        EditText pass = passcodeField();
        col.addView(code);
        col.addView(pass);
        dialog("Join by code").setView(col)
                .setPositiveButton("Join", (d, w) -> join(code.getText().toString(), pass.getText().toString()))
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void askPasscode(String code) {
        LinearLayout col = column();
        col.addView(line(code + " has a passcode."));
        EditText pass = passcodeField();
        col.addView(pass);
        dialog("Join " + code).setView(col)
                .setPositiveButton("Join", (d, w) -> join(code, pass.getText().toString()))
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void join(String code, String pass) {
        busy("Joining " + code + "...", () -> lobbyJoin(code, pass), err -> {
            if (err != null) dialog("Join Server").setMessage(err).setPositiveButton("OK", null).show();
            else toast("In room " + lobbyCode() + ". " + lobbyStatus());
        });
    }

    /** "RetroAchievements": sign in (once; only the token is kept) or out. */
    public void openAchievements() {
        runOnUiThread(() -> {
            String user = ra().getString("user", "");
            if (!user.isEmpty()) {
                String now = achUser();
                dialog("RetroAchievements")
                        .setMessage("Signed in as " + (now.isEmpty() ? user : now) + ". Unlocks are sent as you play (softcore).")
                        .setPositiveButton("OK", null)
                        .setNegativeButton("Sign out", (d, w) -> {
                            ra().edit().clear().apply();
                            achLogout();
                            toast("Signed out of RetroAchievements.");
                        })
                        .show();
                return;
            }
            LinearLayout col = column();
            col.addView(line("Sign in with your retroachievements.org account. The password is sent once; "
                    + "only the login token is kept on this phone."));
            EditText name = field("Username", "", InputType.TYPE_CLASS_TEXT);
            EditText pass = field("Password", "", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
            col.addView(name);
            col.addView(pass);
            dialog("RetroAchievements").setView(col)
                    .setPositiveButton("Sign in", (d, w) -> signIn(name.getText().toString().trim(), pass.getText().toString()))
                    .setNegativeButton("Cancel", null)
                    .show();
        });
    }

    private void signIn(String user, String password) {
        if (user.isEmpty() || password.isEmpty()) { toast("Enter the username and the password."); return; }
        final String[] token = new String[1], name = new String[1];
        busy("Signing in...", () -> {
            try {
                String post = "r=login2&u=" + URLEncoder.encode(user, "UTF-8") + "&p=" + URLEncoder.encode(password, "UTF-8");
                byte[] r = http("https://retroachievements.org/dorequest.php", post, "application/x-www-form-urlencoded");
                if (r.length < 4) return "No connection to RetroAchievements.";
                JSONObject j = new JSONObject(new String(r, 4, r.length - 4, "UTF-8"));
                if (!j.optBoolean("Success")) return j.optString("Error", "Sign-in failed.");
                token[0] = j.optString("Token");
                name[0] = j.optString("User", user);
                return null;
            } catch (Exception e) {
                return "Sign-in failed: " + e.getMessage();
            }
        }, err -> {
            if (err != null) { dialog("RetroAchievements").setMessage(err).setPositiveButton("OK", null).show(); return; }
            ra().edit().putString("user", name[0]).putString("token", token[0]).apply();
            achLogin(name[0], token[0]);
            toast("Signed in as " + name[0] + ".");
        });
    }

    /**
     * For achievements.c (rcheevos' requests), from any thread: GET, or POST
     * when `post` is given. The HTTP status in 4 bytes (0: no answer), then
     * the body.
     */
    public static byte[] http(String url, String post, String contentType) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        int status = 0;
        byte[] body = new byte[0];
        try {
            HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(10000);
            c.setReadTimeout(30000);
            c.setRequestProperty("User-Agent", "FreedomUniteRecomp/0.1 (Android)");
            if (post != null) {
                c.setDoOutput(true);
                c.setRequestMethod("POST");
                c.setRequestProperty("Content-Type", contentType != null ? contentType : "application/x-www-form-urlencoded");
                try (OutputStream o = c.getOutputStream()) { o.write(post.getBytes("UTF-8")); }
            }
            status = c.getResponseCode();
            InputStream in = status >= 400 ? c.getErrorStream() : c.getInputStream();
            if (in != null) {
                ByteArrayOutputStream b = new ByteArrayOutputStream();
                byte[] buf = new byte[16384];
                for (int n; (n = in.read(buf)) > 0;) b.write(buf, 0, n);
                in.close();
                body = b.toByteArray();
            }
        } catch (Exception e) {
            status = 0;
        }
        out.write(status >>> 24); out.write(status >>> 16); out.write(status >>> 8); out.write(status);
        out.write(body, 0, body.length);
        return out.toByteArray();
    }
}
