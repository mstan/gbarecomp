#!/usr/bin/env python3
"""Build a pinned standalone game from its original image and title metadata."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

HLE_FLAGS = [f"-DGBARECOMP_{name}_IMPLEMENTATION=HLE" for name in
             ("BIOS_MATH", "DMA_RAM", "SCANLINE", "HD_SAMPLER", "MEMORY", "PRESENTATION")]


def verify(path, expected):
    if hashlib.sha1(path.read_bytes()).hexdigest() != expected:
        raise ValueError(f"Input SHA-1 mismatch: {path}")


def pinned_source(root, module, path):
    expected = subprocess.check_output(["git", "ls-tree", "HEAD", module], cwd=root, text=True).split()[2]
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=path, text=True).strip()
    if expected != actual:
        raise ValueError(f"{module} must use pinned commit {expected}, got {actual}")
    return actual


def commands(root, meta, args):
    variant = meta["variants"][args.variant]
    generated = (args.generated_dir or root / variant["generated"]).resolve()
    build = args.build_dir.resolve()
    engine, ui = args.engine.resolve(), args.ui.resolve()
    producer = args.producer or build / "tools" / ("gba_recompile.exe" if os.name == "nt" else "gba_recompile")
    common = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DGBARECOMP_BUILD_ORACLE=OFF", *HLE_FLAGS]
    if args.mingw_root:
        prefix = args.mingw_root.resolve()
        common += [f"-DCMAKE_C_COMPILER={(prefix/'bin/cc.exe').as_posix()}",
                   f"-DCMAKE_CXX_COMPILER={(prefix/'bin/c++.exe').as_posix()}",
                   f"-DCMAKE_MAKE_PROGRAM={(prefix/'bin/ninja.exe').as_posix()}",
                   f"-DSDL2_INCLUDE_DIR={(prefix/'include/SDL2').as_posix()}",
                   f"-DSDL2_LIBRARY={(prefix/'lib/libSDL2.dll.a').as_posix()}"]
    plan = []
    if not args.producer:
        plan += [[args.cmake, "-S", engine, "-B", build / "tools", *common],
                 [args.cmake, "--build", build / "tools", "--target", "gba_recompile", "--parallel", args.jobs]]
    if not args.skip_regen or not (generated / "dispatch_table.cpp").is_file():
        command = [producer, "--rom", args.rom.resolve(), "--config", root / variant["config"]]
        for overlay in variant.get("config_overlays", []):
            command += ["--config", root / overlay]
        for key, flag in (("symbols", "--symbols"), ("data_symbols", "--data-symbols")):
            if variant.get(key):
                command += [flag, root / variant[key]]
        plan.append([*command, "--out", generated, "--max-functions", "65536"])
    plan.append([producer, "--bios", args.bios.resolve(), "--config", engine / "bios/gba_bios.toml",
                 "--out", build / "generated-bios", "--max-functions", "4096"])
    flags = [f"-DGBARECOMP_ROOT={engine.as_posix()}", f"-DRECOMP_UI_ROOT={ui.as_posix()}",
             f"-DGBARECOMP_RUNTIME_UI_ROOT={ui.as_posix()}", f"-DGBA_GAME_GENERATED_ROOT={generated.as_posix()}",
             f"-DGBARECOMP_GENERATED_BIOS_DIR={(build/'generated-bios').as_posix()}",
             "-DRECOMP_UI_ENABLE_MODS=ON"]
    flags += [f"-D{key}={value}" for key, value in variant.get("cmake", {}).items()]
    flags += args.cmake_arg
    plan += [[args.cmake, "-S", root, "-B", build, *common, *flags],
             [args.cmake, "--build", build, "--target", meta["target"], "--parallel", args.jobs]]
    return [[str(part) for part in command] for command in plan]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--bios", type=Path, required=True)
    parser.add_argument("--engine", type=Path)
    parser.add_argument("--ui", type=Path)
    parser.add_argument("--producer", type=Path, help="Reuse a recompiler built from the pinned engine")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--generated-dir", type=Path)
    parser.add_argument("--variant")
    parser.add_argument("--usa", action="store_true", help="Compatibility alias for --variant usa")
    parser.add_argument("--skip-regen", action="store_true")
    parser.add_argument("--package", action="store_true")
    parser.add_argument("--jobs", type=int, choices=range(1, 5), default=2)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--mingw-root", type=Path,
                        default=Path(os.environ.get("MINGW_PREFIX", "C:/msys64/mingw64")) if os.name == "nt" else None)
    parser.add_argument("--cmake-arg", action="append", default=[])
    parser.add_argument("--plan", action="store_true", help="Validate inputs and print commands without running them")
    args = parser.parse_args(argv)
    root = args.project_root.resolve()
    meta = json.loads((root / "release.json").read_text())
    args.variant = "usa" if args.usa else args.variant or meta["default_variant"]
    if args.variant not in meta["variants"]:
        parser.error(f"Unsupported variant: {args.variant}")
    args.engine = (args.engine or root / "gbarecomp").resolve()
    args.ui = (args.ui or root / "recomp-ui").resolve()
    args.build_dir = args.build_dir or root / f'build-{"windows" if os.name == "nt" else "linux"}-{args.variant}'
    if args.mingw_root:
        os.environ["PATH"] = str(args.mingw_root / "bin") + os.pathsep + os.environ["PATH"]
        if args.cmake == "cmake":
            args.cmake = str(args.mingw_root / "bin/cmake.exe")
    for path in (args.engine / "CMakeLists.txt", args.ui / "recomp_ui.cmake"):
        if not path.is_file():
            parser.error(f"Missing submodule: {path}; initialize pinned submodules first")
    pins = {module: pinned_source(root, module, path)
            for module, path in (("gbarecomp", args.engine), ("recomp-ui", args.ui))}
    verify(args.rom, meta["variants"][args.variant]["rom_sha1"])
    verify(args.bios, "300c20df6731a33952ded8c436f7f186d25d3492")
    plan = commands(root, meta, args)
    if args.plan:
        print(json.dumps({"pins": pins, "variant": args.variant, "commands": plan}, indent=2))
        return
    args.build_dir.mkdir(parents=True, exist_ok=True)
    for command in plan:
        print("+", *command, flush=True)
        subprocess.run(command, cwd=root, check=True)
    config = meta["variants"][args.variant]["config"]
    shutil.copy2(root / config, args.build_dir / config)
    if args.package:
        subprocess.run([sys.executable, str(Path(__file__).with_name("consumer_package.py")),
                        "--project-root", str(root), "--build-dir", str(args.build_dir),
                        "--variant", args.variant, "--engine", str(args.engine), "--ui", str(args.ui)],
                       cwd=root, check=True)


if __name__ == "__main__":
    main()
