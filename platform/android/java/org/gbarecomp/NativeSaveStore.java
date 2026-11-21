package org.gbarecomp;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;

/** Raw cartridge-save exchange; never reads or converts execution snapshots. */
public final class NativeSaveStore {
    private static final Object SESSION = new Object();
    private static int activeGames;
    private final File save;
    private final File backup;
    private final File suspendMarker;
    private final int bytes;

    public NativeSaveStore(File root, String relativePath, int expectedBytes,
                           String romFile) throws IOException {
        if (expectedBytes <= 0 || expectedBytes > 16 * 1024 * 1024)
            throw new IOException("This game has no supported native-save size.");
        save = privateFile(root, relativePath);
        backup = privateFile(root, relativePath + ".previous.sav");
        int dot = romFile.lastIndexOf('.');
        String stem = dot < 0 ? romFile : romFile.substring(0, dot);
        suspendMarker = privateFile(root, "roms/" + stem + ".suspend.pending");
        bytes = expectedBytes;
    }

    private static File privateFile(File root, String relative) throws IOException {
        if (relative.isEmpty() || relative.startsWith("/") || relative.contains("\\") ||
                relative.contains(":") || relative.startsWith("../") ||
                relative.contains("/../") || relative.endsWith("/.."))
            throw new IOException("Invalid private save location.");
        File file = new File(root, relative);
        String base = root.getCanonicalPath() + File.separator;
        if (!file.getCanonicalPath().startsWith(base) || Files.isSymbolicLink(file.toPath()))
            throw new IOException("Save location is outside private game storage.");
        return file;
    }

    public static void gameStarted() { synchronized (SESSION) { ++activeGames; } }
    public static void gameStopped() { synchronized (SESSION) { --activeGames; } }
    public static boolean gameRunning() { synchronized (SESSION) { return activeGames != 0; } }

    private static void requireStopped() throws IOException {
        if (activeGames != 0)
            throw new IOException("Quit the game before importing or exporting a saved game.");
    }

    public boolean hasSave() { return save.isFile(); }
    public boolean hasBackup() { return backup.isFile(); }

    private void copyExact(InputStream input, FileOutputStream output) throws IOException {
        if (input == null) throw new IOException("Android could not open the selected save.");
        byte[] buffer = new byte[8192];
        int total = 0;
        while (true) {
            // Read at most one extra byte, even from an unbounded provider.
            int read = input.read(buffer, 0, Math.min(buffer.length, bytes - total + 1));
            if (read < 0) break;
            if (read == 0) continue;
            total += read;
            if (total > bytes) throw wrongSize();
            output.write(buffer, 0, read);
        }
        if (total != bytes) throw wrongSize();
        output.getFD().sync();
    }

    private IOException wrongSize() {
        return new IOException("Choose a raw .sav file of exactly " + bytes +
            " bytes. Save states and emulator containers cannot be imported.");
    }

    public void importSave(InputStream input) throws IOException {
        File parent = save.getParentFile();
        File staged = null;
        try {
            // Own the provider stream and close it BEFORE touching progress.
            // A provider's close can report an I/O failure after its last read.
            try (InputStream source = input) {
                synchronized (SESSION) { requireStopped(); }
                if (!parent.isDirectory() && !parent.mkdirs())
                    throw new IOException("Could not create the private save directory.");
                staged = File.createTempFile("save-import-", ".tmp", parent);
                try (FileOutputStream output = new FileOutputStream(staged)) {
                    copyExact(source, output);
                }
            }
            synchronized (SESSION) {
                requireStopped();
                if (save.exists()) {
                    File previous = File.createTempFile("save-backup-", ".tmp", parent);
                    try {
                        try (InputStream old = new FileInputStream(save);
                             FileOutputStream output = new FileOutputStream(previous)) {
                            copyExact(old, output);
                        }
                        replace(previous, backup);
                    } finally { Files.deleteIfExists(previous.toPath()); }
                }
                // A suspended execution must not restore the old battery image.
                // Snapshots are disposable; the native save is replaced last.
                Files.deleteIfExists(suspendMarker.toPath());
                replace(staged, save);
            }
        } finally {
            if (staged != null) Files.deleteIfExists(staged.toPath());
        }
    }

    private static void replace(File source, File destination) throws IOException {
        // Same-directory atomic rename or fail with the original intact.
        Files.move(source.toPath(), destination.toPath(),
            StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
    }

    public byte[] exportSave(boolean previous) throws IOException {
        synchronized (SESSION) {
            requireStopped();
            File source = previous ? backup : save;
            if (!source.isFile() || source.length() != bytes) throw wrongSize();
            ByteArrayOutputStream snapshot = new ByteArrayOutputStream(bytes);
            try (InputStream input = new FileInputStream(source)) {
                byte[] buffer = new byte[8192];
                int read;
                while ((read = input.read(buffer)) >= 0) {
                    if (snapshot.size() + read > bytes) throw wrongSize();
                    snapshot.write(buffer, 0, read);
                }
            }
            if (snapshot.size() != bytes) throw wrongSize();
            return snapshot.toByteArray();
        }
    }
}
