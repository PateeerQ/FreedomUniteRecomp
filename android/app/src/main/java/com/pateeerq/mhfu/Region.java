package com.pateeerq.mhfu;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * Which release of the game the app plays. Each (Europe, USA, Japan's Portable
 * 2nd G, the English FUComplete disc of it) is recompiled into a game of its
 * own: libmain_eu.so ... in the universal app (-Pmhfu.region=ALL), or the one
 * libmain.so of an app built for one release. The universal app tells the
 * release from the disc the player chose (its PARAM.SFO's product code, and
 * FUComplete's FUC folder; SetupActivity) and keeps it in the "setup" prefs
 * ("region"); the native side has its own copy of these (host/region.h).
 */
final class Region {
    final String tag, lib, gameId, gameIdDash, name, title, ebootFile;
    final long ebootSize;
    final int codeEnd;
    final boolean ebootOnDisc;

    private Region(String tag, String lib, String gameId, String gameIdDash, String name, String title,
                   String ebootFile, long ebootSize, int codeEnd, boolean ebootOnDisc) {
        this.tag = tag; this.lib = lib; this.gameId = gameId; this.gameIdDash = gameIdDash; this.name = name;
        this.title = title; this.ebootFile = ebootFile; this.ebootSize = ebootSize; this.codeEnd = codeEnd;
        this.ebootOnDisc = ebootOnDisc;
    }

    static final Region[] ALL = {
        new Region("EU", "main_eu", "ULES01213", "ULES-01213", "Europe", "Monster Hunter Freedom Unite",
                   "ULES01213_EBOOT.BIN", 1910384L, 0x10E6B8, false),
        new Region("US", "main_us", "ULUS10391", "ULUS-10391", "USA", "Monster Hunter Freedom Unite",
                   "ULUS10391_EBOOT.BIN", 1910672L, 0x10E7D8, false),
        new Region("JP", "main_jp", "ULJM05500", "ULJM-05500", "Japan", "Monster Hunter Portable 2nd G",
                   "ULJM05500_EBOOT.BIN", 1890112L, 0x10C1F0, false),
        new Region("FUC", "main_fuc", "ULJM05500", "ULJM-05500", "FUComplete", "Monster Hunter Freedom Unite Complete",
                   "ULJM05500_FUC_EBOOT.BIN", 1889772L, 0x10C1F0, true),
    };

    private static Region current;

    static Region byTag(String tag) {
        for (Region r : ALL) if (r.tag.equals(tag)) return r;
        return null;
    }

    /** The release this app plays now. */
    static Region get(Context ctx) {
        if (current != null) return current;
        Region r = null;
        if (BuildConfig.UNIVERSAL) {
            SharedPreferences p = ctx.getSharedPreferences("setup", Context.MODE_PRIVATE);
            r = byTag(p.getString("region", ""));
        }
        if (r == null) r = byTag(BuildConfig.REGION_TAG);
        if (r == null) r = ALL[0];
        current = r;
        return r;
    }

    /** The release in play, for code without a Context (Region.get ran by then: setup, the game's start). */
    static Region now() {
        if (current != null) return current;
        Region r = byTag(BuildConfig.REGION_TAG);
        return r != null ? r : ALL[0];
    }

    /** The universal app: play `r` from now on (its disc was chosen). */
    static void set(Context ctx, Region r) {
        current = r;
        ctx.getSharedPreferences("setup", Context.MODE_PRIVATE).edit().putString("region", r.tag).apply();
    }

    /** The native library to load: libmain_<tag>.so in the universal app, else libmain.so. */
    String library() { return BuildConfig.UNIVERSAL ? lib : "main"; }

    /** The release on a disc image (a path SetupActivity can read), or null. */
    static Region ofDisc(String isoPath) {
        java.nio.channels.FileChannel ch = null;
        try {
            ch = SetupActivity.discChannel(isoPath);
            long[] sfo = SetupActivity.isoPath(ch, "PSP_GAME", "PARAM.SFO");
            if (sfo == null || sfo[1] > 65536) return null;
            byte[] b = new byte[(int) sfo[1]];
            SetupActivity.readAt(ch, sfo[0] * 2048, b, b.length);
            String s = new String(b, "ISO-8859-1");
            if (s.contains("ULES01213")) return byTag("EU");
            if (s.contains("ULUS10391")) return byTag("US");
            if (s.contains("ULJM05500"))
                return SetupActivity.isoPath(ch, "FUC", "PRELOAD.BIN") != null ? byTag("FUC") : byTag("JP");
            return null;
        } catch (Exception e) {
            android.util.Log.w("mhfu", "which release is the disc: " + e);
            return null;
        } finally {
            SetupActivity.doneWith(isoPath, ch);
        }
    }
}
