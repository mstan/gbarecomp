"""recomp_paths.py — worktree-aware resolution of the recompiled game build.

Oracle scripts live in <gbarecomp checkout>/oracle/ and historically hardcoded
the paired game as `ROOT.parent / "MinishCapRecomp"`. That breaks under the
git-worktree layout, where the gbarecomp checkout is e.g. `gbarecomp-mc-hp-002`
and the paired game worktree is `MinishCapRecomp-mc-hp-002` — the hardcoded name
would silently drive the WRONG (root/main) exe.

Resolution order (first hit wins):
  1. $GBARECOMP_GAME_DIR / $GBARECOMP_RECOMP_EXE  — explicit override.
  2. Convention pairing: swap the leading `gbarecomp` in the checkout's own
     directory name for `MinishCapRecomp`, as a sibling. So:
         gbarecomp            -> MinishCapRecomp
         gbarecomp-mc-hp-002  -> MinishCapRecomp-mc-hp-002
     Used only if that sibling actually exists.
  3. Fallback: ROOT.parent / "MinishCapRecomp" (legacy behavior).

ROOT is the gbarecomp checkout the calling script lives in
(pathlib.Path(__file__).resolve().parent.parent).
"""
from __future__ import annotations
import os
import pathlib


def game_dir(root: pathlib.Path) -> pathlib.Path:
    env = os.environ.get("GBARECOMP_GAME_DIR")
    if env:
        return pathlib.Path(env)
    paired = root.parent / root.name.replace("gbarecomp", "MinishCapRecomp", 1)
    if paired.exists():
        return paired
    return root.parent / "MinishCapRecomp"


def recomp_exe(root: pathlib.Path) -> pathlib.Path:
    env = os.environ.get("GBARECOMP_RECOMP_EXE")
    if env:
        return pathlib.Path(env)
    return game_dir(root) / "build" / "MinishCapRecomp.exe"
