#!/usr/bin/env python3
"""Generate TAMNAMS MOS names and EDO-MOS combination tables."""

import csv
import math
import sys
sys.path.insert(0, "/Users/peter/dev/PitchGrid/scalatrix/venv/lib/python3.12/site-packages")
import scalatrix

# All TAMNAMS MOS names up to 10 steps
# (name, prefix, nL, ns)
MOS_NAMES = [
    # 2-note
    ("trivial",          "triv-",     1, 1),
    # 3-note
    ("antrial",          "atri-",     1, 2),
    ("trial",            "tri-",      2, 1),
    # 4-note
    ("antetric",         "atetra-",   1, 3),
    ("biwood",           "biwd-",     2, 2),
    ("tetric",           "tetra-",    3, 1),
    # 5-note
    ("pedal",            "ped-",      1, 4),
    ("pentic",           "pent-",     2, 3),
    ("antipentic",       "apent-",    3, 2),
    ("manual",           "manu-",     4, 1),
    # 6-note
    ("antimachinoid",    "amech-",    1, 5),
    ("malic",            "mal-",      2, 4),
    ("triwood",          "triwd-",    3, 3),
    ("citric",           "citro-",    4, 2),
    ("machinoid",        "mech-",     5, 1),
    # 7-note
    ("onyx",             "on-",       1, 6),
    ("antidiatonic",     "pel-",      2, 5),
    ("mosh",             "mosh-",     3, 4),
    ("smitonic",         "smi-",      4, 3),
    ("diatonic",         "dia-",      5, 2),
    ("archaeotonic",     "arch-",     6, 1),
    # 8-note
    ("antipine",         "apine-",    1, 7),
    ("subaric",          "subar-",    2, 6),
    ("checkertonic",     "check-",    3, 5),
    ("tetrawood",        "tetrawd-",  4, 4),
    ("oneirotonic",      "oneiro-",   5, 3),
    ("ekic",             "ek-",       6, 2),
    ("pine",             "pine-",     7, 1),
    # 9-note
    ("antisubneutralic", "ablu-",     1, 8),
    ("balzano",          "bal-",      2, 7),
    ("tcherepnin",       "cher-",     3, 6),
    ("gramitonic",       "gram-",     4, 5),
    ("semiquartal",      "cthon-",    5, 4),
    ("hyrulic",          "hyru-",     6, 3),
    ("armotonic",        "arm-",      7, 2),
    ("subneutralic",     "blu-",      8, 1),
    # 10-note
    ("antisinatonic",    "asina-",    1, 9),
    ("jaric",            "jara-",     2, 8),
    ("sephiroid",        "seph-",     3, 7),
    ("lime",             "lime-",     4, 6),
    ("pentawood",        "pentawd-",  5, 5),
    ("lemon",            "lem-",      6, 4),
    ("dicoid",           "dico-",     7, 3),
    ("taric",            "tara-",     8, 2),
    ("sinatonic",        "sina-",     9, 1),
]

# Step ratio combinations: (L_edsteps, s_edsteps, add_depth, Ls_ratio, hardness)
STEP_COMBOS = [
    (1, 1, 0, "1:1", "equalized"),
    (2, 1, 1, "2:1", "basic"),
    (3, 1, 2, "3:1", "hard"),
    (3, 2, 2, "3:2", "soft"),
    (4, 1, 3, "4:1", "superhard"),
    (4, 3, 3, "4:3", "supersoft"),
    (5, 2, 3, "5:2", "semihard"),
    (5, 3, 3, "5:3", "semisoft"),
]

