"""Export the scalar configuration understood by runtime.cpp, without host paths.

This deliberately follows apply_toml_file's line-oriented subset, not the
recompiler's full TOML schema. Keep RUNTIME_KEYS in sync with that function.
Python 3.9 compatible; no third-party parser is required.
"""
import argparse
from pathlib import Path
import re


RUNTIME_KEYS = {
    "game": {"short_name", "default_region"},
    "rom": {"sha1", "crc32"},
    "bios": {"sha1", "crc32", "hle", "hle_keep_intro", "skip_intro"},
    "save": {"type", "size"},
    "video": {"screen", "view_width", "resize_view", "sharp_filter",
              "affine_filter", "widescreen"},
    "audio": {"shadow"},
}


def uncomment(raw):
    quoted = False
    for i, char in enumerate(raw):
        if char == '"':
            quoted = not quoted
        if char == '#' and not quoted:
            return raw[:i]
    return raw


def unquote(value):
    if len(value) >= 2 and value.startswith('"') and value.endswith('"'):
        value = value[1:-1]
    return "" if value == "TBD" else value


def read_settings(path, settings):
    section = ""
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = uncomment(raw).strip()
        if line.startswith('[') and line.endswith(']'):
            section = line[1:-1].strip()
        elif '=' in line:
            key, value = (part.strip() for part in line.split('=', 1))
            if key in RUNTIME_KEYS.get(section, set()):
                if value.startswith(('"""', "'''")):
                    raise ValueError(f"{path}: multiline runtime value [{section}].{key} is unsupported")
                scalar = unquote(value)
                # These assignments are conditional in apply_toml_file. A
                # no-op in an overlay must not erase a value from its base.
                if section == "save" and not scalar:
                    continue
                if section == "video" and key in {"view_width", "widescreen"}:
                    if not re.fullmatch(r'[+-]?(?:0[xX][0-9a-fA-F]+|0[0-7]*|[1-9][0-9]*)', scalar):
                        continue
                    unsigned = scalar.lstrip('+-')
                    number = int(scalar, 16 if unsigned.lower().startswith('0x')
                                 else 8 if unsigned.startswith('0') else 10)
                    minimum, maximum = ((240, 2147483647) if key == "view_width"
                                        else (0, (2147483647 - 240) // 2))
                    if not minimum <= number <= maximum:
                        continue
                values = settings.setdefault(section, {})
                # Preserve last-assignment order (view_width and widescreen
                # both set the same runtime field).
                values.pop(key, None)
                values[key] = value


def export_config(source):
    settings = {}
    if source is not None:
        read_settings(source, settings)
        game = settings.get("game", {})
        region = unquote(game.get("default_region", ""))
        name = unquote(game.get("short_name", ""))
        if region:
            names = [region]
            if name:
                names.append(name + "_" + region)
                if '_' in name:
                    names.append(name.replace('_', '') + "_" + region)
            # Same candidates and precedence as runtime.cpp::load_config.
            for stem in names:
                if '/' in stem or '\\' in stem or stem in {'.', '..'}:
                    raise ValueError("Regional config names must not contain directory paths")
                overlay = source.parent / "config" / (stem + ".toml")
                if overlay.exists():
                    read_settings(overlay, settings)
        # Regional overrides are already flattened; no local filenames travel.
        settings.get("game", {}).pop("default_region", None)
    lines = ["# Browser runtime settings; ROM, BIOS and save paths belong to the web host."]
    for section, values in settings.items():
        if values:
            lines.extend(["", "[" + section + "]"])
            lines.extend(key + " = " + value for key, value in values.items())
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--config", type=Path)
    args = parser.parse_args()
    text = export_config(args.config)
    args.output.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
