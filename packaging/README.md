# packaging — portable release builds

Game-agnostic packaging for gbarecomp titles. Every game exe is a plain SDL2
program, so one script per platform serves all of them; a game repo calls in
rather than copying scripts around.

| Platform | Script | Status |
|---|---|---|
| macOS | `package_macos.sh` | verified booting a real ROM |
| Steam Deck / Flatpak | `flatpak/build_flatpak.sh` | **verified on a real Deck**: builds, installs, launches |
| Linux (ordinary distros) | `package_linux.sh` | **untested** — the counterpart to the macOS script |
| Windows | see a game's `tools/package_release.ps1` | pre-existing, unchanged |

## macOS

```bash
gbarecomp/packaging/package_macos.sh --target BoktaiRecomp --name Boktai \
    -DGBAGAME_RECOMP_UI=ON
```

Produces `release-stage/<name>-macos-<arch>/` plus a `.tar.gz`: the executable,
every non-system dylib beside it with install names rewritten to
`@executable_path`, the launcher assets, and an ad-hoc signature.

Two things this handles that a naive script does not:

- **dlopen'd libraries.** `otool -L` cannot see them, and a missing one is not a
  link error — it is a hard abort at startup with no output. Homebrew's `sdl2` is
  now often `sdl2-compat`, whose libSDL2 is a shim that dlopens **SDL3**; the
  script finds and copies it. Without that the game dies with "failed to load
  SDL3".
- **No `.app` bundle.** A bundle was tried and is SIGKILLed before it prints a
  byte. The reason is structural: the launcher resolves its assets directory as
  the executable's own directory, so `assets/` must live in `Contents/MacOS`,
  and `codesign` treats everything there as code —
  `code object is not signed at all / In subcomponent: .../OpenMoji-black-glyf.ttf`
  — so the bundle cannot hold a valid signature while the assets are where the
  runtime needs them. A flat directory is not bundle-validated and works signed
  or unsigned.

`GBARECOMP_STATIC_RELEASE` is deliberately unused here: it pins
`C:/msys64/mingw64/lib/libSDL2.a` and links Windows system libs, so it is
MSYS2-only, and macOS has no static libSystem either.

## Linux

```bash
gbarecomp/packaging/package_linux.sh --target BoktaiRecomp --name Boktai \
    -DGBAGAME_RECOMP_UI=ON
```

Same shape as the macOS script, with an `$ORIGIN` rpath instead of
`@executable_path`. It deliberately does **not** bundle everything `ldd` reports:
glibc, libstdc++, the loader and the graphics stack stay on the host, or the binary
breaks on any machine with a different driver or glibc — bundling libGL in
particular kills hardware acceleration.

For a Steam Deck prefer the Flatpak below; SteamOS has an immutable root, so a
package bringing its own runtime beats a tarball that depends on the host.

## Steam Deck

```bash
gbarecomp/packaging/flatpak/build_flatpak.sh --target BoktaiRecomp \
    --name Boktai --id tech.recomp.BoktaiRecomp
```

Flatpak is the right answer for SteamOS because its root filesystem is
immutable: the runtime supplies SDL2 and the toolchain, so nothing is installed
into the OS and no static linking is needed. Run it in Desktop Mode, then add
the result via Steam > Games > Add a Non-Steam Game to reach it from Gaming Mode.

`--generate-only` writes the manifest, `.desktop` and metainfo without building,
which is all CI can do.

## What is never packaged

The ROM and the BIOS. A recompiled binary already embeds translated ROM code, so
these scripts are for building **your own** copy from **your own** dump — not for
redistribution. The launcher prompts for both on first run.

Recompiled sources (`generated/`) are not committed anywhere in this ecosystem;
regenerate them locally before packaging.
