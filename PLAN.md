# Multi‑Scale Body — VST Implementation Plan

## 1. Overview
Multi‑Scale Body is a struck‑object resonator instrument based on Picard, Faure,
Kry & Drettakis' *Robust and Multi‑Scale Modal Analysis for Sound Synthesis*.
A linear elastic body (metal bowl, squirrel, etc.) is voxelized into a regular
grid, its mass/stiffness matrices assembled by finite elements, and the damped
response decoupled (under Rayleigh damping) into $n$ independent single‑DOF
oscillators. Each mode contributes one damped sinusoid
$s(t)=\sum_i a_i\sin(\omega_i t)\,e^{-d_i t}$. The plugin precomputes a fixed
reference mesh's modal set offline and renders it in real time as a bank of
reson filters (per van den Doel et al.) driven by an excitation impulse at a
user‑positioned strike point — the *Sound Map* $a_i$. For the VST we ship a
fixed precomputed geometry (e.g. a steel bowl) and expose material/geometry
scaling, damping, strike position and mode count. *Instrument* (triggered by note‑on).

## 2. Paper & References
- **Title:** A Robust and Multi‑Scale Modal Analysis for Sound Synthesis
- **Authors:** Cécile Picard, François Faure, Paul G. Kry, George Drettakis (INRIA / McGill)
- **Venue/Year:** DAFx‑09, Como, 2009, paper 47.
- **Reference list (selected from paper):**
  - O'Brien, Shen, Gatchalian, "Synthesizing sounds from rigid‑body simulations," SCA'02 [1].
  - Adrien, "The missing link: modal synthesis," 1991 [2].
  - Cook, *Real Sound Synthesis for Interactive Applications*, 2002 [3].
  - van den Doel, Kry, Pai, "Foley automatic," SIGGRAPH'01 [6].
  - Maxwell & Bindel, DAFx'07 [7]; Bonneel et al., "Fast modal sounds," SIGGRAPH'08 [8].
  - Bathe, *Finite Element Procedures in Engineering Analysis*, 1982 [9] (Rayleigh damping, FEM).
  - Nesme, Payan, Faure, VRIPHYS'06 [10]; James, Barbic, Pai, "Precomputed acoustic transfer," TOG 25(3) [12].
  - Nesme, Kry, Jeřábková, Faure, SIGGRAPH'09 [14].
- **Paper markdown:** [paper_47.md](./paper_47.md)

## 3. DSP Mathematics
Verified against `paper_47.md` (Eq. 1 + prose; several forms are RECONSTRUCTED per paper — no displayed equations given).

- **(Eq. 1) Modal synthesis.**
  $s(t)=\sum_{i=1}^{n} a_i\sin(\omega_i t)\,e^{-d_i t}$.
  $s(t)$ emitted sound; $n$ retained modes; $\omega_i$ natural angular frequency (rad/s); $d_i$ decay rate (1/s); $a_i$ gain at strike point (the Sound Map); excitation ≈ Dirac impact; $a_i=0$ at a vibration node of mode $i$.

- **(RECONSTRUCTED) Continuum dynamics.**
  $\mathbf M\ddot{\mathbf u}+\mathbf C\dot{\mathbf u}+\mathbf K\mathbf u=\mathbf f(t)$;
  $\mathbf M,\mathbf C,\mathbf K\in\mathbb R^{n\times n}$ mass/damping/stiffness; $\mathbf u$ DOF displacements; $\mathbf f$ nodal forces (Dirac impacts).

- **(prose, [9] Bathe) Rayleigh damping.** $\mathbf C=\alpha_1\mathbf M+\alpha_2\mathbf K$;
  $\alpha_1$ (1/s, mass), $\alpha_2$ (s, stiffness). Decouples the damped system; uniform damping.

- **(RECONSTRUCTED, Algorithm 1) Generalized eigenproblem.**
  $\mathbf K\mathbf x_i=\lambda_i\mathbf M\mathbf x_i$, $\lambda_i=\omega_i^2$; $\mathbf x_i$ mode shape. Each free body has **six zero eigenvalues** (rigid‑body freedoms), dropped.

- **(RECONSTRUCTED) Fine‑to‑coarse averaging.** Coarse cell $c$ from children $e_1..e_8$:
  $\mathbf K_c=\sum_{e}w_e\mathbf K_e,\ \mathbf M_c=\sum_{e}w_e\mathbf M_e,\ \sum w_e=1$;
  $w_e$ = material‑distribution weights.

- **(RECONSTRUCTED) Sound Map** via trilinear interpolation of mode shapes at cell‑local $(\xi,\eta,\zeta)\in[0,1]^3$:
  $\mathbf u=\sum_{i,j,k\in\{0,1\}}N_i(\xi)N_j(\eta)N_k(\zeta)\,\mathbf u_{ijk}$, $N_0(t)=1-t,\ N_1(t)=t$.

- **Rendering:** per‑mode **reson filter** (van den Doel et al. [6]); no radiation model.

*Flag:* Element matrices (8‑node hexahedral $\mathbf K_e,\mathbf M_e$), coarse‑cell weighting rule, and exact Sound‑Map amplitudes are not given in closed form — RECONSTRUCTED from standard FEM; offline precompute resolves them for the shipped mesh.

