package org.gbarecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.Icon;
import android.hardware.Sensor;
import android.hardware.SensorManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;
import java.util.Collections;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * First-launch setup shared by every gbarecomp Android game: the player picks
 * their own cartridge dump (and, optionally or when required, a GBA BIOS)
 * through the Storage Access Framework. Files are size- and SHA-1-verified,
 * then copied into app-private storage. Nothing copyrighted ships in the APK.
 */
public class GbaSetupActivity extends Activity {
    private static final int PICK_BIOS = 1001;
    private static final int PICK_ROM = 1002;
    private static final int IMPORT_SAVE = 1003;
    private static final int EXPORT_SAVE = 1004;
    private static final String SETUP_PREFERENCES = "setup";
    private static final String SKIP_LAUNCHER_ON_BOOT = "skip_launcher_on_boot";
    private static final long BIOS_SIZE = 16 * 1024;
    /** Validation/launcher-shortcut extra: start the game when ready without
     *  changing the player's skip-this-screen preference. */
    public static final String EXTRA_AUTOSTART = "org.gbarecomp.extra.AUTOSTART";
    public static final String EXTRA_MANAGE_SAVE = "org.gbarecomp.extra.MANAGE_SAVE";

    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private GbaGameConfig config;
    private TextView biosStatus;
    private TextView romStatus;
    private TextView infoStatus;
    private Button playButton;
    private Button biosButton;
    private Button romButton;
    private CheckBox skipLauncherCheck;
    private boolean biosReady;
    private boolean romReady;
    private NativeSaveStore saves;
    private TextView saveStatus;
    private Button importSaveButton;
    private Button exportSaveButton;
    private Button exportBackupButton;
    private boolean busy;

    /** The Activity that runs the game; games may override. */
    protected Class<?> gameActivityClass() {
        return GbaGameActivity.class;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        config = GbaGameConfig.load(this);
        if (!config.saveFile.isEmpty() && config.saveBytes > 0) {
            try {
                saves = new NativeSaveStore(getFilesDir(), config.saveFile,
                    config.saveBytes, config.romFile);
            } catch (IOException error) {
                throw new IllegalStateException("Invalid native-save configuration", error);
            }
        }
        setRequestedOrientation(GbaGameActivity.orientationFor(config.orientation));
        boolean manageSave = getIntent().getBooleanExtra(EXTRA_MANAGE_SAVE, false);
        if (saves != null) installSaveShortcut();
        if (!manageSave && hasBundledPrivateAssets()) {
            startActivity(new Intent(this, gameActivityClass()));
            finish();
            return;
        }
        setContentView(buildContent());
        enterImmersiveMode();
        refreshStatus();
        if (savedInstanceState != null) {
            pendingPreviousExport = savedInstanceState.getBoolean("previous-save-export", false);
            return;
        }
        if (manageSave) return;
        if (!ready()) return;
        if (getIntent().getBooleanExtra(EXTRA_AUTOSTART, false)) {
            startActivity(new Intent(this, gameActivityClass()));
        } else if (skipLauncherOnBoot()) {
            launchGame();
        }
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        if (playButton != null) {
            setContentView(buildContent());
            refreshStatus();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        enterImmersiveMode();
        if (playButton != null) refreshStatus();
    }

    @Override
    protected void onDestroy() {
        worker.shutdownNow();
        super.onDestroy();
    }

    @Override
    protected void onSaveInstanceState(Bundle state) {
        state.putBoolean("previous-save-export", pendingPreviousExport);
        super.onSaveInstanceState(state);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) enterImmersiveMode();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        if (requestCode == IMPORT_SAVE || requestCode == EXPORT_SAVE) {
            transferSave(requestCode, data.getData());
            return;
        }
        if (requestCode != PICK_BIOS && requestCode != PICK_ROM) return;
        final boolean bios = requestCode == PICK_BIOS;
        final Uri uri = data.getData();
        setBusy(true, bios ? "Checking BIOS…" : "Checking ROM…");
        worker.execute(() -> {
            try {
                installVerifiedAsset(uri, bios);
                runOnUiThread(() -> {
                    setBusy(false, null);
                    refreshStatus();
                });
            } catch (Exception error) {
                runOnUiThread(() -> {
                    setBusy(false, null);
                    refreshStatus();
                    showError(bios ? "BIOS not accepted" : "ROM not accepted",
                        error.getMessage());
                });
            }
        });
    }

