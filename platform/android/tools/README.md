# Android platform tools

PowerShell tooling for building and validating gbarecomp Android games
(the shared template in `platform/android/`, applied by each game's own
`android/` Gradle project). Requires the Android SDK (`ANDROID_HOME` /
`ANDROID_SDK_ROOT`, `adb` on `PATH`) and JDK 17 (`JAVA_HOME`).

Examples below use `EmeraldRecomp-android-touch` (applicationId
`com.mstan.emeraldrecomp`).

## build-apk.ps1

Builds a game's APK via its own `gradlew.bat`, at BelowNormal process
priority so it does not compete with other work on the machine. Forwards
the `gbarecomp-app.gradle` properties (`-PgbarecompRoot`, `-PrecompUiRoot`,
`-PgbaAbis`, `-PgbaNativeJobs`, `-PprivateRom`, `-PprivateBios`) and prints
the resulting APK path(s) and size. Exits with gradle's own exit code.

```powershell
# Debug build, default ABIs (arm64-v8a,x86_64), engine/UI roots as pinned
# by the game's settings/build.gradle (or GBARECOMP_ROOT/RECOMP_UI_ROOT env).
.\build-apk.ps1 -GameAndroidDir F:\Projects\gbarecomp\EmeraldRecomp-android-touch\android

# Private build embedding a verified ROM + BIOS dump (never distribute the
# resulting APK -- see gbarecomp-app.gradle's verifyNoPrivateAssets guard).
.\build-apk.ps1 -GameAndroidDir F:\Projects\gbarecomp\EmeraldRecomp-android-touch\android `
    -PrivateRom F:\roms\emerald_usa.gba -PrivateBios F:\bios\gba_bios.bin

# Release build, x86_64-only (fast emulator iteration), custom engine worktree.
.\build-apk.ps1 -GameAndroidDir F:\Projects\gbarecomp\EmeraldRecomp-android-touch\android `
    -Release -Abis x86_64 -EngineRoot F:\Projects\gbarecomp\gbarecomp-wt-cosim -Jobs 4
```

## android-validate.ps1

Drives an installed game on a device/emulator: optionally installs an APK,
stages a ROM/BIOS into the app's private storage, launches it, walks a
rotation sequence with a screenshot at each step, pulls diagnostic files
out of app-private storage, and checks Android's crash ring buffer for
fatal signals belonging to the package. Emits `summary.json` in `-OutDir`
(default a fresh timestamped folder under
`$env:TEMP\gbarecomp-android-validate\`).

Crash detection reads `adb logcat -b crash -d` as-is -- it never clears
the buffer first, so a run that starts mid-session still sees whatever
crashed before this script attached.

```powershell
# Validate whatever is already running (or on screen) -- no install, no launch.
.\android-validate.ps1 -Package com.mstan.emeraldrecomp `
    -Rotations 'portrait,landscape,portrait' -PullArtifacts

# Full pass: install, stage assets, launch, rotate through all four
# orientations, forward the runtime's debug port, pull artifacts.
.\android-validate.ps1 -Package com.mstan.emeraldrecomp `
    -Apk F:\Projects\gbarecomp\EmeraldRecomp-android-touch\android\app\build\outputs\apk\debug\app-debug.apk `
    -Rom F:\roms\emerald_usa.gba -Bios F:\bios\gba_bios.bin `
    -Launch -Rotations 'portrait,landscape,reverse-landscape,portrait' `
    -ObservePort 19892 -PullArtifacts

# Target a specific device when more than one is attached.
.\android-validate.ps1 -Package com.mstan.emeraldrecomp -Serial emulator-5554 -PullArtifacts
```

Notes:
- `-Rom`'s local filename is trusted as the on-device destination name
  (`files/roms/<basename>`) -- it must match what the game's manifest
  expects (e.g. `emerald_usa.gba` for Emerald), since that isn't otherwise
  discoverable over adb from an installed APK. `-Bios` always stages to
  the fixed `files/bios/gba_bios.bin`.
- Staging is skipped when a same-size file is already present on-device.
- All app-private file pulls go through `adb exec-out run-as <pkg> cat`
  piped into a real `cmd.exe` redirection (not PowerShell's `>`, which
  mangles binary output such as screenshots or `.frag`/`.json` files with
  non-ASCII bytes); each pull's byte size is verified against the
  on-device size.
- `accelerometer_rotation` (and `user_rotation`) are restored to their
  original values when the script exits, including on failure.

## avd-bootstrap.ps1

Ensures the AVDs used for phone + tablet validation exist, with host-GPU
acceleration and enough RAM. Idempotent: skips creation if the AVD is
already registered.

```powershell
# Create the tablet AVD (gbarecomp_tablet_api35) if it doesn't exist yet.
.\avd-bootstrap.ps1

# Also fix Pixel_7_API_35's GPU/RAM settings (off by default on that AVD).
.\avd-bootstrap.ps1 -FixPhone

# Create (if needed) and boot the tablet AVD, waiting for sys.boot_completed.
.\avd-bootstrap.ps1 -Start gbarecomp_tablet_api35
```

Do not pass `-Start` while another emulator instance you still need is
running unless you intend to run two side by side -- it launches a new
instance rather than reusing one.
