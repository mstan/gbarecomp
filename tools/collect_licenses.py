#!/usr/bin/env python3
"""Collect notices for the dependencies actually included in a desktop package."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

CORE_NAMES = ("audio", "diagnostics", "input", "mod", "persistence", "presentation")
TCC_URL = "https://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27.tar.bz2"
TCC_SHA256 = "de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c"


def revision(root):
    return subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()


def source_url(root):
    for remote in ("origin", "mstan"):
        result = subprocess.run(["git", "-C", str(root), "remote", "get-url", remote],
                                text=True, capture_output=True)
        if result.returncode == 0:
            return result.stdout.strip().removesuffix(".git").replace("git@github.com:", "https://github.com/")
    raise RuntimeError(f"Source remote is missing: {root}")


def dependency_sources(engine, ui, *, netplay=False):
    sources = {"gbarecomp": engine / "LICENSE",
               "arm-recomp-core": engine / "external/arm-recomp-core/LICENSE",
               "recomp-ui": ui / "LICENSE",
               "Dear-ImGui": ui / "src/third_party/imgui/LICENSE.txt",
               "launcher-fonts": ui / "assets/common/fonts/NOTICE.md",
               "launcher-images": ui / "assets/common/img/NOTICE.md"}
    # These services are linked by the runtime, regardless of optional netplay.
    for core in CORE_NAMES:
        name = f"recomp-{core}-core"
        sources[name] = engine / "external/shared-cores" / name / "LICENSE"
    if netplay:
        for name in ("recomp-net", "rbengine"):
            sources[name] = engine / "external" / name / "LICENSE"
    return sources


def tinycc_source(archive, output):
    if hashlib.sha256(archive.read_bytes()).hexdigest() != TCC_SHA256:
        raise RuntimeError("TinyCC source archive checksum differs")
    output.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(archive, output / archive.name)
    with tarfile.open(archive) as source:
        license_file = source.extractfile("tcc-0.9.27/COPYING")
        if license_file is None:
            raise RuntimeError("TinyCC source license is missing")
        (output / "COPYING").write_bytes(license_file.read())


def archive_notices(a):
    header = (a.archive_root / 'include/archive.h').read_text(encoding='utf-8')
    if not any(line.split() == ['#define', 'ARCHIVE_VERSION_NUMBER', '3008011'] for line in header.splitlines()):
        raise RuntimeError('The Windows archive notices recipe is pinned to libarchive 3.8.11')
    if not a.mingw:
        raise RuntimeError('--archive-root currently requires the Windows release codec set')
    for package in ('libb2', 'bzip2', 'libiconv', 'xz', 'zlib', 'zstd'):
        shutil.copytree(a.mingw / 'share/licenses' / package,
                        a.output / package, dirs_exist_ok=True)
    archive_out = a.output / 'libarchive'
    archive_out.mkdir(exist_ok=True)
    shutil.copyfile(a.archive_root / 'share/licenses/libarchive/LICENSE', archive_out / 'LICENSE')
    source_cache = a.cache_dir / 'rom-archives'
    source_cache.mkdir(parents=True, exist_ok=True)
    archive_sources = [
        ('libarchive-3.8.11.tar.xz', 'https://github.com/libarchive/libarchive/releases/download/v3.8.11/libarchive-3.8.11.tar.xz',
         'a3dc0872b1b39da34dc18ef1b944f0f11ffd37425d70ae04be679279f1cf43d2'),
        ('libiconv-1.18.tar.gz', 'https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.18.tar.gz',
         '3b08f5f4f9b4eb82f151a7040bfd6fe6c6fb922efe4b1659c66ea933276965e8'),
        ('lz4-LICENSE', 'https://raw.githubusercontent.com/lz4/lz4/v1.10.0/lib/LICENSE',
         '8b58c446121a109ccf32edc094bba3010a3d85e4ee3702950db55e4d3e87736c'),
    ]
    recipe_url = 'https://raw.githubusercontent.com/msys2/MINGW-packages/f89ccd3564cceff9ed46d778c91f278381393c75/mingw-w64-libiconv/'
    for filename, digest in (
        ('0003-add-cp65001-as-utf8-alias.patch', 'cb2b1cca44c9b9bd11c3bd33997c10d22dff263542275324c88f34b59910ef87'),
        ('0002-fix-cr-for-awk-in-configure.all.patch', '89d5c0f666e50a0186cfb142ba7b77e8fe1ac4d65bdfd9ae14ae8d2f0045a87c'),
        ('fix-pointer-buf.patch', '6b6e2393840f4dc6067587165777b1a07978f4c05247d7c1010a45ad251bbeeb'),
        ('iconv.pc', '56e7ec406bf42eb66b1d972f20d229f7a76ba00c38c7b4403bf348875704baae'),
        ('PKGBUILD', '4d9f62cdcd26dc0e2c1abe9c558239a273dc9668a00948cf3585fae86f3389d2'),
    ):
        archive_sources.append((filename, recipe_url + filename, digest))
    lines = ['# Archive reader and codec source', '',
             'libarchive and its RAR readers use BSD licenses, not proprietary UnRAR.',
             'libiconv is a separate replaceable LGPL DLL; its source and build inputs are included.', '']
    for filename, url, digest in archive_sources:
        cached = source_cache / filename
        if not cached.exists():
            with urllib.request.urlopen(url, timeout=60) as response:
                cached.write_bytes(response.read())
        if hashlib.sha256(cached.read_bytes()).hexdigest() != digest:
            raise RuntimeError(f'Archive dependency source checksum differs: {filename}')
        shutil.copyfile(cached, archive_out / filename)
        lines.append(f'- {filename}: {url} (SHA-256 {digest})')
    (archive_out / 'SOURCES.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')


def collect(args):
    engine, ui, output = args.engine.resolve(), args.ui.resolve(), args.output.resolve()
    cache = {}
    if args.build_dir:
        for line in (args.build_dir / "CMakeCache.txt").read_text().splitlines():
            if ":" in line and "=" in line and not line.startswith(("#", "//")):
                key, value = line.split("=", 1)
                cache[key.split(":", 1)[0]] = value
    netplay = cache.get("GBARECOMP_NETPLAY", "OFF") in ("ON", "TRUE", "1")
    sources = dependency_sources(engine, ui, netplay=netplay)
    if args.dependencies is not None:
        unknown = set(args.dependencies) - sources.keys()
        if unknown:
            raise RuntimeError(f"Unknown or inactive dependencies: {sorted(unknown)}")
        sources = {name: sources[name] for name in args.dependencies}
    # Validate before staging; a missing required notice must fail packaging.
    for name, source in sources.items():
        if not source.is_file():
            raise FileNotFoundError(f"Required {name} notice is missing: {source}")
    output.mkdir(parents=True, exist_ok=True)
    shipped = (args.shipped_dir or output.parent).resolve()
    for name, source in sources.items():
        shutil.copyfile(source, output / f"{name}.txt")
    if args.mingw:
        packages = {"gcc-libs", "libwinpthread", "SDL2"}
        for library, package in (("zlib1.dll", "zlib"), ("libEGL.dll", "angleproject"),
                                 ("libGLESv2.dll", "angleproject"), ("libpng16-16.dll", "libpng"),
                                 ("libbrotlidec.dll", "brotli"), ("libbrotlicommon.dll", "brotli")):
            if (shipped / library).is_file():
                packages.add(package)
        for package in sorted(packages):
            shutil.copytree(args.mingw / "share/licenses" / package,
                            output / package, dirs_exist_ok=True)
    elif not args.skip_system:
        shutil.copyfile(engine / "platform/android/third_party/SDL/LICENSE.txt", output / "SDL2.txt")
        for package in ("libfreetype6", "libharfbuzz0b", "libpng16-16", "libbrotli1", "libgraphite2-3"):
            shutil.copyfile(args.system_doc / package / "copyright", output / f"{package}.txt")

    tcc = any((shipped / "overlay_toolchain" / path).is_file()
              for path in ("tcc.exe", "tcc", "tcc/tcc.exe", "tcc/tcc", "bin/tcc.exe", "bin/tcc"))
    if tcc:
        archive = args.tcc_source
        if archive is None:
            archive = args.cache_dir / "tcc-0.9.27.tar.bz2"
            if not archive.exists():
                archive.parent.mkdir(parents=True, exist_ok=True)
                with urllib.request.urlopen(TCC_URL) as response:
                    data = response.read()
                if hashlib.sha256(data).hexdigest() != TCC_SHA256:
                    raise RuntimeError("TinyCC source archive checksum differs")
                archive.write_bytes(data)
        tinycc_source(archive, output / "TinyCC")

    if getattr(args, 'archive_root', None):
        archive_notices(args)

    identities = []
    for label, root in (("Title", args.title_root), ("Engine", engine), ("Launcher", ui)):
        if root:
            supplied = getattr(args, ("ui" if label == "Launcher" else label.lower()) + "_source", None)
            supplied_revision = getattr(args, ("ui" if label == "Launcher" else label.lower()) + "_revision", None)
            url = getattr(args, "title_url", None) if label == "Title" else None
            if supplied:
                import re
                match = re.fullmatch(r"(https://github\.com/[^/]+/[^/]+)/tree/([0-9a-f]{40})", supplied)
                if not match: raise RuntimeError("Source URL must identify an exact public Git commit")
                url, supplied_revision = match.groups()
            head = supplied_revision or revision(root)
            if len(head) != 40 or any(c not in "0123456789abcdef" for c in head):
                raise RuntimeError("Source identity must be a full Git revision")
            identities.append({"component": label, "repository": url or source_url(root), "commit": head})
    for name, source in sources.items():
        if name.startswith("recomp-") and name.endswith("-core") or name in ("arm-recomp-core", "recomp-net", "rbengine"):
            root = source.parent
            identities.append({"component": name, "repository": source_url(root), "commit": revision(root)})
    lines = ["# Source identities", "",
             "Supply your own original game image to regenerate the game corpus.", ""]
    lines.extend(f'- {entry["component"]}: {entry["repository"]}/tree/{entry["commit"]}' for entry in identities)
    if tcc:
        lines.extend(("", f"TinyCC 0.9.27 source: TinyCC/tcc-0.9.27.tar.bz2; upstream: {TCC_URL}",
                      f"TinyCC source SHA256: {TCC_SHA256}"))
    (output / "SOURCES.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (output / "dependencies.json").write_text(json.dumps({"dependencies": sorted(sources), "sources": identities,
                                                          "tinycc_source_included": tcc}, indent=2) + "\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("engine", "ui", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--title-root", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--shipped-dir", type=Path)
    parser.add_argument("--mingw", type=Path)
    parser.add_argument("--dependencies", nargs="+")
    parser.add_argument("--tcc-source", type=Path)
    parser.add_argument("--archive-root", type=Path)
    parser.add_argument("--title-url")
    for label in ("title", "engine", "ui"):
        parser.add_argument(f"--{label}-revision")
        parser.add_argument(f"--{label}-source")
    parser.add_argument("--cache-dir", type=Path,
                        default=Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "gbarecomp-sources")
    parser.add_argument("--system-doc", type=Path, default=Path("/usr/share/doc"))
    parser.add_argument("--skip-system", action="store_true", help="For packages without system libraries")
    collect(parser.parse_args(argv))


if __name__ == "__main__":
    main()