    private boolean ready() {
        return romReady && (biosReady || !config.biosRequired);
    }

    private boolean portrait() {
        return getResources().getConfiguration().orientation ==
            Configuration.ORIENTATION_PORTRAIT;
    }

    private View buildContent() {
        final boolean vertical = portrait();
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setBackgroundColor(Color.rgb(9, 11, 24));

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(Color.rgb(9, 11, 24));

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setPadding(dp(vertical ? 20 : 36), dp(vertical ? 36 : 14),
            dp(vertical ? 20 : 36), dp(10));
        scroll.addView(root, new ScrollView.LayoutParams(
            ScrollView.LayoutParams.MATCH_PARENT, ScrollView.LayoutParams.WRAP_CONTENT));
        page.addView(scroll, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));

        TextView eyebrow = text("ANDROID EDITION", 12, Color.rgb(78, 216, 255));
        eyebrow.setTypeface(Typeface.DEFAULT_BOLD);
        eyebrow.setLetterSpacing(0.16f);
        root.addView(eyebrow);

        TextView title = text(config.title, 27, Color.WHITE);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setGravity(Gravity.CENTER);
        root.addView(title, margins(-1, -2, 0, dp(2), 0, 0));

        TextView subtitle = text("Choose your own legally dumped files to begin.", 15,
            Color.rgb(177, 187, 216));
        subtitle.setGravity(Gravity.CENTER);
        root.addView(subtitle, margins(-1, -2, 0, 0, 0, dp(10)));

