"""Run with python3 packaging/tests/test_web_runtime_config.py (no ROM needed)."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "packaging/web/runtime_config.py"
spec = importlib.util.spec_from_file_location("runtime_config", SCRIPT)
config = importlib.util.module_from_spec(spec)
spec.loader.exec_module(config)


class RuntimeConfigTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('bash'), 'web build requires bash')
    def test_build_script_selects_default_explicit_and_no_config(self):
        with tempfile.TemporaryDirectory(prefix='web config ') as directory:
            root = Path(directory)
            project, bios = root / 'project', root / 'bios'
            project.mkdir()
            bios.mkdir()
            default = project / 'game.toml'
            default.write_text('[save]\nsize = 512\n')
            explicit = root / 'another config.toml'
            explicit.write_text('[save]\nsize = 8192\n')
            output = root / 'web/runtime.toml'
            env = dict(os.environ, GBARECOMP_WEB_BUILD_DIR=str(root / 'core-build'),
                       GBARECOMP_WEB_GAME_BUILD_DIR=str(root / 'game-build'),
                       GBARECOMP_WEB_OUT_DIR=str(output.parent))
            def run(extra):
                result = subprocess.run(['bash', str(ROOT / 'packaging/web/build_web.sh'),
                                         str(project), str(bios)] + extra,
                                        env=env, capture_output=True, text=True)
                # Intentionally stop before SDK/build: BIOS sources are absent.
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('missing generated BIOS', result.stderr)
                return output.read_text()
            self.assertIn('size = 512', run([]))
            self.assertIn('size = 8192', run(['', '', str(explicit)]))
            default.unlink()
            self.assertEqual(config.export_config(None), run([]))

    def test_runtime_subset_and_host_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "game.toml"
            source.write_text('''[game]
short_name = "Example #1" # keep the quoted hash
entry = 0x08000000
[rom]
path = "/private/game.gba"
sha1 = "abc"
[bios]
path = "C:/private/bios.bin"
skip_intro = true
[save]
path = "../old.sav"
type = "eeprom"
size = 512
[video]
sharp_filter = true
[audio]
shadow = false
[[functions]]
address = 0x08000000
name = "private_symbol"
''')
            result = config.export_config(source)
            for text in ['short_name = "Example #1"', 'sha1 = "abc"',
                         'skip_intro = true', 'type = "eeprom"', 'size = 512',
                         'sharp_filter = true', 'shadow = false']:
                self.assertIn(text, result)
            for text in ['private', 'old.sav', 'path =', 'functions', 'entry =']:
                self.assertNotIn(text, result)

    def test_regional_precedence_and_width_aliases(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "config").mkdir()
            source = root / "game.toml"
            source.write_text('[game]\nshort_name = "Test_Game"\ndefault_region = "us"\n'
                              '[video]\nview_width = 280\nwidescreen = 30\n')
            (root / "config/us.toml").write_text('[video]\nview_width = 360\n')
            (root / "config/Test_Game_us.toml").write_text('[save]\nsize = 512\n')
            (root / "config/TestGame_us.toml").write_text('[save]\nsize = 8192\n')
            result = config.export_config(source)
            self.assertNotIn('default_region', result)
            self.assertIn('size = 8192', result)
            self.assertNotIn('size = 512', result)
            self.assertLess(result.index('widescreen = 30'), result.index('view_width = 360'))

    def test_no_config_replaces_previous_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "runtime.toml"
            output.write_text('[save]\nsize = 512\n')
            subprocess.run([sys.executable, str(SCRIPT), str(output)], check=True)
            self.assertEqual(output.read_text(), config.export_config(None))
            self.assertNotIn('[save]', output.read_text())

    def test_overlay_noops_preserve_base_values(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'config').mkdir()
            source = root / 'game.toml'
            source.write_text('[game]\ndefault_region = "us"\n'
                              '[save]\ntype = "eeprom"\nsize = 512\n'
                              '[video]\nview_width = 320\n')
            (root / 'config/us.toml').write_text('[save]\ntype = "TBD"\nsize = ""\n'
                                                 '[video]\nview_width = 0\nwidescreen = -1\n')
            result = config.export_config(source)
            self.assertIn('type = "eeprom"', result)
            self.assertIn('size = 512', result)
            self.assertIn('view_width = 320', result)
            self.assertNotIn('widescreen', result)

    def test_explicit_missing_config_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "runtime.toml"
            result = subprocess.run([sys.executable, str(SCRIPT), str(output),
                                     '--config', str(output.parent / 'missing.toml')],
                                    capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
