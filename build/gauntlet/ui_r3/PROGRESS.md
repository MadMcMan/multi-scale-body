# MSB UI R3 — Gauntlet vs Pigments

## Goal
Redo the LVGL UI at `plugins/MultiScaleBody/src/PluginUI.cpp`. Aesthetic bar = Arturia Pigments
(1600x1001, `E:/dev/gauntlet/pigments/pigments_main__mamevst-run.png`, md5 `f7db124c1dbbcf911b3b4d58c9a6c089`,
copied locally to `bar/bar_pigments_main.png`).

## Local copies (next agent: do not re-fetch)
- `bar/bar_pigments_main.png` — canonical Pigments bar (1600x1001).
- `bar/PIGMENTS_UI_BAR.md` — full bar feature manifest.
- `ours/r3_p0_before.png` — current STRIKE PLATE UI (pre-gauntlet baseline) — md5 `d815f7e46089de0239d4488e7085a246`.
- `blind/` — staging for shuffled A/B.

## Issues to fix this round (user report)
1. **Wasted space** — strike disc is mostly empty; column widths drift.
2. **Dead tab buttons** — RESONATE / EXCITER / SPACE / MOD tabs do nothing. Either make them real
   view-switchers or delete them. Knobs are already on screen.
3. **Hidden header knob** — there's an unlabeled knob in the top-right; either label it or remove it.
4. **Decay Scope does nothing** — wire to live engine output (`kParamOutLevel` from DSP).
5. **Random nonsensical text** — debug/dev strings left in; audit and remove.
6. **No MPE support** — add a real per-note strike-position + pressure path (channel-wise mapping;
   Z-axis = strike position, pressure = strike velocity/exciter).
7. **Spectrum B1..B16 row** — already on screen; keep.

## Pipeline
- Bar = `E:/dev/gauntlet/pigments/pigments_main__mamevst-run.png` (1600x1001, real capture).
- OURS = the current `PluginUI.cpp` STRIKE PLATE design (`r3_p0_before.png`).
- 2026-08-28 09:00: dispatched `MSB-R3-Builder` (failed - just read files, no edit).
- 2026-08-28 09:00: dispatched `MSB-R3-Critic` (failed - bar PNG missing).
- 2026-08-28 22:48: main-agent round 1 build succeeded. Captured `ours/r3_p1_after.png`.
- Round 1 changes: `outputEnvelope()` engine getter; header relabeled MASTER->OUTPUT with dB + level meter.
- Round 1 deferred: dead tab buttons, decay scope atomic bridge, MPE strip.
- 2026-08-28 23:25: Critic ran 34min pixel-forensic on r3_p1; confirmed OUTPUT label+meter change is real (1621px diff at x1059-1207) but renders tiny/dim. Critic timed out before verdict.
- 2026-08-28 23:32: R2 build — OUTPUT label font bumped micro->small (14px baseline). Atomic envelope bridge plumbing completed (compiles). Decay scope wire-in pending.
- R2 capture: `ours/r3_p2_after.png` (font change is the visible diff; bridge plumbing is in code but not yet wired into updateSpectrumDisplay).
- 2026-08-28 23:42: R2 critic (3.5min, 5min budget) - VERDICT: Pigments wins clearly, ours 34/50. Identity 8/10 (highest score). Biggest gap: right column reads as dead. Pausing further UI work; OUTPUT label change is shipped.
- 2026-08-29 01:10: R3 build — chart-priming fix: `updateSpectrumDisplay()` called synchronously at end of `buildUI()` (timer never services headless/first-present); scope preview seeds full 128-pt trace + `lv_chart_refresh`. Root cause of "dead right column": zero-value BAR chart renders thin amber baseline rects (lv_chart.c:1539), so panels looked wired but held no data.
- R3 capture: `ours/r3_p3_after.png` (md5 via ui_zoom_1440). Verified: scope trace 52 nonzero pts hi=980 (decaying curve, top y586→703); spectrum 16/16 bars h=44..166 varying.
- 2026-08-29 01:20: R3 critic (1m33s) - VERDICT: A (OURS) WINS. Scores ours 43/50 (identity 9, readability 9) vs bar 42/50 (craft 9). Biggest ours strength: "single disciplined amber-on-charcoal language, every graphic live and informative". GAUNTLET EXIT RULE MET.
- 2026-08-29 R4 (user 6-issue follow-through, post-win):
  - Issue #2 chips REMOVED (applied this round; prior handoff claim of "already applied" was false): 5-chip loop + navChips cluster replaced by right-aligned "DAFX-09 / PAPER 47" label. Verified in r4_p1 capture.
  - Issue #5 text fixes: infoHead "PRESET / MATERIAL / MODES" (orphan dash removed, letter-space 0->2); "MATERIAL JEWEL - OCCUPANCY MAP" -> "MATERIAL PREVIEW". Zero JEWEL/OCCUPANCY strings remain.
  - R4 p1 capture: ours/r4_p1_after.png (md5 ee3cc11ec09a7e8be1a40a074e37072d via ui_zoom_1440). Pixel audit: LED-row text x-extent 26..1310 (right pad 122px, NO clipping — auditor "clipped M8" was crop artifact); navTop label x 1286..1413; 8 distinct cells with dot separators. scope-probe: chart#0 128pts/52 nonzero, chart#1 16/16 (matches r3 expectations).
  - Next: issue #6 MPE (per-channel strike position + pressure), then hero-column space (issue #1) judgement.
