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
configurations = [("O1", "O1", "OFF", None), ("O2", "O2", "OFF", None),
                  ("O2-repeat", "O2", "OFF", None), ("O2-IPO", "O2", "ON", None)]
for name, value in (("BIOS-A", 0), ("BIOS-repeat", 0), ("BIOS-changed", 1)):
    bios = output / (name + "-source")
    bios.mkdir(exist_ok=True)
    # Synthetic fixture only; no ROM-derived source is stored here.
    (bios / "bios_recompiled.cpp").write_text(f"int synthetic_bios_body = {value};\n")
    (bios / "bios_recompiled.h").write_text("extern int synthetic_bios_body;\n")
    (bios / "bios_dispatch_table.cpp").write_text("int synthetic_bios_dispatch = 0;\n")
    configurations.append((name, "O2", "OFF", bios))
configurations.extend([("AUDIO-windows", "O2", "OFF", None),
                       ("AUDIO-repeat", "O2", "OFF", None)])
for name, level, ipo, bios in configurations:
    build = output / name
    with (output / (name + ".log")).open("w") as log:
        command = [args.cmake, "-S", str(fixture), "-B", str(build),
            "-G", args.generator, "-DCMAKE_CXX_COMPILER=" + args.compiler,
            "-DCMAKE_MAKE_PROGRAM=" + args.make_program, "-DCMAKE_BUILD_TYPE=Release",
            "-DIDENTITY_MODULE=" + str(module), "-DGENERATED_OPT=" + level,
            "-DUSE_IPO=" + ipo,
            "-DGBARECOMP_DEVICE_AUDIO_WINDOWS=" + ("ON" if name.startswith("AUDIO-") else "OFF")]
        if bios is not None:
            command.append("-DGBARECOMP_GENERATED_BIOS_DIR=" + str(bios))
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    header = (build / "identity_probe-netplay/gba_netplay_build_identity.h").read_text()
    rows.append({"configuration": name, "identity": re.search(r'"([a-f0-9]{64})"', header)[1]})
assert rows[0]["identity"] != rows[1]["identity"], "O1/O2 unexpectedly match"
assert rows[1]["identity"] == rows[2]["identity"], "Identical choices depend on build directory"
assert rows[1]["identity"] != rows[3]["identity"], "IPO choice is missing"
assert rows[4]["identity"] == rows[5]["identity"], "BIOS identity depends on its directory"
assert rows[4]["identity"] != rows[6]["identity"], "Selected BIOS body is missing from identity"
assert rows[1]["identity"] != rows[4]["identity"], "BIOS availability is missing from identity"
assert rows[1]["identity"] != rows[7]["identity"], "Device/audio ownership profile is missing"
assert rows[7]["identity"] == rows[8]["identity"], "Device/audio profile depends on build directory"
(output / "results.json").write_text(json.dumps(rows, indent=2))
print("Optimization/IPO and selected BIOS identities differ; equivalent inputs are directory-independent")
