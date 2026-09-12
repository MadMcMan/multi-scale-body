# MultiScaleBody

Struck-object modal resonator instrument. A physical body (bowl, plate, bell, gong, ...) is analysed offline into its modal set — frequencies, decays, and per-strike-position gains — and rendered in real time as a bank of reson filters driven by note-on impulses. Based on Picard, Faure, Kry & Drettakis, *A Robust and Multi-Scale Modal Analysis for Sound Synthesis* (DAFx-09, paper 47).

![MultiScaleBody UI](ui_remake_1440.png)

## Signal model

Each retained mode contributes one damped sinusoid:

$$s(t)=\sum_{i=1}^{n} a_i \sin(\omega_i t)\, e^{-d_i t}$$

where $\omega_i$ / $d_i$ are the baked modal frequencies/decays and $a_i$ is the Sound Map gain at the strike point. The plugin ships precomputed modal data (`ModalData.hpp`); the DSP is a per-mode biquad reson bank with strike-position gain interpolation — no runtime FEM.

## Formats

VST3 · CLAP · LV2 · JACK standalone (DPF), with an LVGL-based UI.

## Features

- **12 baked bodies**: Bowl, WoodBlock, Plate, Squirrel, Blade, Shell, Bar, Membrane, Bell, Glass, Chime, Gong — up to 128 modes each
- **Playable strike disc**: click sets strike position (X/Y) and triggers a hit; onset-triggered ripple rings
- **Tone shaping**: tune, decay, brightness, stereo width
- **Exciter**: exciter mix, velocity-to-strike, detune spread, glide, mono mode, LFO (rate/depth), bow/friction excitation (held notes swell instead of decaying — a stick-slip friction bridge feeding the same modal bank, a documented extension: see below)
- **Space**: radiation mix, 16-band output EQ trims, wet/dry
- **Per-band decay trims**: the 16-band spectrum chart scrubs gain (default) or per-band decay via the GAIN/DECAY toggle, so highs can dull while lows bloom
- **Felt damper + half-pedal**: a Damper knob loads a felt strip (frequency-dependent absorption); CC64 is continuous — full pedal defers note-offs, half-pedal deadens the ring
- **Microtonal tuning**: pick an EDO (5, 7, 10, 12, 15, 17, 19, 22, 24, 31, 41, 53, 72) or LOAD a Scala `.scl` file (keyboard row → EDIT); the keyboard remaps chromatically, Tune stays a global offset. Optional kbm-style mapping ("first,last" + optional consecutive note list).
- **Inharmonicity**: one knob stretches partials from the baked pure ratios toward bell-like quadratic spacing
- **MIDI learn**: right-click any knob → move a CC on channel 0 → the binding saves with the patch
- **MPE slide routing**: Slide mode routes per-channel pitch bend to classic whole-voice bend (default), per-mode dispersion bend, or a per-voice brightness macro
- **Physical model strip** (always visible, between stage and keyboard): Rayleigh damping law (αM/βK per the paper's C = αM + βK), boundary support with clamp touch position, mallet head + scrape, FEM-resolution morph (4³→8³ baked tables), body morphing with target dropdown, material physics rescale with Rayleigh defaults, ECO toggle + budget — no modals, everything on one 1440×1068 screen
- **Inline tuning**: EDO selector + LOAD .SCL + CLEAR live in the keyboard header next to the scale readout — no modal editor
- **Non-blocking MIDI learn**: right-click any knob to arm; a keyboard-header chip shows the pending target while knobs stay playable; the chip clears when the CC binds, on X, or on right-click elsewhere
- **Live visuals**: 16-band spectrum, decay scope, mode spectrum chart, on-screen keyboard

### Physical model (PHYSICS strip)

The PHYSICS strip hosts the paper-grounded extensions, always visible between stage and keyboard. Rayleigh sliders add the paper's damping law on top of the baked decays: per-mode rate += ½(α + βω²) with α = A²·10, β = B²·6.3e-6. Support clamps the edge: modes stretch up quadratically (top +25% at full) and tails damp 1.5×; the same knob drives live tails. Sup X/Y move the clamp touch point on the disc: a mode that moves strongly where the hand clamps loses energy into the hand faster (per-mode weight up to +1.5× local rate, armed from the sound map at noteOn, live on moves). Head scales the mallet contact pulse around nominal (softer/longer … harder/shorter); the pulse-shape normalizer re-measures every pulse so the summit stays within 2 dB while the contact spectrum moves. Scrape blends tangential friction chatter into the strike (longer + louder contact transient). Hold Damp deadens strikes near the belly (position-dependent damping, the paper's named gap). Resolution morphs each body between its committed 4³ bake and the new 8³ fine bake (`ModalData::fineFreq/fineDecay`, merged by `tools/merge_fine.py` without touching coarse numbers). Body morph crossfades frequencies, decays and strike gains toward any other baked body (target index clamps to the target's mode count). Material rescales the whole modal set by √((E/ρ)ₘₐₜ/(E/ρ)ᵦₒₔᵧ) across 10 presets (DEFAULT = the body's own material, exact unity) and follows preset changes — and selecting a real material snaps the Rayleigh knobs to that material's baked defaults (DEFAULT leaves them alone). ECO caps each voice's mode count to (budget ÷ live voices) at noteOn — the paper's "resolution adapted to the number of sounding objects". All fourteen default to identity; the golden blob is unchanged.

