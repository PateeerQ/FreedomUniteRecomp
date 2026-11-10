package com.pateeerq.mhfu;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;

/**
 * Starts the game again after MhfuActivity closed its process (the game reads
 * its files only at start: Change game files, Load from folder). It runs in a
 * process of its own (":restart" in the manifest), so it outlives the game's,
 * and it is on screen, so starting SetupActivity is a foreground start. The
 * restart used to be an AlarmManager PendingIntent, which Android 14 and up
 * block as a background activity start ("Background activity launch blocked",
 * HyperOS 2026-11-10): the folder was not asked for and the old disc stayed.
 */
public class RestartActivity extends Activity {
    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        View bg = new View(this);
        bg.setBackgroundColor(0xFF2A1E14);
        setContentView(bg);
        // Give the game's process time to end before SetupActivity starts a new one.
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            startActivity(new Intent(this, SetupActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            finish();
            overridePendingTransition(0, 0);
            new Handler(Looper.getMainLooper()).postDelayed(() -> System.exit(0), 500);
        }, 700);
    }
}
