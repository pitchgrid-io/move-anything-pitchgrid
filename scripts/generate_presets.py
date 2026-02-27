#!/usr/bin/env python3
"""
Generate dsp/pitchgrid_presets.h from pitchgrid-plugin factory presets.

Reads XML preset files from pitchgrid-plugin/Source/FactoryPresets/,
extracts depth/stretch/skew/mode/baseTune, and writes a C header
with a static preset table.

Usage: python3 scripts/generate_presets.py
"""

import os
import sys
import xml.etree.ElementTree as ET
import urllib.parse

PRESET_DIR = os.path.join(
    os.path.dirname(__file__), "..", "..", "pitchgrid-plugin",
    "Source", "FactoryPresets"
)
OUTPUT = os.path.join(
    os.path.dirname(__file__), "..", "dsp", "pitchgrid_presets.h"
)


def parse_preset(filepath):
    tree = ET.parse(filepath)
    root = tree.getroot()

    mode = int(root.get("mode", "0"))

    params = {}
    for p in root.findall(".//PARAM"):
        pid = p.get("id")
        pval = p.get("value", "")
        params[pid] = pval

    depth_f = float(params.get("depth", "3.0"))
    depth = int(depth_f + 0.5)  # round to nearest int
    if depth < 1:
        depth = 1

    stretch = float(params.get("stretch", "1.0"))
    skew = float(params.get("skew", "0.5833"))

    # baseTune is in octaves (log2 units); convert to cents
    base_tune_str = params.get("baseTune", params.get("base_tune", ""))
    if base_tune_str:
        root_freq_cents = float(base_tune_str) * 1200.0
    else:
        root_freq_cents = 0.0

    return {
        "depth": depth,
        "stretch": stretch,
        "skew": skew,
        "mode": mode,
        "root_freq_cents": root_freq_cents,
    }


def clean_name(filename):
    """Decode URL-encoded filename, strip number prefix and extension."""
    name = urllib.parse.unquote(filename.replace(".xml", ""))
    # Strip leading number prefix like "001_"
    parts = name.split("_", 1)
    if len(parts) > 1 and parts[0].isdigit():
        name = parts[1]
    # Replace underscores with spaces
    name = name.replace("_", " ")
    return name


def main():
    preset_dir = os.path.abspath(PRESET_DIR)
    if not os.path.isdir(preset_dir):
        print(f"Error: preset dir not found: {preset_dir}", file=sys.stderr)
        sys.exit(1)

    files = sorted(f for f in os.listdir(preset_dir) if f.endswith(".xml"))
    if not files:
        print(f"Error: no .xml files in {preset_dir}", file=sys.stderr)
        sys.exit(1)

    presets = []
    for f in files:
        name = clean_name(f)
        data = parse_preset(os.path.join(preset_dir, f))
        data["name"] = name
        presets.append(data)

    # Generate C header
    lines = []
    lines.append("/*")
    lines.append(" * PitchGrid Factory Presets — Auto-generated")
    lines.append(f" * Source: pitchgrid-plugin/Source/FactoryPresets/ ({len(presets)} presets)")
    lines.append(" * Do not edit manually. Regenerate with: python3 scripts/generate_presets.py")
    lines.append(" */")
    lines.append("")
    lines.append("#ifndef PITCHGRID_PRESETS_H")
    lines.append("#define PITCHGRID_PRESETS_H")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    const char *name;")
    lines.append("    int    depth;")
    lines.append("    double stretch;")
    lines.append("    double skew;")
    lines.append("    int    mode;")
    lines.append("    double root_freq_cents;")
    lines.append("} pg_preset_t;")
    lines.append("")
    lines.append(f"#define PG_PRESET_COUNT {len(presets)}")
    lines.append("")
    lines.append("static const pg_preset_t pg_presets[PG_PRESET_COUNT] = {")

    for i, p in enumerate(presets):
        comma = "," if i < len(presets) - 1 else ""
        # Escape name for C string
        cname = p["name"].replace('"', '\\"')
        lines.append(
            f'    {{ "{cname}", '
            f'{p["depth"]}, '
            f'{p["stretch"]:.16g}, '
            f'{p["skew"]:.16g}, '
            f'{p["mode"]}, '
            f'{p["root_freq_cents"]:.6f} '
            f'}}{comma}'
        )

    lines.append("};")
    lines.append("")
    lines.append("#endif /* PITCHGRID_PRESETS_H */")
    lines.append("")

    output = os.path.abspath(OUTPUT)
    with open(output, "w") as f:
        f.write("\n".join(lines))

    print(f"Generated {output} with {len(presets)} presets:")
    for i, p in enumerate(presets):
        print(f"  [{i:2d}] {p['name']}: depth={p['depth']} stretch={p['stretch']:.4f} "
              f"skew={p['skew']:.4f} mode={p['mode']} cents={p['root_freq_cents']:.1f}")


if __name__ == "__main__":
    main()