        LinearLayout assets = new LinearLayout(this);
        assets.setOrientation(vertical ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        assets.setGravity(Gravity.CENTER);
        root.addView(assets, margins(-1, -2, 0, 0, 0, dp(8)));

        LinearLayout romCard = assetCard("1", config.romLabel,
            "Choose your clean cartridge dump (.gba).");
        romStatus = statusText();
        romButton = actionButton("CHOOSE ROM");
        romButton.setOnClickListener(view -> pickFile(PICK_ROM));
        romCard.addView(romStatus, margins(-1, -2, 0, dp(4), 0, dp(4)));
        romCard.addView(romButton, margins(-1, dp(48), 0, 0, 0, 0));
        assets.addView(romCard, vertical ? margins(-1, -2, 0, dp(6), 0, dp(6))
                                         : weightedMargins(1, dp(8), 0, dp(8), 0));

        LinearLayout biosCard = assetCard("2",
            config.biosRequired ? "GBA BIOS" : "GBA BIOS (optional)",
            config.biosRequired
                ? "Choose your clean 16 KiB gba_bios.bin dump."
                : "Recommended for accuracy. Without it the built-in BIOS " +
                  "substitute is used.");
        biosStatus = statusText();
        biosButton = actionButton("CHOOSE BIOS");
        biosButton.setOnClickListener(view -> pickFile(PICK_BIOS));
        biosCard.addView(biosStatus, margins(-1, -2, 0, dp(4), 0, dp(4)));
        biosCard.addView(biosButton, margins(-1, dp(48), 0, 0, 0, 0));
        assets.addView(biosCard, vertical ? margins(-1, -2, 0, dp(6), 0, dp(6))
                                          : weightedMargins(1, dp(8), 0, dp(8), 0));

        if (saves != null) {
            LinearLayout card = assetCard("3", "Saved game",
                "Import or export a raw .sav file. Quit the game to return here. " +
                "Import keeps your previous save as an exportable backup.");
            saveStatus = statusText();
            card.addView(saveStatus, margins(-1, -2, 0, dp(4), 0, dp(4)));
            importSaveButton = actionButton("IMPORT SAVE…");
            importSaveButton.setOnClickListener(view -> new AlertDialog.Builder(this)
                .setTitle("Import saved game?")
                .setMessage("This replaces the game's progress with the selected .sav. " +
                    "Your current save will be kept as the previous-save backup.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Choose save", (dialog, which) -> pickFile(IMPORT_SAVE))
                .show());
            card.addView(importSaveButton, margins(-1, dp(48), 0, dp(4), 0, 0));
            exportSaveButton = actionButton("EXPORT SAVE…");
            exportSaveButton.setOnClickListener(view -> exportSave(false));
            card.addView(exportSaveButton, margins(-1, dp(48), 0, dp(4), 0, 0));
            exportBackupButton = actionButton("EXPORT PREVIOUS SAVE…");
            exportBackupButton.setOnClickListener(view -> exportSave(true));
            card.addView(exportBackupButton, margins(-1, dp(48), 0, dp(4), 0, 0));
            root.addView(card, margins(-1, -2, 0, dp(6), 0, dp(6)));
        }

        infoStatus = text("", 13, Color.rgb(177, 187, 216));
        infoStatus.setGravity(Gravity.CENTER);
        root.addView(infoStatus, margins(-1, -2, 0, 0, 0, dp(6)));

        TextView privacy = text(
            "Your files stay in this app’s private storage and are never included " +
            "in the APK." + (config.setupNote.isEmpty() ? "" : " " + config.setupNote),
            11, Color.rgb(132, 143, 173));
        privacy.setGravity(Gravity.CENTER);
        root.addView(privacy, margins(-1, -2, 0, 0, 0, 0));

        LinearLayout footer = new LinearLayout(this);
        footer.setOrientation(vertical ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        footer.setGravity(Gravity.CENTER);
        footer.setPadding(dp(vertical ? 20 : 36), dp(8), dp(vertical ? 20 : 36), dp(18));

        skipLauncherCheck = new CheckBox(this);
        skipLauncherCheck.setText("Skip this screen on launch when ready");
        skipLauncherCheck.setTextColor(Color.rgb(203, 212, 238));
        skipLauncherCheck.setTextSize(14);
        skipLauncherCheck.setMinHeight(dp(52));
        skipLauncherCheck.setChecked(skipLauncherOnBoot());
        skipLauncherCheck.setOnCheckedChangeListener((button, checked) ->
            getSharedPreferences(SETUP_PREFERENCES, MODE_PRIVATE)
                .edit().putBoolean(SKIP_LAUNCHER_ON_BOOT, checked).apply());
        footer.addView(skipLauncherCheck, vertical
            ? margins(-2, -2, 0, 0, 0, dp(8)) : weightedMargins(1, 0, 0, dp(18), 0));

        playButton = actionButton("PLAY");
        playButton.setTextSize(18);
        playButton.setTypeface(Typeface.DEFAULT_BOLD);
        playButton.setOnClickListener(view -> launchGame());
        footer.addView(playButton, margins(vertical ? -1 : dp(360), dp(58), 0, 0, 0, 0));

        page.addView(footer, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        return page;
    }

    private LinearLayout assetCard(String number, String title, String body) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(20), dp(12), dp(20), dp(12));
        card.setBackground(roundRect(Color.rgb(20, 25, 48), 18, Color.rgb(48, 58, 92)));
        TextView step = text(number + "  " + title, 17, Color.WHITE);
        step.setTypeface(Typeface.DEFAULT_BOLD);
        card.addView(step);
        TextView description = text(body, 13, Color.rgb(164, 175, 207));
        card.addView(description, margins(-1, -2, 0, dp(4), 0, 0));
        return card;
    }

    private TextView statusText() {
        TextView status = text("Checking…", 13, Color.rgb(255, 211, 78));
        status.setTypeface(Typeface.DEFAULT_BOLD);
        return status;
    }

