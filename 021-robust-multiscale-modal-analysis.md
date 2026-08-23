# A Robust and Multi-Scale Modal Analysis (2009) — VoxelBowl
Category: Synthesis & Sound Generation | PDF: pdfs\2009\paper_47.pdf | Authors: Cécile Picard et al.
Seed idea: A modal-percussion VST that turns any 3D mesh (even thin-bladed, non-manifold) into a physical impact instrument: automatic voxelization → hexahedral FEM → mode bank; hit a point on the surface, hear the object's location-dependent "sound map".

## Core Math
**Modal model.** Sound = sum of damped sinusoids, one per vibration mode:
$$s(t) = \sum_{i=1}^{n} a_i \sin(\omega_i t)\, e^{-d_i t} \tag{1}$$
$\omega_i$ = natural frequency, $d_i$ = decay rate, $a_i$ = gain of mode $i$ at the pickup; $a_i$ depends on where the object is struck (a mode at a vibration node of the impact point stays silent) — the *Sound Map*. Rayleigh damping decouples the damped system (per [1], O'Brien et al.). Rendering via reson filters (van den Doel et al. [6]).

**Hexahedral FEM via voxelization.** Instead of tetrahedralization (slow, fragile on thin/non-manifold geometry), build a high-resolution voxelization of the surface model (sparse regular grid embedding; interior filled for solids), then recursively merge voxels to an arbitrary coarse mechanical resolution; merged voxels become hexahedral finite elements. Mass/stiffness of a merged voxel = weighted average of its 8 children's, weighted by material distribution in each cell (automatic parameter tuning). Assemble global $\mathbf{M}$, $\mathbf{K}$; solve the generalized eigenproblem $\mathbf{K}\mathbf{x} = \lambda \mathbf{M}\mathbf{x}$; drop the six zero eigenvalues (rigid-body modes). Surface points move by trilinear interpolation of mechanical DOFs, so fine visual meshes work with coarse mechanical grids. SOFA framework used for simulation/animation coherence.

**Results.** Metal bowl (274 verts): tetrahedral 822 modes took 5 min (2.33 GHz Core Duo); hexahedral 2×2×2 grid → 81 modes (75 used) — audibly similar with ~10× fewer modes. Aluminum: $E = 69\times10^9$, $\nu = 0.33$, $\rho = 2700$ kg/m³; Rayleigh stiffness 3×10⁻⁷, mass 10. Squirrel (999 verts, pine $E=12\times10^9$, $\nu=0.3$, $\rho=750$, damping 8×10⁻⁶ / 50): grids 2³→9³; frequency content converges by 4×4×4; resolution raises both frequency range and frequency shift. Cost/memory (unoptimized): 7³: 115 s / 11.3 MB; 6³: 39 s / 5.7 MB; 5³: 13 s / 2.7 MB; 4³: 5.4 s / 1.0 MB. Thin parts (blades, wings) excite higher frequencies. Lower resolutions flatten the sound-map variation (limitation).

## VST Architecture
```
load mesh (OBJ) ─▶ [offline worker] voxelize (fine) → merge (coarse grid)
      → assemble M,K → eigen-solve → mode bank {ω_i, d_i, a_i(x,y,z)}
      where a_i(surface point) = trilinear-interpolated mode shape at point
trigger (audio envelope, MIDI note, or sidechain hit) ─▶ impulse/force profile
      ─▶ per-mode damped sine (or 2nd-order reson) bank ─▶ Σ ─▶ out
UI: 3D view — click impact point; sound map overlay (mode gains vs location);
      mode browser (freq list, decay, per-mode mute)
```
- Mode bank: n = 50–500 modes typical; per hit, reset each mode phase/amplitude: $x_i(t) = a_i F \sin(\omega_i t)e^{-d_i t}$ — per-mode state = 2 floats; SIMD-friendly.
- Pre-analysis runs on a worker thread (seconds–minutes depending on grid); ship preset banks (bowl, squirrel, blades) so the plugin works instantly.
- Live knobs (no reanalysis): global pitch scale (ω_i×k), decay scale (d_i×k), impact gain; grid resolution requires re-run.
- Stereo: two pickups (independent a_i vectors) or pan per mode group.
- Latency: impulse response is generated per trigger — zero added latency; audio-input mode converts input into a force profile (input × impact-point gain, sum over modes).

## Parameters & Controls
| Knob | Range | Default | Maps to |
|---|---|---|---|
| Geometry | list/load OBJ | bowl | mesh → voxelize → modes |
| Grid resolution | 2³–9³ | 4³ | hexahedral FEM coarseness (re-run) |
| Mode count | 10–all | 75 | resonators used |
| Pitch scale | 0.25–4 | 1 | ω_i scaling (live) |
| Decay scale | 0.1–10 | 1 | d_i scaling (live) |
| Impact point | 3D click | center | a_i = mode shape at point |
| Material | presets | aluminum | E, ν, ρ, Rayleigh α₁,α₂ |
| Trigger mode | MIDI/audio/env | MIDI | hit excitation |

## Implementation Plan
1. **Prototype (Python):** hexahedral FEM (trilinear hex elements) on a voxelized sphere/plate; eigen-solve (scipy); verify mode frequencies vs. analytic plate; implement Eq. 1 rendering; reproduce bowl-like sound-map variation (3 impact points → different spectra). Risk: eigen-solver memory at 9³ — start 4³.
2. **Real-time port (C++):** voxelizer + FEM assembly + eigen (ARPACK/spectra); mode-bank renderer (SSE); worker-thread pre-analysis. Milestone: 4³ grid analysis < 10 s; 200 modes rendering < 5% CPU.
3. **UI:** 3D mesh + clickable surface, sound-map heat overlay, mode list with solo/mute.
4. **Testing:** impulse → IR with peaks at ω_i; decay matches e^{−d_i t}; impact at a modal node suppresses that mode (spectral null check); grid convergence (4³ vs 8³ spectra similar, per paper Fig. 6).

## Risks & Open Questions
- Non-linear effects (large deformation) excluded — linear modal only.
- Uniform damping assumption (no contact-position damping); no radiation modeling.
- Very coarse grids (2³) distort the sound map (paper limitation) — enforce ≥ 4³ default.
- FEM assembly details (hex element matrices) not in paper — needs standard hex FEM formulation (e.g. Zienkiewicz).

## References
Picard, Faure, Kry & Drettakis, DAFx'09; extra material at INRIA REVES. O'Brien, Shen & Gatchalian, SCA'02 (modal synthesis for rigid bodies); van den Doel, Kry & Pai, SIGGRAPH'01 (FoleyAutomatic); Nesme, Payan & Faure, VRIPHYS'06 (non-uniform stiffness embedding); Nesme et al., SIGGRAPH'09 (topology/elasticity preserving embedding); Bonneel et al., SIGGRAPH'08 (fast modal sounds); Bathe, *Finite Element Procedures*.
