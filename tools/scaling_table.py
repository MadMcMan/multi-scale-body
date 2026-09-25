#!/usr/bin/env python3
"""Reproduce the structure of paper-47 Table 1: modal-data compute time and
memory vs hexahedral coarse-grid resolution (paper section 5 / Table 1).

The paper reports wall-clock modal-extraction time and the memory of the
stored modal data for its squirrel test model at coarse resolutions
2^3..7^3 (fine level one step finer). This script does the same for one of
our baked bodies using the SAME pipeline (build_grid + compute_modes, fine
level = 2x coarse, matching the paper's "one level up" fine grid).

Stored-modal-data memory model: per retained mode we ship freq (f32) + decay
(f32) + a 16x16 Sound-Map gain grid (256 x f32) = 2*4 + 256*4 = 1032 bytes.
That is the audio-rate payload the plugin carries per body.

Usage:
  python tools/scaling_table.py            # quick smoke (2^3, 4^3)
  python tools/scaling_table.py --full     # the full 2^3..9^3 table
  python tools/scaling_table.py --preset Squirrel --full
"""
import sys, os, time, argparse
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import modal_bake as mb

BYTES_PER_MODE = 2 * 4 + 16 * 16 * 4  # freq + decay + 16x16 gain


def bake_time(preset, g):
    """Bake one resolution (fine=2g) and return (seconds, n_modes)."""
    t0 = time.perf_counter()
    K, M, *_ = mb.build_grid(2 * g, preset)   # fine level, per paper
    freq, _vecs, _vals, _nrigid = mb.compute_modes(K, M, 128)
    dt = time.perf_counter() - t0
    return dt, len(freq)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", default="Squirrel")
    ap.add_argument("--full", action="store_true")
    args = ap.parse_args()
    preset = next((p for p in mb.PRESETS if p['name'] == args.preset), None)
    if preset is None:
        print("unknown preset", args.preset); sys.exit(2)
    grids = [2, 3, 4, 5, 6, 7, 8, 9] if args.full else [2, 4]
    print(f"# Table 1 reproduction: preset={preset['name']}  "
          f"(E={preset['E']:.3g}, nu={preset['nu']}, rho={preset['rho']}, L={preset['L']})")
    print(f"# fine level = 2x coarse (paper: 'fine level one step finer'); "
          f"memory = n_modes * {BYTES_PER_MODE} B stored modal payload")
    print("| coarse | fine  | modes | time (s) | memory (MB) |")
    print("|--------|-------|-------|----------|-------------|")
    for g in grids:
        dt, n = bake_time(preset, g)
        mb_ = n * BYTES_PER_MODE / (1024 * 1024)
        print(f"| {g}^3    | {2*g}^3  | {n:5d} | {dt:8.2f}  | {mb_:11.3f}  |")
    if not args.full:
        print("\n(quick smoke; run with --full for the complete 2^3..9^3 table)")


if __name__ == "__main__":
    main()