    private Button actionButton(String label) {
        Button button = new Button(this);
        button.setText(label);
        button.setTextColor(Color.rgb(8, 16, 27));
        button.setTextSize(13);
        button.setTypeface(Typeface.DEFAULT_BOLD);
        button.setAllCaps(false);
        button.setGravity(Gravity.CENTER);
        button.setPadding(dp(16), 0, dp(16), 0);
        button.setBackground(roundRect(Color.rgb(78, 216, 255), 14, Color.TRANSPARENT));
        return button;
    }

    private TextView text(String value, float size, int color) {
        TextView text = new TextView(this);
        text.setText(value);
        text.setTextSize(size);
        text.setTextColor(color);
        text.setLineSpacing(0, 1.12f);
        return text;
    }

    private File biosFile() { return new File(getFilesDir(), "bios/gba_bios.bin"); }
    private File romFile() { return new File(getFilesDir(), "roms/" + config.romFile); }

    private void refreshStatus() {
        biosReady = biosFile().isFile() && biosFile().length() == BIOS_SIZE;
        romReady = romFile().isFile() && romFile().length() == config.romSize;
        setAssetStatus(romStatus, romButton, romReady, "ROM", true);
        setAssetStatus(biosStatus, biosButton, biosReady, "BIOS", config.biosRequired);
        playButton.setEnabled(ready() && !busy);
        biosButton.setEnabled(!busy);
        romButton.setEnabled(!busy);
        playButton.setAlpha(playButton.isEnabled() ? 1.0f : 0.35f);
        refreshSaveStatus();

        if (config.showGyroStatus) {
            SensorManager manager = (SensorManager) getSystemService(SENSOR_SERVICE);
            Sensor gyro = manager == null ? null
                : manager.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
            infoStatus.setText(gyro != null
                ? "●  Device gyroscope ready  ·  " + gyro.getName()
                : "Device gyroscope unavailable — controller motion still works.");
            infoStatus.setTextColor(gyro != null ? Color.rgb(120, 235, 171)
                                                 : Color.rgb(255, 184, 104));
        } else {
            infoStatus.setText("");
        }
    }

    private void setAssetStatus(TextView status, Button button, boolean ready,
                                String kind, boolean required) {
        status.setText(ready ? "●  Verified and ready"
                             : (required ? "○  " + kind + " required" : "○  Not provided"));
        status.setTextColor(ready ? Color.rgb(120, 235, 171)
                                  : (required ? Color.rgb(255, 211, 78)
                                              : Color.rgb(164, 175, 207)));
        button.setText(ready ? "REPLACE " + kind : "CHOOSE " + kind);
    }

    private void pickFile(int requestCode) {
        if (requestCode == IMPORT_SAVE && NativeSaveStore.gameRunning()) {
            showError("Game is running", "Quit the game before importing a saved game.");
            return;
        }
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        // DocumentsUI often gives .bin/.gba vendor-specific MIME types; filter
        // nothing and let the strict size + SHA-1 check decide.
        intent.setType("*/*");
        startActivityForResult(intent, requestCode);
    }

    private void refreshSaveStatus() {
        if (saves == null || saveStatus == null) return;
        boolean available = !busy && !NativeSaveStore.gameRunning();
        importSaveButton.setEnabled(available);
        exportSaveButton.setEnabled(available && saves.hasSave());
        exportBackupButton.setEnabled(available && saves.hasBackup());
        exportBackupButton.setVisibility(saves.hasBackup() ? View.VISIBLE : View.GONE);
        saveStatus.setText(NativeSaveStore.gameRunning() ? "Quit the game to manage saves."
            : saves.hasSave() ? "Saved game available" : "No saved game yet");
        saveStatus.setTextColor(Color.rgb(177, 187, 216));
    }

