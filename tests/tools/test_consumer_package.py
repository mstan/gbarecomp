import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

spec = importlib.util.spec_from_file_location("consumer_package", Path(__file__).resolve().parents[2] / "tools/consumer_package.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class PackageContract(unittest.TestCase):
    def test_title_payload_provenance_and_private_data_guard(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            title, build, mingw = root / "title", root / "build", root / "mingw"
            files = {title / "release.json": json.dumps({"target": "Fixture", "name": "Fixture Game",
                "repository": "fixture/game", "version": "1", "package": "fixture.view",
                "default_variant": "europe", "variants": {"europe": {"config": "region.toml"}}}),
                title / "region.toml": "[rom]\nsha1='fixture'\n",
                title / "mods/preloaded/packages/fixture.view/1.0.0/manifest.toml": "fixture",
                title / "docs/check.txt": "fixture", title / "recomp/launcher/boxart.tga": "art",
                title / "recomp/launcher/boxart.png": "art", build / "assets/img/boxart.tga": "art",
                build / ("Fixture.exe" if os.name == "nt" else "Fixture"): "executable"}
            for name in ("README.md", "LICENSE", "PERMISSIONS.md", "baserom.md"):
                files[title / name] = name
            for name in ("SDL2.dll", "libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll", "zlib1.dll", "libEGL.dll", "libGLESv2.dll"):
                files[mingw / "bin" / name] = "fixture DLL"
            for path, contents in files.items():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(contents)
            args = ["package", "--project-root", str(title), "--build-dir", str(build),
                    "--mingw-root", str(mingw), "--out-dir", str(root / "out")]
            def git_output(command, **kwargs):
                if "ls-tree" in command:
                    return "160000 commit " + "a" * 40 + "\tmodule\n"
                return "" if "status" in command else "a" * 40 + "\n"
            with patch("sys.argv", args), patch.object(package, "run") as notices, \
                 patch.object(package.subprocess, "check_output", side_effect=git_output), \
                 patch.object(package, "stage_windows_dependencies"), patch.object(package, "appimage"):
                package.main()
                self.assertIn("--title-root", notices.call_args.args)
            stage = next(path for path in (root / "out").iterdir() if path.is_dir())
            self.assertEqual((stage / "region.toml").read_text(), files[title / "region.toml"])
            info = json.loads((stage / "build-info.json").read_text())
            self.assertEqual(info["pins"]["gbarecomp"], "a" * 40)
            if os.name == "nt":
                with zipfile.ZipFile(next((root / "out").glob("*.zip"))) as archive:
                    self.assertIsNone(archive.testzip())
                    self.assertTrue(any(name.endswith("/region.toml") for name in archive.namelist()))
            (title / "docs/private.gba").write_bytes(b"private fixture")
            args[-1] = str(root / "rejected")
            with patch("sys.argv", args), patch.object(package, "run"), \
                 patch.object(package.subprocess, "check_output", side_effect=git_output), \
                 patch.object(package, "stage_windows_dependencies"), \
                 self.assertRaisesRegex(SystemExit, "Forbidden private data"):
                package.main()
            self.assertFalse(list((root / "rejected").glob("*.zip")))


if __name__ == "__main__":
    unittest.main()