### Bow / friction excitation

Beyond paper 47's Dirac strike, the Bow knob turns held notes into bowed swells: a velocity-driven stick-slip friction bridge (constant bow speed, static/dynamic friction hysteresis, stick window) excites the *same* modal bank at the strike position, injected through the sustained-drive normalizer so bowed loudness stays Q-independent. Velocities, decay, band trims, felt and the limiter all apply as for struck notes; note-off (or disc release) lifts the bow and the body rings free. Default (bow off) keeps the classic mallet path bit-identical.

### Reverb as self-IR convolution

The paper models the struck body directly and solves no emission/room problem. The plugin's reverb deliberately re-uses the synth's own shaped modal response as the body/space IR: the same per-mode damped sinusoids and strike-position gains that drive the reson bank are rendered into a short stereo IR and baked incrementally on parameter changes (≤16 modes per audio block, converging over ~8 blocks). Normalization is dual — scale by min(peak → 0.8, L1 → 0.85) — because peak-normalization alone bounds nothing: against correlated input (the engine's own ringing output), convolution gain approaches the IR's L1 norm (matched filter), whereas capping L1 bounds the wet path for any input via $|\mathrm{conv}| \le L_1 \cdot \max|x|$. The send mixes as dry·(1 − 0.7·wet) + conv·wet.

## Building

Requires CMake + Ninja and DPF/LVGL under `deps/` (see `BUILD.md`):

```sh
cmake -S . -B build -G Ninja
cmake --build build --target MultiScaleBody-vst3 MultiScaleBody-clap MultiScaleBody-lv2 MultiScaleBody-jack
```

Artifacts land in `build/bin/`.

## Rebaking modal data

`plugins/MultiScaleBody/src/ModalData.hpp` is committed; CMake never regenerates it. To rebake after changing `tools/modal_bake.py`:

```sh
python tools/modal_bake.py -o plugins/MultiScaleBody/src/ModalData.hpp   # needs numpy + scipy
```

## Tests

Two standalone tests, run by hand (see `BUILD.md`):

```sh
g++ -std=c++17 -I plugins/MultiScaleBody/src tests/test_modal_dsp.cpp plugins/MultiScaleBody/src/MultiScaleBodyEngine.cpp plugins/MultiScaleBody/src/OutputLP.cpp -o build/test_modal_dsp.exe && build/test_modal_dsp.exe
g++ -std=c++17 -O1 -DHOST_BINARY -I plugins/MultiScaleBody/src -I deps/DPF/distrho -I deps/DPF/dgl tests/test_preset_regression.cpp build/libMultiScaleBody-dsp.a deps/DPF/distrho/src/DistrhoPlugin.cpp deps/DPF/distrho/src/DistrhoUtils.cpp -DDISTRHO_IS_STANDALONE -o build/test_preset_regression.exe && build/test_preset_regression.exe
```

## References

- C. Picard, F. Faure, P. G. Kry, G. Drettakis, *A Robust and Multi-Scale Modal Analysis for Sound Synthesis*, DAFx-09 — local transcript: [`paper_47.md`](paper_47.md)
- K. van den Doel, P. G. Kry, D. K. Pai, *FoleyAutomatic*, SIGGRAPH 2001 (reson-filter rendering)
- Implementation notes: [`PLAN.md`](PLAN.md) · [`research-plan.md`](research-plan.md) · [`BUILD.md`](BUILD.md)

## License

MIT (see `getLicense()` in `plugins/MultiScaleBody/src/PluginMultiScaleBody.cpp`).