## 4. Parameters
| Name | Symbol | Norm range | Default | Mapping | Unit |
|---|---|---|---|---|---|
| Pitch / Size | — | 0..1 | 0.50 | log scale factor 0.5..2.0 (shifts all $\omega_i$ by $\sqrt{E/\rho}$) | — |
| Stiffness | $E$ | 0..1 | 0.50 | log 10e9..200e9 | Pa |
| Rayl. mass $\alpha_1$ | $\alpha_1$ | 0..1 | 0.30 | log 1..200 | 1/s |
| Rayl. stiff $\alpha_2$ | $\alpha_2$ | 0..1 | 0.30 | log 1e‑7..1e‑5 | s |
| Strike X | $x_0$ | 0..1 | 0.50 | lin across mesh X | — |
| Strike Y | $y_0$ | 0..1 | 0.50 | lin across mesh Y | — |
| Mode Count | $n$ | 0..1 | 0.70 | lin 8..128 retained modes | — |
| Excite | — | trig | — | note‑on impulse | — |

## 5. DSP Module Design
- **Offline precompute (`ModalBodyBuilder`):** voxelize a fixed steel bowl mesh at a chosen coarse grid; assemble $\mathbf K,\mathbf M$; partial generalized eigendecomposition (Lanczos); drop 6 rigid modes; store $(\omega_i,d_i,\mathbf x_i)$. Compute Sound‑Map gains $a_i(x_0,y_0)$ by trilinear interpolation.
- **`MultiScaleBodyEngine`:** bank of `n` reson filters (biquad, per‑mode $\omega_i,d_i$). On note‑on, inject a unit impulse scaled by $a_i$ into each mode; per‑sample IIR update sums to output.
- **Per‑sample `process()`:** for each mode $y_i += b_0 x_i - a_1 y_{i,1} - a_2 y_{i,2}$; output $\sum_i y_i$.
- **Sample‑rate handling:** $\omega_i,d_i$ fixed at design $f_s$ (44.1 kHz); resample mode frequencies on `sampleRateChanged` via ratio.
- **Stability:** reson filters are stable for $d_i>0$; normalize $a_i$ so peak output ≈ −6 dBFS.
- **Deps reuse:** Catch2 (tests); DPF; LVGL9+dpf‑widgets. Eigen‑style eigendecomposition done offline in a small custom Lanczos (no external lib).

## 6. GUI Plan
- **Root:** `COL_BG`, flex column; title "MULTI‑SCALE MODAL BODY" (`COL_TITLE`), subtitle "Picard/Faure/Kry/Drettakis · DAFx‑09".
- **Panel:** `knobsRow()` of `createArcKnob` for Pitch/Size, Stiffness, α1, α2, Strike X, Strike Y, Mode Count.
- **Card "MODE SPECTRUM" (`card()`):** `lv_chart` (BAR, `COL_CHART_BG`) showing the retained modal frequencies $\omega_i$ and per‑mode decay $d_i$ (two `COL_HIGHLIGHT` series), updating as Pitch/Stiffness/ModeCount change.
- **Card "SOUND MAP" (`card()`):** `lv_chart` (LINE) of $a_i$ vs. strike position for the current $(x_0,y_0)$ marker — a 1‑D slice of the Sound Map.
- Live animation: 33 ms `lv_timer` reads `std::atomic` excitation envelope to drive a decay scope.

## 7. Implementation Steps
1. Offline `ModalBodyBuilder`: hexahedral element matrices, assembly, Lanczos eigendecomposition, rigid‑mode drop, Sound‑Map interpolation. Validate against paper's bowl/squirrel numbers.
2. `MultiScaleBodyEngine`: reson‑filter bank, impulse injection, per‑sample sum.
3. `PluginMultiScaleBody`: enum params, `initParameter`, note‑on trigger.
4. `DistrhoPluginInfo.h`: category `PLUGIN_CATEGORY_SYNTH`.
5. `PluginUI.cpp` + `ui/`: knobs + two chart cards.
6. `tests/test_body_dsp.cpp`: mode‑count invariant (6 dropped), spectrum consistency, Sound‑Map null test at a node.
7. `tests/render_body_ui.cpp`: headless BMP + chart state asserts.

## 8. Build Integration
- CMake: `dpf_add_plugin(MultiScaleBody ...)`; `add_subdirectory(deps/lvgl)`; link `dpf-widgets`, `deps/DPF`.
- `DistrhoPluginInfo.h`: `pluginName "MultiScaleBody"`, `vendor "cymbals"`, `category PLUGIN_CATEGORY_SYNTH`, `guiMode PLUGIN_GUI_MODE_NATIVE`.
- Format: **instrument** (note‑on triggered).

## 9. Test & Verification
- Mode‑count invariant: exactly 6 rigid modes dropped; retained $\lambda_i>0$.
- Spectrum consistency: eigen frequencies $\omega_i$ coincide with spectral peaks of synthesized impulse response; decay matches $e^{-d_i t}$.
- Sound‑Map null: striking a modal node suppresses that mode's spectral peak.
- Material scaling: frequencies scale as $\sqrt{E/\rho}$ under isotropic rescaling.
- Rayleigh identity: single‑mode damped frequency $=\omega_i\sqrt{1-\zeta_i^2}$, $\zeta_i=d_i/\omega_i$.
- Positivity: $\mathbf M\succ0$, $\mathbf K\succeq0$ (6 null dirs) for the voxelization.

## 10. Risks / Open Questions
- Hexahedral element matrices and coarse‑cell weighting rule are paper‑omitted → derive from standard FEM and verify against the bowl/squirrel tables.
- Convergence theory is empirical (4³ "converged"); frequency‑shift vs. resolution not analytically characterized.
- Real‑time FEM is impossible → precompute a fixed mesh; user "Pitch/Stiffness" are global scalings only, not true re‑meshing.
- Damping decoupling validity under uniform Rayleigh damping for mixed materials is approximate.