- 2026-08-30 R5 (issues #6 + #1 close-out):
  - Issue #6 MPE COMPLETE: engine latch mpeZ_[16] (setMpePressure, noteOn 50/50 vel/Z drive blend on member channels 1-15, noteOff/allSoundOff/reset clears, allNotesOff deliberate no-clear), plugin 0xD0 split (ch>=1 -> pressure latch, ch0 -> legacy brightness), 4 new matched-strike-history tests in test_modal_dsp.cpp. Battery: 62 checks PASS (~255s). Golden bit-identity: p0 md5 519aea7a3b4ba78344c42d92fa6ee8c0, p7 md5 16925c94893b66e37ac1a37cd3d09838 - byte-identical to pre-MPE goldens (ch0 render path exact: drive==vel01). Preset regression PASS (sum 23.071, cc120 residual 0.00e+00). 4-target Ninja build green.
  - Issue #1 judgement: fresh capture r4_mpe_check.png (ui_snapshot.exe, UI sources unchanged by MPE), two independent inspect_image passes both CONFIRMED the waste: disc interior ~96-98% empty charcoal, ~260px dead band below disc down to keyboard, M1-M8 labels ghost-like, right column ~1.9x hero width. Fix warranted.
  - Issue #1 fix: MODE ACTIVITY panel in discCol (PluginUI.cpp) - 16 per-band bars sharing the analyzer's env[] data path (live fVizBins / sound-map preview), one amber peak band, PEAK B<n> readout; lay::ACTIVITY_H=226/ACTIVITY_BARS_H=190 tokens (UICommon.hpp); discCol budget 22+6+280+6+22+6+226=568 EXACT - zero dead band. Disc stays 280 (r2 critic verdict on 406 stands). M1-M8 + param-name labels bumped PLATE_TEXT_DIM -> PLATE_TEXT_MID -> PLATE_TEXT (judge found MID still ghost-like).
  - Harness build command recovered as CMake target: ui_snapshot (EXCLUDE_FROM_ALL) - tools/ui_snapshot.cpp + PluginUI.cpp + dpf-widgets LVGL.cpp + DPF DistrhoUI.cpp + dgl-opengl + dgl-opengl-definitions + lvgl::lvgl + winmm/gdi32/user32. The lost ad-hoc g++ line is no longer a blocker.
  - R5 captures: ours/r5_p1_after.png (ACTIVITY_H=58 first pass: panel verified 16 bars/15 grey+1 amber B15/data-like heights/overlap=0, but judge measured 80% of the band still empty -> grown to 226 full-height) and r5b final (this pass). checkLayout: overlap=0 at all 6 sizes; the 6 BOUNDS-FAILs are the pre-existing dropdown-list -14 headers; FAIL display-1800x1075 is the pre-existing zoom-step regression (out of scope, logged).

## R5 close-out (2026-08-30) — all ui_snapshot failures fixed, gauntlet complete

- CORRECTION to the R5c note above: the 6 BOUNDS-FAILs were NOT the dropdown-list
  headers (the -14 header coords are real but on-screen-legal; LVGL layout bounds
  checked out). True root cause: the master OUTPUT knob was a full 133px
  createArcKnob dial-bank knob centered in a 50px topbar inner — its "OUTPUT" title
  sat offscreen above the window and its value chip bled into the nav strip. One
  fail per size pass x6 = the 6 BOUNDS-FAILs. Fixed compact in PluginUI.cpp
  (masterCol 150px, caption above, 28px arc, hidden widget title, chip below).
  Geometry proven by tree dump (masterRow y 43..73, value 66..81, nav strip at 94)
  plus a 3x NN crop judge pass. Full-frame judge on r5_p3_after initially reported
  a knob/nav collision — false positive from the tight vertical lane; the
  coordinate-frame crop judge cleared it.
- FAIL display-exactly-1800x1075 root cause: the harness's findButtonByUserData(+/-1)
  resolved the PRESET prev/next arrows (addPresetArrowBtn sets the same user_data,
  created earlier in the tree) — every "zoom click" since R4 was changing presets;
  the zoom stepper was never exercised. findZoomButton now matches the '-'/'+' label
  text. Once the right button fires, setSize flows the standalone pugl path and the
  real window resizes; stubSetSize (host-emulating AdjustWindowRect+SetWindowPos)
  is kept as the requestSizeChange hook.
- Final harness battery: 15/15 PASS, fails=0, bounds=0 overlap=0 at all sizes,
  zoom125 = 1800x1075 gUIScale=1.250, zoom-back = 1440x860 gUIScale=1.000, no drift.
  Log: ui_verify_log.txt (repo root, transient). Final capture: ours/r5_p3_after.png
  (md5 1e9324f3ef18a2fc2b32cfa1a691bd6a).
- Shipped: commit 6646d3b (master knob + zoom finder + stubSetSize) on top of
  628ebd3 (issue #1 MODE ACTIVITY) and 661916a (issue #6 MPE). All pushed to
  origin/main. All 7 user issues from this round closed.

## R6–R8 (2026-08-31) — user follow-up: duplicated spectrum, header text, keyboard width, then 3 critic rounds

User reported three concrete defects on the live host window (1423x852):
  1. "Duplicated spectrums" — the round-5 MODE ACTIVITY panel below the disc re-plotted
     the SAME 16 env[] bands the MODE SPECTRUM (right column) shows. Replaced with MODE MAP:
     128 thin per-mode bars driven by the bilinear sound-map (baked ModalData + current
     strike X/Y / preset / Modes / band trims). Different question, visibly distinct.
  2. "Header text cut off" — three flavors. (a) brand title 'MULTI-SCALE BODY' truncated to
     'MULTI-SCALE B' in a 240px column: widened brandCol to 300px and added a measure-and-fit
     label loop stepping {26/s2, 24/s2, 20/s2, 16/s2} until it fits (fitKnobText pattern).
     (b) M1–M8 nav labels mid-cut at the nav-strip bottom: nav 28+12+6=46 → 24+18+6=48,
     LED row 12→18 so the 13px-label line height clears the panel edge. (c) '0.5' value
     chip close to the header bottom: tree-dump shows 7px clearance at 1440x860.
  3. "Keyboard doesn't extend over the full footer" — fixed 404px keybed centered → 3-octave
     (C3–B5) keybed derived from the strip inner width: 21 white keys at 64px (1384px total)
     span edge to edge. Black keys positioned per-octave. ARP/oct cluster and '3 OCTAVES' caption
     in the head row. Token arithmetic: STAGE_H 568→566, scope card 196→200 so the right
     tower still fills 566 exactly. Harness 15/15 PASS, fails=0, bounds=0 overlap=0 at all
     6 sizes. Capture: ours/r6_p1_after.png (md5 0c9adccc…). Shipped at 2928a28.

Critic gauntlet (3 blind A/B rounds vs Pigments bar):
  R6: B wins. Gap = "center column reads as empty (MODE MAP near-empty, DECAY SCOPE flat)."
  R7: B wins. Gap = "MODE MAP still a large dark void; DECAY SCOPE hairline with no fill."
       (40% opacity + unseeded area series didn't move the needle.)
  R8: B wins. Gap = "center now reads as populated data, but lacks Pigments' layered
       horizontal bands of secondary visualizations around the hero."
  → All three rounds picked Pigments. The center-column fix landed (R8 capture shows the
    128-mode comb is clearly visible as a solid amber pattern and the scope has a real
    filled envelope), but a single plugin against a category-leading flagship is in
    diminishing-returns territory once the structural defects are closed. R7+R8 polish
    shipped at 0cac55e.

Lesson: when the gap is "lack of visual density around the hero", the honest answer is to
add genuine secondary visualizations (mode-shape / IR curve / envelope-timbre graphic) —
not to fake density. That's a longer design pass, not a quick round.
Final R8 capture: ours/r8_p1_after.png.