    private void installSaveShortcut() {
        // A player who skips setup can still manage saves from the normal
        // Android app-icon menu, without hunting for private storage.
        ShortcutManager manager = getSystemService(ShortcutManager.class);
        if (manager == null) return;
        try {
            Intent intent = new Intent(this, getClass()).setAction(Intent.ACTION_VIEW)
                .putExtra(EXTRA_MANAGE_SAVE, true);
            manager.addDynamicShortcuts(Collections.singletonList(
                new ShortcutInfo.Builder(this, "saved-game")
                    .setShortLabel("Saved game")
                    .setLongLabel("Import or export saved game")
                    .setIcon(Icon.createWithResource(this, android.R.drawable.ic_menu_save))
                    .setIntent(intent).build()));
        } catch (IllegalArgumentException | IllegalStateException unavailable) {
            // Some launchers do not support shortcuts. The same controls remain
            // on setup when the player quits the game.
        }
    }

    private void exportSave(boolean previous) {
        try {
            // Validate the selected source now; copy again after the picker so
            // no Activity field or saved-instance Bundle holds player data.
            saves.exportSave(previous);
            Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("application/octet-stream");
            intent.putExtra(Intent.EXTRA_TITLE, config.romFile.replaceFirst("\\.[^.]+$", "") +
                (previous ? "-previous.sav" : ".sav"));
            pendingPreviousExport = previous;
            startActivityForResult(intent, EXPORT_SAVE);
        } catch (IOException error) {
            showError("Save not exported", error.getMessage());
        }
    }

    private boolean pendingPreviousExport;

    private void transferSave(int requestCode, Uri uri) {
        final boolean previous = pendingPreviousExport;
        setBusy(true, requestCode == IMPORT_SAVE ? "Importing saved game…" : "Exporting saved game…");
        worker.execute(() -> {
            try {
                if (requestCode == IMPORT_SAVE) {
                    saves.importSave(getContentResolver().openInputStream(uri));
                } else {
                    byte[] snapshot = saves.exportSave(previous);
                    try (OutputStream output = getContentResolver().openOutputStream(uri, "wt")) {
                        if (output == null) throw new IOException("Android could not create the export.");
                        output.write(snapshot);
                        output.flush();
                    }
                }
                runOnUiThread(() -> {
                    if (isFinishing() || isDestroyed()) return;
                    setBusy(false, null);
                    refreshStatus();
                    showError(requestCode == IMPORT_SAVE ? "Saved game imported" : "Saved game exported",
                        requestCode == IMPORT_SAVE ? "Play will start from your imported progress."
                            : "Your .sav file was saved to the location you chose.");
                });
            } catch (Exception error) {
                runOnUiThread(() -> {
                    if (isFinishing() || isDestroyed()) return;
                    setBusy(false, null);
                    refreshStatus();
                    showError("Save transfer failed", error.getMessage());
                });
            }
        });
    }

    private void launchGame() {
        if (!ready() || busy) return;
        getSharedPreferences(SETUP_PREFERENCES, MODE_PRIVATE).edit()
            .putBoolean(SKIP_LAUNCHER_ON_BOOT,
                skipLauncherCheck == null || skipLauncherCheck.isChecked())
            .apply();
        startActivity(new Intent(this, gameActivityClass()));
    }

    private boolean skipLauncherOnBoot() {
        return getSharedPreferences(SETUP_PREFERENCES, MODE_PRIVATE)
            .getBoolean(SKIP_LAUNCHER_ON_BOOT, true);
    }

    private boolean hasBundledPrivateAssets() {
        try (InputStream rom = getAssets().open("payload/roms/" + config.romFile)) {
            if (rom.read() < 0) return false;
        } catch (IOException ignored) {
            return false;
        }
        if (!config.biosRequired) return true;
        try (InputStream bios = getAssets().open("payload/bios/gba_bios.bin")) {
            return bios.read() >= 0;
        } catch (IOException ignored) {
            return false;
        }
    }

