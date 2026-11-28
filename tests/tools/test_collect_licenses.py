import argparse
import importlib.util
import pathlib
import tempfile
import unittest
from unittest.mock import patch

MODULE = pathlib.Path(__file__).resolve().parents[2] / "tools/collect_licenses.py"
spec = importlib.util.spec_from_file_location("collect_licenses", MODULE)
notices = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notices)


class NoticeCatalog(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.engine, self.ui = self.root / "engine", self.root / "ui"
        self.sources = notices.dependency_sources(self.engine, self.ui, netplay=True)
        for source in self.sources.values():
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("fixture notice")
        self.args = argparse.Namespace(engine=self.engine, ui=self.ui, output=self.root / "package/licenses",
            build_dir=None, dependencies=None, mingw=None, skip_system=True, system_doc=None,
            shipped_dir=None, tcc_source=None, cache_dir=self.root / "cache", title_root=None)

    def collect(self):
        with patch.object(notices, "source_url", return_value="https://example.invalid/source"), \
             patch.object(notices, "revision", return_value="a" * 40):
            notices.collect(self.args)

    def test_runtime_services_and_optional_netplay(self):
        with patch.object(notices.urllib.request, "urlopen", side_effect=AssertionError("unshipped compiler download")):
            self.collect()
        for core in notices.CORE_NAMES:
            self.assertTrue((self.args.output / f"recomp-{core}-core.txt").is_file())
        self.assertFalse((self.args.output / "recomp-net.txt").exists())
        self.assertFalse((self.args.output / "TinyCC").exists())
        build = self.root / "build"
        build.mkdir()
        (build / "CMakeCache.txt").write_text("GBARECOMP_NETPLAY:BOOL=ON\n")
        self.args.build_dir = build
        self.collect()
        self.assertTrue((self.args.output / "recomp-net.txt").is_file())
        self.assertTrue((self.args.output / "rbengine.txt").is_file())

    def test_missing_service_notice_fails_before_staging(self):
        self.sources["recomp-persistence-core"].unlink()
        with self.assertRaisesRegex(FileNotFoundError, "recomp-persistence-core"):
            self.collect()
        self.assertFalse(self.args.output.exists())

    def test_shipped_compiler_requires_matching_source(self):
        toolchain = self.args.output.parent / "overlay_toolchain/tcc"
        toolchain.mkdir(parents=True)
        (toolchain / "tcc.exe").write_bytes(b"fixture")
        self.args.tcc_source = self.root / "invalid.tar.bz2"
        self.args.tcc_source.write_bytes(b"wrong source")
        with self.assertRaisesRegex(RuntimeError, "checksum differs"):
            self.collect()

    def test_archive_recipe_rejects_an_unmatched_codec_release(self):
        self.args.archive_root = self.root / "archive"
        (self.args.archive_root / "include").mkdir(parents=True)
        (self.args.archive_root / "include/archive.h").write_text("#define ARCHIVE_VERSION_NUMBER 3008000\n")
        with self.assertRaisesRegex(RuntimeError, "3.8.11"):
            self.collect()

    def test_source_override_requires_exact_commit(self):
        self.args.title_root = self.root / "title"
        self.args.title_source = "https://github.com/fixture/title/tree/main"
        with self.assertRaisesRegex(RuntimeError, "exact public Git commit"):
            self.collect()


if __name__ == "__main__":
    unittest.main()
