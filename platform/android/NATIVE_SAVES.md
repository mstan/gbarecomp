# Native saved games on Android

Games opt in with `saveFile` (a path relative to Android's private files
directory) and `saveBytes` (the exact raw cartridge-save size) in `gbaGame`.
Keep those values aligned with the Android game TOML's `[save]` section.
Set `RunOptions::launcher_native_save_managed_externally` for that Android
entry point so the legacy launcher SRAM Import/Clear row stays absent.

The launch screen shows **Saved game**, with Import and Export buttons.
Quit the game to return to that screen. A launcher that supports app shortcuts
also shows **Saved game** when the player presses and holds the app icon; this
opens setup without changing the player's skip-setup preference.

Android's document picker chooses the input or export destination. No storage
permission or private-directory browsing is needed. Import accepts raw `.sav`
data, not emulator containers or execution snapshots. It validates the exact
size before committing, keeps the old save as an exportable previous-save
backup and clears pending automatic execution resume. Import/export is locked
while the native game thread is alive, including while backgrounded.

`NativeSaveStore` stages and syncs input before a same-directory atomic rename.
Backup or commit failure preserves the original native save. There is no
non-atomic replacement fallback. The plain-JDK regression can be run with:

```
javac --release 8 -d <temporary-classes> platform/android/java/org/gbarecomp/NativeSaveStore.java tests/android/NativeSaveStoreTest.java
java -cp <temporary-classes> org.gbarecomp.NativeSaveStoreTest
```