    private void installVerifiedAsset(Uri uri, boolean bios)
            throws IOException, NoSuchAlgorithmException {
        String displayName = displayName(uri);
        long expectedSize = bios ? BIOS_SIZE : config.romSize;
        File destination = bios ? biosFile() : romFile();
        File directory = destination.getParentFile();
        if (directory != null && !directory.isDirectory() && !directory.mkdirs()) {
            throw new IOException("Could not create the private game directory.");
        }
        File temporary = File.createTempFile("import-", ".tmp", directory);
        MessageDigest digest = MessageDigest.getInstance("SHA-1");
        long total = 0;
        try (InputStream input = getContentResolver().openInputStream(uri);
             FileOutputStream output = new FileOutputStream(temporary)) {
            if (input == null) throw new IOException("Android could not open " + displayName + ".");
            byte[] buffer = new byte[1024 * 1024];
            int read;
            while ((read = input.read(buffer)) >= 0) {
                if (read == 0) continue;
                total += read;
                if (total > expectedSize) throw new IOException(displayName + " is larger than expected.");
                digest.update(buffer, 0, read);
                output.write(buffer, 0, read);
            }
            output.getFD().sync();
        } catch (IOException error) {
            temporary.delete();
            throw error;
        }
        String actual = toHex(digest.digest());
        boolean accepted = total == expectedSize &&
            (bios ? config.biosSha1.equals(actual) : config.romAccepted(actual));
        if (!accepted) {
            temporary.delete();
            throw new IOException(String.format(Locale.US,
                "%s is not the supported clean %s dump.\n\nSize: %,d bytes (expected %,d)\nSHA-1: %s",
                displayName, bios ? "GBA BIOS" : config.romLabel, total, expectedSize, actual));
        }
        if (destination.exists() && !destination.delete()) {
            temporary.delete();
            throw new IOException("Could not replace the previously imported file.");
        }
        if (!temporary.renameTo(destination)) {
            copyFile(temporary, destination);
            temporary.delete();
        }
    }

    private static void copyFile(File source, File destination) throws IOException {
        try (FileInputStream input = new FileInputStream(source);
             FileOutputStream output = new FileOutputStream(destination)) {
            byte[] buffer = new byte[1024 * 1024];
            int read;
            while ((read = input.read(buffer)) >= 0) {
                if (read > 0) output.write(buffer, 0, read);
            }
            output.getFD().sync();
        }
    }

    private String displayName(Uri uri) {
        try (android.database.Cursor cursor = getContentResolver().query(
                uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (column >= 0) return cursor.getString(column);
            }
        } catch (Exception ignored) {
        }
        return "selected file";
    }

    private void setBusy(boolean busy, String label) {
        this.busy = busy;
        biosButton.setEnabled(!busy);
        romButton.setEnabled(!busy);
        playButton.setEnabled(!busy && ready());
        refreshSaveStatus();
        if (busy && label != null) {
            infoStatus.setText(label);
            infoStatus.setTextColor(Color.rgb(78, 216, 255));
        }
    }

    private void showError(String title, String message) {
        new AlertDialog.Builder(this)
            .setTitle(title)
            .setMessage(message == null ? "The selected file could not be read." : message)
            .setPositiveButton("OK", null)
            .show();
    }

    private void enterImmersiveMode() {
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                controller.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    private GradientDrawable roundRect(int color, int radiusDp, int strokeColor) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(color);
        drawable.setCornerRadius(dp(radiusDp));
        if (strokeColor != Color.TRANSPARENT) drawable.setStroke(dp(1), strokeColor);
        return drawable;
    }

    private LinearLayout.LayoutParams margins(int width, int height, int left, int top,
                                               int right, int bottom) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(width, height);
        params.setMargins(left, top, right, bottom);
        return params;
    }

    private LinearLayout.LayoutParams weightedMargins(float weight, int left, int top,
                                                       int right, int bottom) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
            0, LinearLayout.LayoutParams.WRAP_CONTENT, weight);
        params.setMargins(left, top, right, bottom);
        return params;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private static String toHex(byte[] bytes) {
        StringBuilder value = new StringBuilder(bytes.length * 2);
        for (byte item : bytes) value.append(String.format(Locale.US, "%02x", item & 0xff));
        return value.toString();
    }
}
