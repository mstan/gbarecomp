"""Different source optimization/IPO choices must not silently match peers."""
import argparse
import json
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser()
for name in ("cmake", "generator", "compiler", "make-program", "output"):
    p.add_argument("--" + name, required=True)
args = p.parse_args()
fixture = Path(__file__).resolve().parent / "identity_fixture"
module = fixture.parents[2] / "cmake/GbaNetplayIdentity.cmake"
output = Path(args.output)
output.mkdir(parents=True, exist_ok=True)
rows = []
for name, level, ipo in (("O1", "O1", "OFF"), ("O2", "O2", "OFF"),
                          ("O2-repeat", "O2", "OFF"), ("O2-IPO", "O2", "ON")):
    build = output / name
    with (output / (name + ".log")).open("w") as log:
        subprocess.run([args.cmake, "-S", str(fixture), "-B", str(build),
            "-G", args.generator, "-DCMAKE_CXX_COMPILER=" + args.compiler,
            "-DCMAKE_MAKE_PROGRAM=" + args.make_program, "-DCMAKE_BUILD_TYPE=Release",
            "-DIDENTITY_MODULE=" + str(module), "-DGENERATED_OPT=" + level,
            "-DUSE_IPO=" + ipo], stdout=log, stderr=subprocess.STDOUT, check=True)
    header = (build / "identity_probe-netplay/gba_netplay_build_identity.h").read_text()
    rows.append({"configuration": name, "identity": re.search(r'"([a-f0-9]{64})"', header)[1]})
assert rows[0]["identity"] != rows[1]["identity"], "O1/O2 unexpectedly match"
assert rows[1]["identity"] == rows[2]["identity"], "Identical choices depend on build directory"
assert rows[1]["identity"] != rows[3]["identity"], "IPO choice is missing"
(output / "results.json").write_text(json.dumps(rows, indent=2))
print("O1, O2 and IPO identities differ; repeated O2 is stable")
