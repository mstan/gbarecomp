"""Recorded output paths must not become playback session controls."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("replay_diagnostic", ROOT / "tools/replay_diagnostic.py")
TOOL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOL)


class RecordedArgumentsTest(unittest.TestCase):
    def filtered(self, args):
        with tempfile.TemporaryDirectory(prefix="gba-replay-args-") as temporary:
            path = Path(temporary) / "capture.replay.args.txt"
            path.write_text("Game.exe\n" + "\n".join(args) + "\n", encoding="utf-8")
            return TOOL.recorded_args(path)

    def test_dump_state_path_cannot_consume_following_playback_option(self):
        args = self.filtered(["--dump-state", "original output.state", "--view-width", "284",
                              "--volume", "0", "--no-window"])
        self.assertEqual(args, ["--view-width", "284", "--volume", "0"])

    def test_inline_dump_state_is_not_reused_but_guest_options_are(self):
        args = self.filtered(["--dump-state=original output.state", "--view-width", "320",
                              "--gyro-sensitivity", "2", "--fullscreen"])
        self.assertEqual(args, ["--view-width", "320", "--gyro-sensitivity", "2"])


if __name__ == "__main__":
    unittest.main()
