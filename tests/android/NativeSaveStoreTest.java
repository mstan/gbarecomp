package org.gbarecomp;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

/** Runs with a plain JDK; all data belongs to the disposable test directory. */
public final class NativeSaveStoreTest {
    private static int checks;
    private static void check(boolean condition, String label) {
        ++checks;
        if (!condition) throw new AssertionError(label);
    }
    private interface Action { void run() throws Exception; }
    private static void rejects(Action action, String label) throws Exception {
        try { action.run(); } catch (IOException expected) { ++checks; return; }
        throw new AssertionError(label);
    }
    private static void same(Path file, byte[] bytes, String label) throws Exception {
        check(Arrays.equals(Files.readAllBytes(file), bytes), label);
    }
    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("gba-native-save-test-");
        try {
            final int size = 131072;
            byte[] first = new byte[size], second = new byte[size];
            for (int i = 0; i < size; ++i) {
                first[i] = (byte)(i * 17 + 3);
                second[i] = (byte)(i * 11 + 9);
            }
            NativeSaveStore store = new NativeSaveStore(root.toFile(), "saves/emerald.sav", size,
                "emerald.gba");
            Path save = root.resolve("saves/emerald.sav");
            Path backup = root.resolve("saves/emerald.sav.previous.sav");
            Path marker = root.resolve("roms/emerald.suspend.pending");
            check(!store.hasSave() && !store.hasBackup(), "fresh availability");
            rejects(() -> store.exportSave(false), "missing export");
            store.importSave(new ByteArrayInputStream(first));
            same(save, first, "first import");
            check(!store.hasBackup(), "no fabricated initial backup");
            Files.createDirectories(marker.getParent());
            Files.write(marker, new byte[] {1});
            store.importSave(new ByteArrayInputStream(second));
            same(save, second, "replacement");
            same(backup, first, "previous save recoverable");
            check(!Files.exists(marker), "old execution cannot resume over import");
            check(Arrays.equals(store.exportSave(false), second), "export current raw bytes");
            check(Arrays.equals(store.exportSave(true), first), "export previous raw bytes");
            rejects(() -> store.importSave(new ByteArrayInputStream(new byte[size - 1])), "short file");
            rejects(() -> store.importSave(new ByteArrayInputStream(new byte[size + 1])), "long file");
            rejects(() -> store.importSave(null), "missing provider stream");
            final int[] readBytes = {0};
            rejects(() -> store.importSave(new InputStream() {
                public int read() { ++readBytes[0]; return 0; }
                public int read(byte[] b, int off, int len) {
                    Arrays.fill(b, off, off + len, (byte)0); readBytes[0] += len; return len;
                }
            }), "unbounded source");
            check(readBytes[0] == size + 1, "unbounded read limited to exact size plus one");
            rejects(() -> store.importSave(new InputStream() {
                int calls;
                public int read() { return 0; }
                public int read(byte[] b, int off, int len) throws IOException {
                    if (++calls == 2) throw new IOException("provider disconnected");
                    Arrays.fill(b, off, off + len, (byte)1); return len;
                }
            }), "disconnected source");
            same(save, second, "bad inputs preserve current");
            same(backup, first, "bad inputs preserve backup");
            rejects(() -> store.importSave(new ByteArrayInputStream(first) {
                @Override public void close() throws IOException {
                    throw new IOException("document provider close failed");
                }
            }), "provider close failure");
            same(save, second, "provider close failure preserves native progress");
            same(backup, first, "provider close failure preserves previous backup");
            NativeSaveStore.gameStarted();
            try {
                rejects(() -> store.importSave(new ByteArrayInputStream(first)), "live import");
                rejects(() -> store.exportSave(false), "live export");
            } finally { NativeSaveStore.gameStopped(); }
            check(!NativeSaveStore.gameRunning(), "session released");
            rejects(() -> store.importSave(new ByteArrayInputStream(first) {
                boolean started;
                @Override public synchronized int read(byte[] b, int off, int len) {
                    if (!started) { started = true; NativeSaveStore.gameStarted(); }
                    return super.read(b, off, len);
                }
            }), "game started while document streamed");
            NativeSaveStore.gameStopped();
            same(save, second, "raced import preserves current");
            same(backup, first, "raced import preserves backup");
            Files.delete(backup); Files.createDirectory(backup);
            rejects(() -> store.importSave(new ByteArrayInputStream(first)), "backup commit failure");
            same(save, second, "failed backup prevents primary replacement");
            Files.delete(backup);
            Files.createDirectories(marker); Files.write(marker.resolve("blocked"), first);
            rejects(() -> store.importSave(new ByteArrayInputStream(first)), "resume marker failure");
            same(save, second, "marker failure preserves native progress");
            try (java.util.stream.Stream<Path> paths = Files.list(save.getParent())) {
                check(paths.noneMatch(p -> p.getFileName().toString().endsWith(".tmp")), "staging cleaned");
            }
            rejects(() -> new NativeSaveStore(root.toFile(), "../outside.sav", size, "game.gba"), "traversal");
            rejects(() -> new NativeSaveStore(root.toFile(), save.toString(), size, "game.gba"), "absolute");
            rejects(() -> new NativeSaveStore(root.toFile(), "save.sav", 0, "game.gba"), "invalid size");
            System.out.println("NativeSaveStore PASS " + checks + " checks");
        } finally {
            try (java.util.stream.Stream<Path> paths = Files.walk(root)) {
                for (Path p : (Iterable<Path>)paths.sorted(java.util.Comparator.reverseOrder())::iterator)
                    Files.delete(p);
            }
        }
    }
}