# Hand-crafted presets (original set, always first)
# (name, depth, stretch, skew, mode, root_freq_cents, extra_depth, repetitions)
MANUAL_PRESETS = [
    ("12-TET",                3, 1200.0,  0.5833333134651184, 1,  0.0,      1, 1),
    ("12-TET 432 Hz",         3, 1200.0,  0.5833333134651184, 2, -31.760001,1, 1),
    ("Western 17EDO",         3, 1200.0,  0.58824,            1,  0.0,      2, 1),
    ("Western 19EDO",         3, 1200.0,  0.57893,            1,  0.0,      2, 1),
    ("Western 22EDO",         3, 1200.0,  0.59091,            1,  0.0,      2, 1),
    ("Pythagorean",           3, 1200.0,  0.58497,            1,  0.0,      1, 1),
    ("1/4c Meantone",         3, 1200.0,  0.58050,            1,  0.0,      1, 1),
    ("1/2c Cleantone",        3, 1210.67, 0.57976,            1,  0.0,      1, 1),
    ("Bohlen-Pierce 13EDT",   4, 1901.95, 0.7683332562446594, 5,  0.0,      1, 1),
    ("Dicot 17EDO 7L3s",      4, 1200.0,  0.7058833241462708, 4,  0.0,      1, 1),
    ("Mavila 9EDO",           3, 1200.0,  0.55556,            2,  0.0,      1, 1),
    ("Mavila 16EDO",          3, 1200.0,  0.56251,            2,  0.0,      2, 1),
    ("Orwell 13EDO 4L5s",     4, 1200.0,  0.76923,            5,  0.0,      1, 1),
    ("Orwell 22EDO 4L5s",     4, 1200.0,  0.77273,            5,  0.0,      2, 1),
    ("Porcupine 15EDO 7L1s",  6, 1200.0,  0.86667,            2,  0.0,      1, 1),
    ("Machine 11EDO 5L1s",    4, 1200.0,  0.81818,            0,  0.0,      1, 1),
    ("Machine 16EDO 5L1s",    4, 1200.0,  0.81250,            0,  0.0,      2, 1),
    ("Machine 17EDO 5L1s",    4, 1200.0,  0.82353,            0,  0.0,      2, 1),
    ("Magic7",                3, 1200.0,  0.6827608942985535, 0,  0.0,      1, 1),
    ("Slendric[11]",          5, 1200.32, 0.8052850961685181, 0,  0.0,      1, 1),
]

# Short hardness codes for preset names
HARDNESS_SHORT = {
    "equalized": "eq",
    "basic":     "b",
    "hard":      "h",
    "soft":      "s",
}

# Hardness values included in presets
PRESET_HARDNESS = {"equalized", "basic", "hard", "soft"}


