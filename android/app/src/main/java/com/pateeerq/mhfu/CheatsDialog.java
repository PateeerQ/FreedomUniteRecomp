package com.pateeerq.mhfu;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Color;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * The drawer's Cheats: root/cheats/ULES01213.ini (CWCheat, as PPSSPP reads it;
 * host/mods.c reads it again when it changes, so a tick applies at once). The
 * list can come from the CWCheat Database Plus (the one PPSSPP uses). Cheats
 * that write the game's code cannot work on the recompiled game and are greyed
 * out; cheats pause in rooms, and achievements do not unlock while one is on.
 */
final class CheatsDialog {
    private static final String DB_URL = "https://raw.githubusercontent.com/Saramagrean/CWCheat-Database-Plus-/master/cheat.db";

    static final class Cheat {
        String name;
        int line;
        boolean on, header, writesCode;
    }

    private CheatsDialog() { }

    static File file(Activity a) { return new File(a.getExternalFilesDir(null), "root/cheats/ULES01213.ini"); }

    static List<String> lines(File f) {
        List<String> out = new ArrayList<>();
        if (!f.exists()) return out;
        try {
            String s = new String(java.nio.file.Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
            for (String l : s.split("\n", -1)) out.add(l.endsWith("\r") ? l.substring(0, l.length() - 1) : l);
            if (!out.isEmpty() && out.get(out.size() - 1).isEmpty()) out.remove(out.size() - 1);
        } catch (Exception ignored) { }
        return out;
    }

    static void write(File f, List<String> ls) throws Exception {
        f.getParentFile().mkdirs();
        StringBuilder b = new StringBuilder();
        for (String l : ls) b.append(l).append('\n');
        try (FileOutputStream o = new FileOutputStream(f)) { o.write(b.toString().getBytes(StandardCharsets.UTF_8)); }
    }

    private static boolean ours(String l) { return l.contains("ULES-01213") || l.contains("ULES01213"); }

    static List<Cheat> load(File f) {
        List<Cheat> out = new ArrayList<>();
        List<String> ls = lines(f);
        boolean game = true, anyLine = false, nonZero = false;
        Cheat cur = null;
        for (int i = 0; i < ls.size(); i++) {
            String l = ls.get(i).trim();
            if (l.length() < 2 || l.charAt(0) != '_') continue;
            if (l.charAt(1) == 'S') { if (cur != null) cur.header = !anyLine || !nonZero; cur = null; game = ours(l); continue; }
            if (!game) continue;
            if (l.charAt(1) == 'C') {
                if (cur != null) cur.header = !anyLine || !nonZero;
                cur = new Cheat();
                cur.name = l.length() > 4 ? l.substring(4) : "";
                cur.line = i;
                cur.on = l.length() > 2 && l.charAt(2) >= '1' && l.charAt(2) <= '9';
                out.add(cur);
                anyLine = nonZero = false;
            } else if (l.charAt(1) == 'L' && cur != null) {
                String[] p = l.substring(2).trim().split("\\s+");
                if (p.length < 2) continue;
                try {
                    long a = Long.decode(p[0]), v = Long.decode(p[1]);
                    anyLine = true;
                    if (a != 0 || v != 0) nonZero = true;
                    long t = a >>> 28, off = a & 0x0FFFFFFF;
                    // types 0-2, 4 and 8 write where they say: the EBOOT's code (host/mods.h) is recompiled
                    if ((t <= 2 || t == 4 || t == 8) && off >= 0x4000 && off < 0x10E6B8) cur.writesCode = true;
                } catch (NumberFormatException ignored) { }
            }
        }
        if (cur != null) cur.header = !anyLine || !nonZero;
        return out;
    }

    /** ULES-01213's block of a database into the file; what was on stays on. How many cheats. */
    static int importText(File f, String text) throws Exception {
        Set<String> on = new HashSet<>();
        for (Cheat c : load(f)) if (c.on) on.add(c.name);
        List<String> out = new ArrayList<>();
        out.add("_S ULES-01213");
        out.add("_G Monster Hunter Freedom Unite [EU]");
        boolean games = false, in = false;
        int n = 0;
        for (String raw : text.split("\n")) {
            String l = raw.trim();
            if (l.startsWith("_S")) { games = true; in = ours(l); continue; }
            if (!in && games) continue;
            if (l.startsWith("_C")) {
                String name = l.length() > 4 ? l.substring(4) : "";
                out.add("_C" + (on.contains(name) ? "1" : "0") + " " + name);
                n++;
            } else if (l.startsWith("_L")) out.add(l);
        }
        if (n == 0) throw new Exception("no cheats for ULES-01213 in it");
        write(f, out);
        return n;
    }

    static void show(Activity a) {
        File f = file(a);
        List<Cheat> cheats = new ArrayList<>(load(f));
        float dp = a.getResources().getDisplayMetrics().density;

        LinearLayout col = new LinearLayout(a);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding((int) (16 * dp), 0, (int) (16 * dp), 0);
        TextView note = new TextView(a);
        note.setTextSize(12);
        note.setTextColor(Color.rgb(190, 178, 156));
        note.setText("Cheats pause while you are in a room; achievements do not unlock while one is on. "
                + "Greyed out: changes the game's code, which cannot work here.");
        col.addView(note);
        TextView status = new TextView(a);
        status.setTextSize(12);
        status.setTextColor(Color.rgb(236, 194, 104));
        col.addView(status);
        ListView list = new ListView(a);
        col.addView(list, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                (int) (a.getResources().getDisplayMetrics().heightPixels * 0.5f)));
        BaseAdapter adapter = new BaseAdapter() {
            @Override public int getCount() { return cheats.size(); }
            @Override public Object getItem(int i) { return cheats.get(i); }
            @Override public long getItemId(int i) { return i; }
            @Override public View getView(int i, View old, ViewGroup parent) {
                Cheat c = cheats.get(i);
                if (c.header) {
                    TextView t = new TextView(a);
                    t.setText(c.name);
                    t.setTextColor(Color.rgb(236, 194, 104));
                    t.setPadding(0, (int) (8 * dp), 0, (int) (2 * dp));
                    return t;
                }
                CheckBox b = new CheckBox(a);
                b.setText(c.name);
                b.setChecked(c.on && !c.writesCode);
                b.setEnabled(!c.writesCode);
                b.setOnCheckedChangeListener((v, on) -> {
                    List<String> ls = lines(f);
                    String l = ls.get(c.line);
                    int at = l.indexOf("_C");
                    if (at < 0 || at + 2 >= l.length()) return;
                    ls.set(c.line, l.substring(0, at + 2) + (on ? '1' : '0') + l.substring(at + 3));
                    try { write(f, ls); c.on = on; } catch (Exception e) { status.setText(e.getMessage()); }
                });
                return b;
            }
        };
        list.setAdapter(adapter);
        status.setText(cheats.isEmpty() ? "No cheats yet: get the list." : cheats.size() + " cheats");

        AlertDialog d = new AlertDialog.Builder(a, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Cheats")
                .setView(col)
                .setNeutralButton("Get the list", null)
                .setNegativeButton("Close", null)
                .create();
        d.setOnShowListener(x -> d.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> {
            status.setText("Downloading the CWCheat Database Plus...");
            new Thread(() -> {
                String msg;
                try {
                    HttpURLConnection c = (HttpURLConnection) new URL(DB_URL).openConnection();
                    c.setConnectTimeout(15000);
                    c.setReadTimeout(30000);
                    ByteArrayOutputStream buf = new ByteArrayOutputStream();
                    try (InputStream in = c.getInputStream()) {
                        byte[] b = new byte[1 << 16];
                        for (int k; (k = in.read(b)) > 0;) buf.write(b, 0, k);
                    }
                    int n = importText(f, buf.toString("UTF-8"));
                    msg = n + " cheats for ULES-01213";
                } catch (Exception e) {
                    msg = "Could not get the list: " + e.getMessage();
                }
                final String m = msg;
                a.runOnUiThread(() -> {
                    cheats.clear();
                    cheats.addAll(load(f));
                    adapter.notifyDataSetChanged();
                    status.setText(m);
                });
            }).start();
        }));
        d.show();
    }
}
