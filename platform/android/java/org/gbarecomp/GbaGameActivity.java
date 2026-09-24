package org.gbarecomp;

import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.graphics.Insets;
import android.os.Build;
import android.os.Bundle;
import android.view.DisplayCutout;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/**
 * The shared game Activity: installs the packaged payload (game TOML, mod
 * catalog, optional private assets) into private storage, runs immersive and
 * edge-to-edge (drawing under display cutouts), follows the game's
 * orientation policy with live rotation, and reports safe-area insets to the
 * runtime so host chrome avoids cutouts and system-gesture edges.
 */
public class GbaGameActivity extends SDLActivity {
    private GbaGameConfig config;
    private boolean nativeReady;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        config = GbaGameConfig.load(this);
        setRequestedOrientation(orientationFor(config.orientation));
        try {
            installPayload();
        } catch (IOException error) {
            throw new IllegalStateException("Unable to install game payload", error);
        }
        super.onCreate(savedInstanceState);
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams attributes = getWindow().getAttributes();
            attributes.layoutInDisplayCutoutMode = Build.VERSION.SDK_INT >= 30
                ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(attributes);
        }
        View decor = getWindow().getDecorView();
        decor.setOnApplyWindowInsetsListener((view, insets) -> {
            reportInsets(insets);
            return view.onApplyWindowInsets(insets);
        });
        enterImmersiveMode();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            enterImmersiveMode();
            View decor = getWindow().getDecorView();
            WindowInsets insets = decor.getRootWindowInsets();
            if (insets != null) reportInsets(insets);
        }
    }

    static int orientationFor(String policy) {
        if ("any".equals(policy)) return ActivityInfo.SCREEN_ORIENTATION_FULL_USER;
        if ("portrait".equals(policy)) return ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT;
        return ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE;
    }

    private void reportInsets(WindowInsets insets) {
        int left = 0, top = 0, right = 0, bottom = 0;
        if (Build.VERSION.SDK_INT >= 28) {
            DisplayCutout cutout = insets.getDisplayCutout();
            if (cutout != null) {
                left = cutout.getSafeInsetLeft();
                top = cutout.getSafeInsetTop();
                right = cutout.getSafeInsetRight();
                bottom = cutout.getSafeInsetBottom();
            }
        }
        if (Build.VERSION.SDK_INT >= 29) {
            // Edge swipes starting here belong to the system (home/back).
            Insets gestures = insets.getMandatorySystemGestureInsets();
            left = Math.max(left, gestures.left);
            top = Math.max(top, gestures.top);
            right = Math.max(right, gestures.right);
            bottom = Math.max(bottom, gestures.bottom);
        }
        try {
            GbaNative.setSafeInsets(left, top, right, bottom);
            nativeReady = true;
        } catch (UnsatisfiedLinkError notLoadedYet) {
            // libmain loads during super.onCreate; later passes succeed.
            nativeReady = false;
        }
    }

    protected void enterImmersiveMode() {
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.statusBars()
                    | WindowInsets.Type.navigationBars());
                controller.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    /**
     * Copies assets/payload into getFilesDir() once per payload version.
     * Player data (saves, config.ini, suspend states, imported ROMs) lives
     * beside the payload and is never overwritten: only files that exist in
     * the payload are replaced, and the payload never contains player data.
     */
    private void installPayload() throws IOException {
        File marker = new File(getFilesDir(), ".payload-version");
        if (marker.isFile()) {
            byte[] version = new byte[(int) marker.length()];
            try (FileInputStream input = new FileInputStream(marker)) {
                int offset = 0;
                while (offset < version.length) {
                    int read = input.read(version, offset, version.length - offset);
                    if (read < 0) break;
                    offset += read;
                }
            }
            String installed = new String(version, StandardCharsets.UTF_8).trim();
            if (config.payloadVersion.equals(installed)) return;
        }
        copyAssetTree(getAssets(), "payload", getFilesDir());
        try (FileOutputStream output = new FileOutputStream(marker)) {
            output.write((config.payloadVersion + "\n").getBytes(StandardCharsets.UTF_8));
        }
    }

    private static void copyAssetTree(AssetManager assets, String assetPath,
                                      File destination) throws IOException {
        String[] children = assets.list(assetPath);
        if (children != null && children.length > 0) {
            if (!destination.isDirectory() && !destination.mkdirs()) {
                throw new IOException("Unable to create " + destination);
            }
            for (String child : children) {
                copyAssetTree(assets, assetPath + "/" + child, new File(destination, child));
            }
            return;
        }
        File parent = destination.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("Unable to create " + parent);
        }
        // Player-owned files next to the payload are never replaced.
        if (destination.isFile() && isPlayerOwned(destination.getName())) return;
        try (InputStream input = assets.open(assetPath);
             FileOutputStream output = new FileOutputStream(destination)) {
            byte[] buffer = new byte[1024 * 1024];
            int read;
            while ((read = input.read(buffer)) >= 0) {
                if (read > 0) output.write(buffer, 0, read);
            }
        }
    }

    /** Files the player (or the running game) owns once they exist. */
    private static boolean isPlayerOwned(String name) {
        return name.equals("state.toml") || name.endsWith(".sav") ||
               name.equals("config.ini") || name.equals("keybinds.ini");
    }
}
