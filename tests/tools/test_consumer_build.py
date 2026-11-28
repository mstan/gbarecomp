import argparse
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("consumer_build", Path(__file__).resolve().parents[2] / "tools/consumer_build.py")
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


class BuildPlan(unittest.TestCase):
    def test_metadata_controls_region_and_symbols_without_title_branches(self):
        root = Path("/fixture/title")
        meta = {"target": "FixtureGame", "variants": {"europe": {"generated": "generated/europe",
            "config": "game.toml", "config_overlays": ["symbols/region.toml"],
            "symbols": "symbols/functions.tsv", "data_symbols": "symbols/data.tsv",
            "cmake": {"FIXTURE_REGION": "EUROPE"}}}}
        args = argparse.Namespace(variant="europe", generated_dir=None, build_dir=Path("/fixture/build"),
            engine=Path("/fixture/engine"), ui=Path("/fixture/ui"), producer=Path("/fixture/gba_recompile"),
            mingw_root=None, cmake="cmake", jobs=2, skip_regen=False,
            rom=Path("/fixture/original.gba"), bios=Path("/fixture/bios.bin"), cmake_arg=[])
        plan = driver.commands(root, meta, args)
        self.assertIn("--data-symbols", plan[0])
        self.assertIn(str(root / "symbols/region.toml"), plan[0])
        self.assertIn("-DFIXTURE_REGION=EUROPE", plan[-2])
        self.assertTrue(all(flag in plan[-2] for flag in driver.HLE_FLAGS))
        self.assertEqual(plan[-1][-3:], ["FixtureGame", "--parallel", "2"])
        self.assertNotIn("_mmz_build.lock", " ".join(sum(plan, [])))

    def test_hash_failure_leaves_original_unchanged(self):
        with tempfile.TemporaryDirectory() as temp:
            image = Path(temp) / "original.gba"
            image.write_bytes(b"original input")
            with self.assertRaisesRegex(ValueError, "SHA-1 mismatch"):
                driver.verify(image, "0" * 40)
            self.assertEqual(image.read_bytes(), b"original input")


if __name__ == "__main__":
    unittest.main()
