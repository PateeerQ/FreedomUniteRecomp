package com.pateeerq.mhfu;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.text.Editable;
import android.text.InputType;
import android.text.TextUtils;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/**
 * Join Server: the open rooms (this network's and the relay's) in a compact,
 * scrolling list with a search box. Each row: the code and the room's name,
 * players, a lock for a passcode, LAN or Internet, the room's frame rate (its
 * slowest player's: what you would play at there) and the ping to its server.
 * The nearest rooms come first. The list comes from MhfuActivity.lobbyList()
 * (host/android.c), refreshed in place.
 */
final class RoomPicker {
    interface Pick { void room(String code, boolean locked); }

    private static final int GOLD = Color.rgb(236, 194, 104), TEXT = Color.rgb(250, 245, 235),
            DIM = Color.rgb(190, 178, 156), GOOD = Color.rgb(140, 210, 110), OK = Color.rgb(230, 200, 90),
            BAD = Color.rgb(230, 110, 90);

    static final class Room {
        String code = "", name = "";
        int players, fps, ping = -1;
        boolean locked, internet;
    }

    private RoomPicker() { }

    /** "code\tname\tplayers\tlocked\tinternet\tfps\tping" lines, nearest first. */
    static List<Room> parse(String[] lines) {
        List<Room> out = new ArrayList<>();
        if (lines == null) return out;
        for (String line : lines) {
            String[] f = line.split("\t", -1);
            Room r = new Room();
            r.code = f[0];
            if (f.length > 1) r.name = f[1];
            try {
                if (f.length > 2) r.players = Integer.parseInt(f[2]);
                if (f.length > 3) r.locked = f[3].equals("1");
                if (f.length > 4) r.internet = f[4].equals("1");
                if (f.length > 5) r.fps = Integer.parseInt(f[5]);
                if (f.length > 6) r.ping = Integer.parseInt(f[6]);
            } catch (NumberFormatException ignored) { }
            out.add(r);
        }
        Collections.sort(out, (a, b) -> Integer.compare(a.ping < 0 ? Integer.MAX_VALUE : a.ping, b.ping < 0 ? Integer.MAX_VALUE : b.ping));
        return out;
    }

    private static int dp(Activity a, float v) { return (int) (v * a.getResources().getDisplayMetrics().density + 0.5f); }

    private static TextView text(Activity a, String s, int color, float sp, boolean bold) {
        TextView t = new TextView(a);
        t.setText(s);
        t.setTextColor(color);
        t.setTextSize(sp);
        t.setSingleLine(true);
        t.setEllipsize(TextUtils.TruncateAt.END);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    /** A small rounded tag: "60 FPS". */
    private static TextView badge(Activity a, String s, int color) {
        TextView t = text(a, s, color, 11, true);
        GradientDrawable g = new GradientDrawable();
        g.setCornerRadius(dp(a, 8));
        g.setStroke(dp(a, 1), color);
        g.setColor(Color.argb(40, Color.red(color), Color.green(color), Color.blue(color)));
        t.setBackground(g);
        t.setPadding(dp(a, 6), dp(a, 1), dp(a, 6), dp(a, 1));
        // Never shortened: as wide as its text ("60 FPS" came out as "6...").
        t.setEllipsize(null);
        t.setGravity(Gravity.CENTER);
        t.setMinWidth((int) Math.ceil(t.getPaint().measureText(s)) + dp(a, 14));
        return t;
    }

    private static View row(Activity a, Room r) {
        LinearLayout row = new LinearLayout(a);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(a, 10), dp(a, 8), dp(a, 10), dp(a, 8));

        LinearLayout left = new LinearLayout(a);
        left.setOrientation(LinearLayout.VERTICAL);
        LinearLayout top = new LinearLayout(a);
        top.setOrientation(LinearLayout.HORIZONTAL);
        top.setGravity(Gravity.CENTER_VERTICAL);
        top.addView(text(a, r.code, GOLD, 14, true));
        TextView name = text(a, "  " + r.name, TEXT, 13, false);
        top.addView(name, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        left.addView(top);
        String info = r.players + (r.players == 1 ? " player" : " players") + "  ·  " + (r.internet ? "Internet" : "LAN")
                + (r.locked ? "  ·  🔒 passcode" : "");
        left.addView(text(a, info, DIM, 11, false));
        row.addView(left, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));

        LinearLayout right = new LinearLayout(a);
        right.setOrientation(LinearLayout.VERTICAL);
        right.setMinimumWidth(dp(a, 72));
        right.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        right.addView(r.fps >= 60 ? badge(a, "60 FPS", GOOD) : r.fps > 0 ? badge(a, "30 FPS", DIM) : badge(a, "30 FPS?", DIM));
        TextView ping = text(a, r.ping < 0 ? "-- ms" : r.ping + " ms", r.ping < 0 ? DIM : r.ping < 80 ? GOOD : r.ping < 150 ? OK : BAD, 12, true);
        ping.setGravity(Gravity.END);
        LinearLayout.LayoutParams pl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        pl.topMargin = dp(a, 3);
        pl.gravity = Gravity.END;
        right.addView(ping, pl);
        LinearLayout.LayoutParams rl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        rl.leftMargin = dp(a, 10);
        row.addView(right, rl);
        return row;
    }

