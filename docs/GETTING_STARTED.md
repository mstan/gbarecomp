# Your first GBARecomp project

This is a Windows x64 path for a developer starting a new port, using
PowerShell, an editor and an ordinary C++ toolchain. No AI assistant is
required. The first milestone is generating and compiling cartridge code;
the game-specific host comes next.

To play an existing port, choose a title from the
[game list](../README.md#public-game-builds) and follow its release instructions.

## 1. Build the recompiler

Install Git for Windows, Python 3.11 or newer, and Visual Studio 2022 with
**Desktop development with C++** and its CMake tools. Open **Developer
PowerShell for VS 2022** and check `git --version`, `python --version` and
`cmake --version`.

Choose a development drive for the checkout and generated output. Replace
the example paths with yours:

```powershell
New-Item -ItemType Directory -Force D:\GBADev\Temp | Out-Null
$env:TEMP = 'D:\GBADev\Temp'
$env:TMP = $env:TEMP
Set-Location D:\GBADev
git clone --recurse-submodules https://github.com/mstan/gbarecomp.git
Set-Location gbarecomp
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64
cmake --build build --config Release --target gba_recompile --parallel
$env:GBARECOMP_CORE = "$PWD\build\Release\gba_recompile.exe"
```

CMake downloads the pinned analysis dependency when needed. This target
builds the code generator; it does not require a game ROM or BIOS.

## 2. Generate and compile one cartridge

Use your own unmodified `.gba` dump, kept outside Git. Check for an existing
port of that title before starting another. Keep the first attempt to one
known revision and the stock game behavior.

From the framework checkout:

```powershell
python tools/cli.py build --rom 'D:\ROMs\My Game.gba' `
  --output D:\GBADev\MyFirstPort
powershell -File D:\GBADev\MyFirstPort\build.ps1
```

The CLI emits C++ under `generated`, records ROM metadata, and creates
CMake/build scripts. The build produces `build\Release\gbarecomp_game.lib`.
That library is the compiled cartridge code. A playable executable also
needs a host linked to the shared runtime.

The default analysis is a starting point. Use a current game's `game.toml`
as a format reference when adding verified function roots, ARM/Thumb entry
states, dispatch targets and cartridge hardware. Pass your configuration to
the CLI with `--config`. Copying another game's addresses does not describe
your ROM.

## 3. Connect a game host

Create the game repository separately, with a pinned `gbarecomp` submodule.
Use a maintained game project's CMake and startup code as a layout reference.
The host must connect generated dispatch to the runtime, verify the ROM,
provide the game's configuration, and integrate display, audio, input and
saves. Follow that project's BIOS generation/linking pattern using your own
supported BIOS; neither BIOS nor cartridge data belongs in the repository.

Read the [architecture guide](ARCHITECTURE.md) for the boundary between
shared GBA hardware and game-owned integration. Keep generated code in
ignored build output. Fix analysis inputs or the emitter and regenerate
instead of editing emitted functions by hand.

## 4. Establish one working route

Start with cold boot, title selection and the first playable scene at the
original 240×160 view. Compare input, scrolling, sprites, sound and scene
transitions with the original game in a trusted emulator. Record passed,
failed and untested behavior. A successful compile is one milestone; a
working scene and a complete campaign require separate checks.

Keep the interpreter fallback available while resolving code coverage.
Add widescreen, link play or mods after the baseline is repeatable. Each
needs title-specific integration and its own validation. Keep alternate ROM
revisions and content variants for later; they need explicit identities and
compatible saves.

The [Windows setup reference](WINDOWS_GAME_SETUP.md) explains the generated
files and common tool errors. The [project principles](../PRINCIPLES.md)
and [mod package reference](MOD_PACKAGES.md) cover the next development steps.