def main():
    # --- Table 1: MOS names ---
    mos_csv = "scripts/data/tamnams_mos_names.csv"
    with open(mos_csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["name", "prefix", "steps", "nL", "ns", "gcd"])
        for name, prefix, nL, ns in MOS_NAMES:
            steps = nL + ns
            g = math.gcd(nL, ns)
            w.writerow([name, prefix, steps, nL, ns, g])
    print(f"Wrote {len(MOS_NAMES)} MOS names to {mos_csv}")

    # --- Pre-compute generator vectors and depth per MOS pattern ---
    gen_cache = {}   # (nL, ns) -> (genL, gens, depth)
    for name, prefix, nL, ns in MOS_NAMES:
        g = math.gcd(nL, ns)
        mos = scalatrix.MOS.fromParams(nL // g, ns // g, 0, 1, 0.5)
        gen_cache[(nL, ns)] = (mos.v_gen.x, mos.v_gen.y, mos.depth)

    # --- Table 2: EDO-MOS combinations ---
    edomos_csv = "scripts/data/tamnams_edomos.csv"
    rows = 0
    with open(edomos_csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["name", "prefix", "steps", "nL", "ns", "gcd",
                     "L_edsteps", "s_edsteps", "add_depth", "Ls_ratio", "hardness", "ed",
                     "genL", "gens", "gen_edsteps", "skew", "depth"])
        for name, prefix, nL, ns in MOS_NAMES:
            steps = nL + ns
            g = math.gcd(nL, ns)
            genL, gens, base_depth = gen_cache[(nL, ns)]
            for L_ed, s_ed, add_depth, ratio, hardness in STEP_COMBOS:
                ed = L_ed * nL + s_ed * ns
                gen_edsteps = genL * L_ed + gens * s_ed
                ed_per_period = ed // g
                skew = gen_edsteps / ed_per_period
                depth = base_depth
                w.writerow([name, prefix, steps, nL, ns, g,
                            L_ed, s_ed, add_depth, ratio, hardness, ed,
                            genL, gens, gen_edsteps, f"{skew:.6f}", depth])
                rows += 1
    print(f"Wrote {rows} EDO-MOS combinations to {edomos_csv}")

    # --- Table 3: Presets (manual + generated) ---
    presets = []

    # Manual (hand-crafted) presets first
    for mp in MANUAL_PRESETS:
        presets.append({
            "name": mp[0],
            "depth": mp[1],
            "stretch": mp[2],
            "skew": mp[3],
            "mode": mp[4],
            "root_freq_cents": mp[5],
            "extra_depth": mp[6],
            "repetitions": mp[7],
        })

    # Generated TAMNAMS presets (filtered: ed>=5, eq/b/h/s only)
    for name, prefix, nL, ns in MOS_NAMES:
        g = math.gcd(nL, ns)
        genL, gens, base_depth = gen_cache[(nL, ns)]
        for L_ed, s_ed, add_depth, ratio, hardness in STEP_COMBOS:
            if hardness not in PRESET_HARDNESS:
                continue
            ed = L_ed * nL + s_ed * ns
            if ed < 5:
                continue
            gen_edsteps = genL * L_ed + gens * s_ed
            ed_per_period = ed // g
            skew = gen_edsteps / ed_per_period
            depth = base_depth
            hs = HARDNESS_SHORT[hardness]
            preset_name = f"{prefix}{ed}EDO {nL}L{ns}s {hs}"
            presets.append({
                "name": preset_name,
                "depth": depth,
                "stretch": 1200.0,
                "skew": skew,
                "mode": 1,
                "root_freq_cents": 0.0,
                "extra_depth": add_depth,
                "repetitions": g,
            })

    # Write presets header
    presets_h = "dsp/pitchgrid_presets.h"
    with open(presets_h, "w") as f:
        f.write("/*\n")
        f.write(" * PitchGrid Factory Presets — Auto-generated\n")
        n_manual = len(MANUAL_PRESETS)
        f.write(f" * {n_manual} hand-crafted + {len(presets) - n_manual} TAMNAMS EDO-MOS = {len(presets)} total\n")
        f.write(" * Do not edit manually. Regenerate with: python3 scripts/generate_tamnams_tables.py\n")
        f.write(" */\n\n")
        f.write("#ifndef PITCHGRID_PRESETS_H\n")
        f.write("#define PITCHGRID_PRESETS_H\n\n")
        f.write("typedef struct {\n")
        f.write("    const char *name;\n")
        f.write("    int    depth;\n")
        f.write("    double stretch;\n")
        f.write("    double skew;\n")
        f.write("    int    mode;\n")
        f.write("    double root_freq_cents;\n")
        f.write("    int    extra_depth;\n")
        f.write("    int    repetitions;\n")
        f.write("} pg_preset_t;\n\n")
        f.write(f"#define PG_PRESET_COUNT {len(presets)}\n\n")
        f.write(f"static const pg_preset_t pg_presets[PG_PRESET_COUNT] = {{\n")
        for i, p in enumerate(presets):
            comma = "," if i < len(presets) - 1 else ""
            f.write(f'    {{ "{p["name"]}", {p["depth"]}, {p["stretch"]:.1f}, '
                    f'{p["skew"]:.6f}, {p["mode"]}, {p["root_freq_cents"]:.6f}, '
                    f'{p["extra_depth"]}, {p["repetitions"]} }}{comma}\n')
        f.write("};\n\n")
        f.write("#endif /* PITCHGRID_PRESETS_H */\n")
    print(f"Wrote {len(presets)} presets to {presets_h}")


if __name__ == "__main__":
    main()