    /** The dialog. `byCode` opens "Join by code"; `pick` joins a room (asking its passcode first when it has one). */
    static void show(Activity a, Runnable byCode, Pick pick) {
        final List<Room> all = new ArrayList<>(), shown = new ArrayList<>();

        LinearLayout col = new LinearLayout(a);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(a, 16), dp(a, 4), dp(a, 16), 0);

        LinearLayout bar = new LinearLayout(a);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        EditText search = new EditText(a);
        search.setHint("Search code or name");
        search.setSingleLine(true);
        search.setTextSize(14);
        search.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        bar.addView(search, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        Button refresh = new Button(a, null, android.R.attr.borderlessButtonStyle);
        refresh.setText("Refresh");
        refresh.setTextColor(GOLD);
        refresh.setAllCaps(false);
        bar.addView(refresh);
        col.addView(bar);

        TextView status = text(a, "", DIM, 12, false);
        status.setPadding(dp(a, 4), 0, 0, dp(a, 4));
        col.addView(status);

        ListView list = new ListView(a);
        list.setDividerHeight(dp(a, 1));
        list.setDivider(new android.graphics.drawable.ColorDrawable(Color.argb(60, 236, 194, 104)));
        // At most half the screen: the dialog stays small and the list scrolls.
        col.addView(list, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                (int) (a.getResources().getDisplayMetrics().heightPixels * 0.5f)));

        BaseAdapter adapter = new BaseAdapter() {
            @Override public int getCount() { return shown.size(); }
            @Override public Object getItem(int i) { return shown.get(i); }
            @Override public long getItemId(int i) { return i; }
            @Override public View getView(int i, View old, ViewGroup parent) { return row(a, shown.get(i)); }
        };
        list.setAdapter(adapter);

        final boolean[] loading = { false };
        Runnable filter = () -> {
            String q = search.getText().toString().trim().toLowerCase(Locale.ROOT);
            shown.clear();
            for (Room r : all)
                if (q.isEmpty() || r.code.toLowerCase(Locale.ROOT).contains(q) || r.name.toLowerCase(Locale.ROOT).contains(q))
                    shown.add(r);
            adapter.notifyDataSetChanged();
            if (loading[0]) status.setText("Looking for rooms...");
            else if (all.isEmpty()) status.setText("No open rooms on this network or the server. Create one, or join by code.");
            else status.setText(shown.size() + " of " + all.size() + (all.size() == 1 ? " room" : " rooms") + "  ·  nearest first");
        };
        search.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int st, int c, int n) { }
            @Override public void onTextChanged(CharSequence s, int st, int b, int c) { }
            @Override public void afterTextChanged(Editable e) { filter.run(); }
        });

        Runnable load = () -> {
            if (loading[0]) return;
            loading[0] = true;
            refresh.setEnabled(false);
            filter.run();
            new Thread(() -> {
                List<Room> got = parse(MhfuActivity.lobbyList());
                a.runOnUiThread(() -> {
                    all.clear();
                    all.addAll(got);
                    loading[0] = false;
                    refresh.setEnabled(true);
                    filter.run();
                });
            }).start();
        };
        refresh.setOnClickListener(v -> load.run());

        AlertDialog d = new AlertDialog.Builder(a, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Join Server")
                .setView(col)
                .setNeutralButton("Join by code", (x, w) -> byCode.run())
                .setNegativeButton("Close", null)
                .create();
        list.setOnItemClickListener((p, v, i, id) -> {
            Room r = shown.get(i);
            d.dismiss();
            pick.room(r.code, r.locked);
        });
        d.show();
        load.run();
    }
}
