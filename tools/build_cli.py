"""Build the self-contained Windows GBARecomp CLI archive."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys


ARCHIVE = "gbarecomp-cli-windows-x86_64.zip"


def run(command: list[str], cwd: Path) -> None:
    print("+", subprocess.list2cmdline(command))
    subprocess.run(command, cwd=cwd, check=True)


def remove_readonly(function, path: str, _error) -> None:
    os.chmod(path, stat.S_IWRITE)
    function(path)


def find_native_cmake(explicit: str | None) -> str:
    if explicit:
        return explicit
    candidate = shutil.which("cmake")
    if candidate and not any(
        marker in candidate.lower().replace("/", "\\")
        for marker in ("\\msys", "\\cygwin", "\\git\\usr\\bin\\")
    ):
        return candidate
    program_files_x86 = os.environ.get("ProgramFiles(x86)")
    if program_files_x86:
        vswhere = Path(program_files_x86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        if vswhere.is_file():
            result = subprocess.run(
                [str(vswhere), "-latest", "-products", "*", "-find",
                 r"Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"],
                check=True,
                capture_output=True,
                text=True,
            )
            match = next((line.strip() for line in result.stdout.splitlines() if line.strip()), None)
            if match:
                return match
    raise SystemExit(
        "Native Windows CMake was not found. Install CMake or the Visual Studio "
        "C++ CMake tools, or pass --cmake PATH."
    )


def main() -> int:
    args_parser = argparse.ArgumentParser()
    args_parser.add_argument("--cmake", help="CMake executable")
    args_parser.add_argument("--python", default=sys.executable)
    args_parser.add_argument("--output", help="archive output directory")
    args = args_parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    cmake = find_native_cmake(args.cmake)
    build_root = root / "build" / "cli-release"
    core_build = build_root / "core"
    dist = build_root / "dist"
    work = build_root / "pyinstaller"
    output = Path(args.output).resolve() if args.output else build_root
    if build_root.exists():
        shutil.rmtree(build_root, onexc=remove_readonly)
    build_root.mkdir(parents=True)
    output.mkdir(parents=True, exist_ok=True)
    header_stage = build_root / "framework" / "include"
    header_stage.mkdir(parents=True)
    for name in ("runtime_arm.h", "runtime_arm_types.h"):
        shutil.copy2(root / "src" / "armv4t" / name, header_stage / name)

    run([cmake, "-S", str(root), "-B", str(core_build), "-A", "x64",
         "-DGBARECOMP_COMPILER_CACHE=OFF"], root)
    run([cmake, "--build", str(core_build), "--config", "Release",
         "--target", "gba_recompile"], root)
    candidates = (
        core_build / "Release" / "gba_recompile.exe",
        core_build / "gba_recompile.exe",
    )
    core = next((path for path in candidates if path.is_file()), None)
    if core is None:
        raise SystemExit("gba_recompile.exe was not produced.")
    packaged_core = build_root / "gba_recompile-core.exe"
    shutil.copy2(core, packaged_core)
    separator = ";" if sys.platform == "win32" else ":"
    run([
        args.python, "-m", "PyInstaller", "--noconfirm", "--clean",
        "--onedir", "--console", "--name", "gbarecomp",
        "--distpath", str(dist), "--workpath", str(work),
        "--specpath", str(build_root),
        "--add-binary", f"{packaged_core}{separator}.",
        "--add-data", f"{header_stage}{separator}framework/include",
        str(root / "tools" / "cli.py"),
    ], root)
    package = dist / "gbarecomp"
    if not (package / "gbarecomp.exe").is_file():
        raise SystemExit("PyInstaller did not produce gbarecomp.exe.")
    shutil.copy2(root / "LICENSE", package / "LICENSE")
    shutil.copy2(
        root / "THIRD_PARTY_ATTRIBUTION.md",
        package / "THIRD_PARTY_ATTRIBUTION.md",
    )
    (package / "README.txt").write_text(
        "GBARecomp command-line release\n"
        "\n"
        "Generate a C++ source project:\n"
        "\n"
        "  gbarecomp.exe build --rom C:\\Games\\MyGame.gba "
        "--output C:\\Projects\\MyGameRecomp\n"
        "\n"
        "Then run build.ps1 in the output folder to compile the generated "
        "static library.\n"
        "A complete playable port still needs per-game configuration and "
        "runtime integration.\n"
        "\n"
        "Documentation: https://github.com/mstan/gbarecomp\n",
        encoding="utf-8",
        newline="\n",
    )
    archive = Path(shutil.make_archive(
        str(output / ARCHIVE.removesuffix(".zip")), "zip", package
    ))
    print(f"Created {archive} ({archive.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
