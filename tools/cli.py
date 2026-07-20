"""Human-friendly GBARecomp release CLI."""

from __future__ import annotations

import argparse
from collections import deque
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


VERSION = "0.2.0"
HEADERS = ("runtime_arm.h", "runtime_arm_types.h")


def resource_root() -> Path:
    frozen = getattr(sys, "_MEIPASS", None)
    return Path(frozen) if frozen else Path(__file__).resolve().parents[1]


def core_path() -> Path:
    override = os.environ.get("GBARECOMP_CORE")
    if override:
        return Path(override).expanduser().resolve()
    root = resource_root()
    for candidate in (
        root / "gba_recompile-core.exe",
        root / "build" / "cli-release" / "core" / "Release" / "gba_recompile.exe",
    ):
        if candidate.is_file():
            return candidate
    raise FileNotFoundError("The bundled gba_recompile core executable is missing.")


def header_root() -> Path:
    root = resource_root()
    bundled = root / "framework" / "include"
    if bundled.is_dir():
        return bundled
    source = root / "src" / "armv4t"
    if source.is_dir():
        return source
    raise FileNotFoundError("The GBARecomp runtime headers are missing.")


def run_core(command: list[str], cwd: Path, verbose: bool) -> int:
    process = subprocess.Popen(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    assert process.stdout is not None
    recent: deque[str] = deque(maxlen=80)
    for raw in process.stdout:
        line = raw.rstrip()
        recent.append(line)
        if verbose or line.startswith("==>") or line.startswith("warn:"):
            print(line)
    code = process.wait()
    if code and not verbose:
        print("[gbarecomp] Native recompiler diagnostics:", file=sys.stderr)
        for line in recent:
            print(line, file=sys.stderr)
    return code


def copy_framework(output: Path) -> None:
    destination = output / "framework" / "include"
    destination.mkdir(parents=True, exist_ok=True)
    source = header_root()
    for name in HEADERS:
        path = source / name
        if not path.is_file():
            raise FileNotFoundError(f"Required framework header is missing: {name}")
        shutil.copy2(path, destination / name)


def write_project(output: Path, rom: Path, used_config: bool) -> None:
    cmake = '''cmake_minimum_required(VERSION 3.20)
project(gbarecomp_generated CXX)

file(GLOB GBARECOMP_SHARDS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/recompiled_*.cpp"
)
set(GBARECOMP_GENERATED ${GBARECOMP_SHARDS}
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/dispatch_table.cpp"
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/symbol_map.cpp")
    list(APPEND GBARECOMP_GENERATED
        "${CMAKE_CURRENT_SOURCE_DIR}/generated/symbol_map.cpp")
endif()

add_library(gbarecomp_game STATIC ${GBARECOMP_GENERATED})
target_include_directories(gbarecomp_game PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/framework/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated"
)
set_target_properties(gbarecomp_game PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
)
'''
    build_ps1 = '''$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
$cmakeSource = if ($cmake) { $cmake.Source.Replace("\\", "/") }

# MSYS/Cygwin CMake rewrites Windows paths passed by PowerShell. Prefer a
# native CMake, including the copy bundled with Visual Studio.
if ($cmake -and $cmakeSource -notmatch "/(msys[^/]*|cygwin[^/]*)/" -and
    $cmakeSource -notmatch "/Git/usr/bin/") {
    $cmakePath = $cmake.Source
} else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\\Installer\\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $cmakePath = & $vswhere -latest -products * `
            -find "Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe" |
            Select-Object -First 1
    }
}

if (-not $cmakePath) {
    throw "Native Windows CMake 3.20+ was not found. Install CMake or the Visual Studio C++ CMake tools, then run this script again."
}

& $cmakePath -S $root -B "$root/build"
& $cmakePath --build "$root/build" --config Release --parallel
'''
    build_sh = '''#!/usr/bin/env sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/build" --parallel
'''
    readme = f'''# GBARecomp output

This folder contains C++ source generated from `{rom.name}`.

## Build the generated source

Windows PowerShell:

```powershell
.\\build.ps1
```

macOS or Linux:

```sh
./build.sh
```

The build creates the `gbarecomp_game` static library. This confirms that the
generated source compiles; it is not a complete playable port by itself.

To make a playable port, add game-specific configuration and integrate this
library with the GBARecomp runtime. Existing target repositories are useful
starting points: https://github.com/mstan/gbarecomp
'''
    metadata = {
        "format": 1,
        "tool": "gbarecomp",
        "tool_version": VERSION,
        "source_rom_name": rom.name,
        "used_game_config": used_config,
        "generated_at_unix": int(time.time()),
    }
    (output / "CMakeLists.txt").write_text(cmake, encoding="utf-8", newline="\n")
    (output / "build.ps1").write_text(build_ps1, encoding="utf-8", newline="\n")
    (output / "build.sh").write_text(build_sh, encoding="utf-8", newline="\n")
    (output / "README.md").write_text(readme, encoding="utf-8", newline="\n")
    (output / "gbarecomp-project.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8", newline="\n"
    )


def build(args: argparse.Namespace) -> int:
    rom = Path(args.rom).expanduser().resolve()
    output = Path(args.output).expanduser().resolve()
    config = Path(args.config).expanduser().resolve() if args.config else None
    symbols = Path(args.symbols).expanduser().resolve() if args.symbols else None
    if not rom.is_file():
        raise ValueError(f"ROM file not found: {rom}")
    if rom.suffix.lower() != ".gba" or rom.stat().st_size < 192:
        raise ValueError("GBARecomp expects a valid .gba cartridge image.")
    for label, path in (("config", config), ("symbols", symbols)):
        if path and not path.is_file():
            raise ValueError(f"{label.capitalize()} file not found: {path}")
    if output.exists() and any(output.iterdir()) and not args.force:
        raise ValueError(
            f"Output folder is not empty: {output}\n"
            "Choose a new folder or add --force to update it."
        )

    output.mkdir(parents=True, exist_ok=True)
    generated = output / "generated"
    generated.mkdir(exist_ok=True)
    command = [str(core_path()), "--rom", str(rom), "--out", str(generated)]
    if config:
        command.extend(("--config", str(config)))
    if symbols:
        command.extend(("--symbols", str(symbols)))
    if args.entry:
        command.extend(("--entry", args.entry))
    if args.max_functions:
        command.extend(("--max-functions", str(args.max_functions)))
    if args.codegen_shards:
        command.extend(("--codegen-shards", str(args.codegen_shards)))

    print(f"[gbarecomp] Recompiling {rom.name}", flush=True)
    print(f"[gbarecomp] Output: {output}", flush=True)
    code = run_core(command, output, args.verbose)
    if code:
        return code
    required = (generated / "recompiled.h", generated / "dispatch_table.cpp")
    if not all(path.is_file() for path in required) or not list(generated.glob("recompiled_*.cpp")):
        raise RuntimeError("Recompiler completed without the expected generated source.")
    copy_framework(output)
    write_project(output, rom, config is not None)
    print("[gbarecomp] Done. Generated source and build scripts are ready.")
    print(f"[gbarecomp] Next: powershell -File \"{output / 'build.ps1'}\"")
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="gbarecomp",
        description="Recompile a GBA ROM into a buildable C++ source project.",
    )
    result.add_argument("--version", action="version", version=f"gbarecomp {VERSION}")
    commands = result.add_subparsers(dest="command", required=True)
    command = commands.add_parser("build", help="generate C++ source from a GBA ROM")
    command.add_argument("--rom", required=True, help="path to a legally obtained .gba ROM")
    command.add_argument("--output", required=True, help="new folder for generated source")
    command.add_argument("--config", help="optional per-game TOML configuration")
    command.add_argument("--symbols", help="optional imported symbol TSV")
    command.add_argument("--entry", help="optional hexadecimal entry address")
    command.add_argument("--max-functions", type=int, help="limit discovered functions")
    command.add_argument("--codegen-shards", type=int, choices=range(2, 257))
    command.add_argument("--force", action="store_true", help="update a non-empty output folder")
    command.add_argument("--verbose", action="store_true", help="show all recompiler diagnostics")
    command.set_defaults(handler=build)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        return int(args.handler(args))
    except (FileNotFoundError, RuntimeError, ValueError) as error:
        print(f"gbarecomp: error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
