"""Fetch pinned, publicly shared save data for opt-in Emerald qualification."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("directory", type=Path, help="ignored local fixture directory")
args = parser.parse_args()
sources = json.loads((Path(__file__).parent / "scenarios/emerald-public-saves.json").read_text())
args.directory.mkdir(parents=True, exist_ok=True)
for source in sources:
    dest = args.directory / source["filename"]
    if dest.exists():
        data = dest.read_bytes()
    else:
        with urllib.request.urlopen(source["download"], timeout=30) as response:
            data = response.read(source["bytes"] + 1)
    if len(data) != source["bytes"] or hashlib.sha256(data).hexdigest() != source["sha256"]:
        raise ValueError(f"{dest.name}: source changed or file is invalid; refusing to install")
    if not dest.exists():
        with dest.open("xb") as out:
            out.write(data)
    print(f"{dest}: verified {len(data)} bytes")
