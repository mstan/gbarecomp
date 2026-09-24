package org.gbarecomp;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;

import java.util.Locale;

/**
 * Per-game facts for the shared Android shell, read from the application's
 * manifest meta-data. The Gradle template (gbarecomp-app.gradle) fills them
 * from the game's `gbaGame` block through manifest placeholders, so the
 * activities themselves stay game-agnostic.
 */
public final class GbaGameConfig {
    public final String title;
    public final String romLabel;
    public final String[] romSha1;
    public final long romSize;
    public final String romFile;
    public final boolean biosRequired;
    public final String biosSha1;
    public final String orientation;   // landscape | portrait | any
    public final String payloadVersion;
    public final String setupNote;
    public final boolean showGyroStatus;

    private GbaGameConfig(Bundle m) {
        title = string(m, "gbarecomp.title", "gbarecomp");
        romLabel = string(m, "gbarecomp.rom_label", title + " ROM");
        String shas = string(m, "gbarecomp.rom_sha1", "");
        romSha1 = shas.isEmpty() ? new String[0] : shas.toLowerCase(Locale.US).split(",");
        romSize = Long.parseLong(string(m, "gbarecomp.rom_size", "0"));
        romFile = string(m, "gbarecomp.rom_file", "game.gba");
        biosRequired = Boolean.parseBoolean(string(m, "gbarecomp.bios_required", "false"));
        biosSha1 = string(m, "gbarecomp.bios_sha1",
            "300c20df6731a33952ded8c436f7f186d25d3492").toLowerCase(Locale.US);
        orientation = string(m, "gbarecomp.orientation", "landscape");
        payloadVersion = string(m, "gbarecomp.payload_version", "1");
        setupNote = string(m, "gbarecomp.setup_note", "");
        showGyroStatus = Boolean.parseBoolean(string(m, "gbarecomp.gyro", "false"));
    }

    private static String string(Bundle m, String key, String fallback) {
        if (m == null) return fallback;
        Object value = m.get(key);
        return value == null ? fallback : String.valueOf(value);
    }

    public static GbaGameConfig load(Context context) {
        try {
            ApplicationInfo info = context.getPackageManager().getApplicationInfo(
                context.getPackageName(), PackageManager.GET_META_DATA);
            return new GbaGameConfig(info.metaData);
        } catch (PackageManager.NameNotFoundException error) {
            return new GbaGameConfig(null);
        }
    }

    public boolean romAccepted(String sha1) {
        for (String accepted : romSha1) {
            if (accepted.trim().equals(sha1)) return true;
        }
        return false;
    }
}
