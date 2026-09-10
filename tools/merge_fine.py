#!/usr/bin/env python3
"""Merge fine (8^3 hexahedral FEM) mode tables into ModalData.hpp.

The committed ModalData.hpp holds one modal set per body, baked by
tools/modal_bake.py at the default 4^3 grid. This tool runs a SECOND bake at
g=8 (paper-47 multi-resolution FEM, Fig. 6: finer FE resolution -> upward
frequency shift + extended high-frequency content) and appends two arrays per
preset:

    float fineFreq[128];    // 8^3 eigenfrequencies (rad/s), coarse-padded
    float fineDecay[128];   // 8^3 decay rates (1/s), coarse-padded

Modes beyond the fine set's count are padded with the coarse value so a
resolution morph is a no-op there. The coarse numbers are NEVER touched
(bit-identity of the default render is preserved by construction); the tool
verifies the post-merge header is a pure superset of the old one.

Usage: python tools/merge_fine.py   (writes ModalData.hpp in place)
"""
import os, re, sys
import numpy as np
import modal_bake as mb

OUT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..",
                                    "plugins/MultiScaleBody/src/ModalData.hpp"))

def fine_bake(preset, g=8, nmax=128):
    """bake_one() minus the gain grid: we only need freq + decays."""
    K, M, _, _, _, _ = mb.build_grid(g, preset)
    freq, vecs, vals, nrigid = mb.compute_modes(K, M, nmax)
    freq = freq * 0.18
    inharm = {'Bowl':1.02,'WoodBlock':1.08,'Plate':1.18,'Squirrel':1.10,'Blade':1.22,'Shell':1.06,'Bar':1.01,'Membrane':1.15,'Bell':1.04,'Glass':1.12,'Chime':1.015,'Gong':1.09,
              'Handpan':1.03,'LogDrum':1.07,'Marimba':1.05,'Cowbell':1.13,'Kalimba':1.16,'Celesta':1.008}.get(preset['name'],1.0)
    for i in range(len(freq)):
        stretch = 1.0 + (inharm-1.0) * (i/max(1,len(freq)-1)) * 1.5
        freq[i] *= stretch
    alpha1, alpha2 = preset['alpha1'], preset['alpha2']
    decays = 0.5*(alpha1 + alpha2 * (freq**2))
    decays = np.maximum(decays, 0.6)
    sh = {'Plate': (0.7,0.3,1,False), 'WoodBlock': (1.0,0.6,1,True), 'Membrane': (1.0,0.4,1,True),
          'Blade': (0.8,0.2,1,False), 'Bell': (0.65,0.35,0.7,False), 'Glass': (0.55,0.45,0.8,False),
          'Chime': (0.5,0.5,1,False), 'Gong': (0.72,0.28,1,False), 'Handpan': (0.72,0.28,1,False),
          'LogDrum': (1.08,0.38,1,True), 'Marimba': (1.15,0.65,0.8,True),
          'Kalimba': (1.0,0.3,1,True), 'Celesta': (0.6,0.4,1,False)}
    if preset['name'] in sh:
        a,b,p,sub = sh[preset['name']]
        t = np.linspace(0,1,len(decays))
        factor = a + b*((t**p) if sub else (1.0-t)**p)
        decays *= factor
    return np.asarray(freq, dtype=float), np.asarray(decays, dtype=float)

def fmt_arr(vals):
    s = "   {"
    for i in range(128):
        s += f"{float(vals[i]):.6f}f"
        if i != 127: s += ","
        if i % 8 == 7 and i != 127: s += "\n    "
    s += "}"
    return s

def parse_block(text, name, which):
    """Extract the 128-value float block for preset `name` (which: freq|decay)."""
    anchor = f'"{name}",'
    pos = text.find(anchor)
    if pos < 0: sys.exit(f"no anchor {name}")
    # freq block = first '{' after n line; decay = second
    nbr = text.find("{", pos)
    assert nbr >= 0
    if which == "freq":
        start = nbr
    else:
        start = text.find("{", nbr + 1)
    depth = 0; end = None
    for i in range(start, len(text)):
        if text[i] == "{": depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0: end = i; break
    body = text[start+1:end]
    return [float(x.rstrip('fF')) for x in re.findall(r"[-+]?\d*\.\d+f|[-+]?\d+f", body)]

def main():
    if not os.path.exists(OUT):
        sys.exit(f"cannot find {OUT}")
    src = open(OUT, encoding="utf-8").read()
    if "fineFreq" in src:
        sys.exit("ModalData.hpp already contains fine tables — aborting (idempotence guard)")

    fine = {}
    for p in mb.PRESETS:
        print(f"fine-baking {p['name']} g=8 ...", flush=True)
        f, d = fine_bake(p, g=8)
        fine[p['name']] = (f, d)
        print(f"  fine modes={len(f)} fmax={f[-1]/(2*np.pi):.0f}Hz", flush=True)

    new_src = src
    names = [p['name'] for p in mb.PRESETS]
    # "   },\n  }," = gain-grid close + preset close. The freq/decay array
    # closes are followed by "   {" (next array open), so this pair is
    # unambiguous and occurs exactly once per preset.
    PAIR = "   },\n  },"
    for k, name in enumerate(names):
        ff, fd = fine[name]
        n_fine = len(ff)
        cf = parse_block(src, name, "freq")
        cd = parse_block(src, name, "decay")
        ff_pad = [ff[i] if i < n_fine else cf[i] for i in range(128)]
        fd_pad = [fd[i] if i < n_fine else cd[i] for i in range(128)]
        pos = new_src.find(f'"{name}",\n')
        if pos < 0: sys.exit(f"no anchor {name}")
        block_end = (new_src.find(f'"{names[k+1]}",\n') if k+1 < len(names)
                     else len(new_src))
        cand = new_src.rfind(PAIR, pos, block_end)
        if cand < 0: sys.exit(f"no grid/preset pair for {name}")
        # keep the grid close ("   },\n"), then the two fine arrays, then the
        # preset close ("  },") — strictly additive, braces balanced
        ins = "\n" + fmt_arr(ff_pad) + ",\n" + fmt_arr(fd_pad)
        at = cand + len("   },\n")
        new_src = new_src[:at] + ins + new_src[at:]

    # struct gains the two new fields (intentional edit)
    new_src = new_src.replace(
        " float gain[128][16][16];\n};",
        " float gain[128][16][16];\n float fineFreq[128];\n float fineDecay[128];\n};", 1)

    # verification: every coarse freq/decay block must round-trip numerically
    # identical in the NEW text (the golden-identity invariant)
    for p in mb.PRESETS:
        name = p['name']
        if parse_block(new_src, name, "freq") != parse_block(src, name, "freq"):
            sys.exit(f"VERIFY FAILED: coarse freq changed for {name}")
        if parse_block(new_src, name, "decay") != parse_block(src, name, "decay"):
            sys.exit(f"VERIFY FAILED: coarse decay changed for {name}")
    if new_src.count("float fineFreq[128];") != 1:
        sys.exit("VERIFY FAILED: struct fields")
    if new_src.count("   {\n") < 18:
        sys.exit("VERIFY FAILED: fine arrays missing")
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(new_src)
    print(f"merged fine tables into {OUT}")

if __name__ == "__main__":
    main()