# Windows game setup guide

This guide is for Windows 11 users who are new to GBARecomp.

The most important thing:

**`gbarecomp.exe build` does not make a playable `.exe` game.**

It makes generated C++ code. When you run `build.ps1`, it builds a file named
`gbarecomp_game.lib`. That is a library for developers. It is not a game you
can double-click.

To get a playable game `.exe`, use a game project, for example
`MinishCapRecomp`, `MegaManZeroRecomp`, or another public game repo. The game
project adds the window, sound, controls, saves, launcher, and ROM checking.

## What the files mean

After this command:

```powershell
.\gbarecomp.exe build --rom "C:\Games\MyGame.gba" --output "C:\Projects\MyGameRecomp"
```

you should see a folder like this:

```text
MyGameRecomp
  build.ps1
  CMakeLists.txt
  generated
    recompiled_000.cpp
    recompiled_001.cpp
    dispatch_table.cpp
    recompiled.h
```

This means source generation worked.

After this command:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\Projects\MyGameRecomp\build.ps1"
```

you should see:

```text
MyGameRecomp\build\Release\gbarecomp_game.lib
```

This means the generated code compiled.

It is normal that there is no `MyGameRecomp.exe` here.

## If PowerShell blocks `build.ps1`

Windows may say:

```text
cannot be loaded because running scripts is disabled on this system
```

Use this command instead:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\Projects\MyGameRecomp\build.ps1"
```

This only bypasses the rule for this one command.

## If PowerShell cannot find `gbarecomp.exe`

If you are inside the folder with `gbarecomp.exe`, run it with `.\`:

```powershell
.\gbarecomp.exe --version
```

PowerShell does not run programs from the current folder unless you write
`.\` first.

## If CMake uses the wrong Windows tool

Some Windows setups have an MSYS or devkitPro `cmake.exe` first on `PATH`. That
can fail with paths like `C:\...`.

For the generated project, prefer `build.ps1`. It tries to find the native
CMake that comes with Visual Studio.

If you build GBARecomp from source yourself, use Visual Studio's CMake directly:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build-vs -A x64
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build-vs --config Release --target gba_recompile
```

Your Visual Studio folder may be `BuildTools` instead of `Community`.

## The short path: use a public game release

Use this path if you want to play a recompiled game.

1. Open that game's GitHub Releases page.
2. Download the Windows zip.
3. Extract the whole zip.
4. Run the game `.exe`.
5. When it asks, choose your legally dumped ROM.

This is the easiest path. You do not need to build GBARecomp yourself.

## Build a public game from source

Use this path if you want to create the playable game `.exe` yourself.

1. Install Git.
2. Install Visual Studio 2022 with "Desktop development with C++".
3. Install MSYS2.
4. In the MSYS2 MINGW64 shell, install the build tools:

```sh
pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-SDL2
```

5. Clone the game repo. Example:

```powershell
cd "C:\Projects"
git clone --recurse-submodules https://github.com/mstan/MinishCapRecomp.git
cd "C:\Projects\MinishCapRecomp"
```

6. Put your legally dumped Minish Cap USA ROM here:

```text
C:\Projects\MinishCapRecomp\roms\minishcap_usa.gba
```

The ROM's SHA-1 must match the value in that game's README.

7. Build the game's pinned `gba_recompile.exe`:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S ".\gbarecomp" -B ".\gbarecomp\build-vs" -A x64
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build ".\gbarecomp\build-vs" --config Release --target gba_recompile
```

8. Generate the game's C++:

```powershell
& ".\gbarecomp\build-vs\Release\gba_recompile.exe" `
  --rom "roms\minishcap_usa.gba" `
  --config "symbols\minishcap.toml" `
  --symbols "symbols\imported_symbols.tsv" `
  --out generated
```

9. Build the playable game exe:

```powershell
& "C:\msys64\mingw64\bin\cmake.exe" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
& "C:\msys64\mingw64\bin\cmake.exe" --build build --target MinishCapRecomp --parallel
```

10. The exe is here:

```text
C:\Projects\MinishCapRecomp\build\MinishCapRecomp.exe
```

Different game repos may use different ROM names, config files, or target
names. Always check that game's README.

## Build GBARecomp itself

Use this path if you want to build the GBARecomp tool from source. This builds
the recompiler tool, not a game.

1. Install Git.
2. Install Visual Studio 2022 with "Desktop development with C++".
3. Clone GBARecomp:

```powershell
cd "C:\Projects"
git clone --recurse-submodules https://github.com/mstan/gbarecomp.git
cd "C:\Projects\gbarecomp"
```

4. Build `gba_recompile.exe`:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build-vs -A x64
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build-vs --config Release --target gba_recompile
```

The tool exe is here:

```text
C:\Projects\gbarecomp\build-vs\Release\gba_recompile.exe
```

Do not put ROM files in Git. Public repos must not include ROMs.

## The CLI path: generate code for a new game

Use this path if you are starting research on a new GBA game.

1. Download `gbarecomp-cli-windows-x86_64.zip` from the GBARecomp releases page.
2. Extract the whole zip. Do not run it from inside the zip viewer.
3. Open PowerShell in the extracted folder.
4. Run:

```powershell
.\gbarecomp.exe build `
  --rom "C:\Games\GEMini.gba" `
  --output "C:\Projects\GEMiniRecomp"
```

5. Build the generated library:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\Projects\GEMiniRecomp\build.ps1"
```

If you see `gbarecomp_game.lib`, this step is done.

For a brand-new game, the next work is still developer work:

- make a game repo;
- add a real `main.cpp`;
- link the generated library to the GBARecomp runtime;
- add `game.toml` settings when the automatic scan is not enough;
- verify the exact ROM hash;
- test boot, input, sound, saves, and crashes.

Today, GBARecomp does not have a one-command "make a full playable new game
repo" tool.

## Common questions

**I built the project. Where is the exe?**

If you only used `gbarecomp.exe build`, there is no exe. Look for
`gbarecomp_game.lib`. That is the expected output.

**Can I open the `.sln` in Visual Studio?**

Yes. Open the generated `.sln` if you want to inspect the library build. It
still will not create a playable game exe.

**Do I need the GBA BIOS?**

No BIOS is needed to generate the C++ library. A playable game repo may ask for
a BIOS or may run without one, depending on that project.

**Can I use any `.gba` file?**

The CLI can try to generate C++ from many `.gba` files. A playable game repo
usually accepts only one exact ROM revision. Check the SHA-1 in that game's
README.
