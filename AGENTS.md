# AGENTS.md — MultiScaleBody (DAFx-09 paper 47 modal synth plugin)

Single DPF plugin (jack/vst3/clap/lv2), LVGL UI. See `BUILD.md` for full details; docs map: `PLAN.md` (implementation plan + parameter table), `research-plan.md` (paper math), `paper_47.md` (source paper).

## Build

- **Ninja is required.** Path contains `&` and spaces; MinGW Makefiles routes through `cmd.exe` where `&` splits commands (`cc.exe: no input files`). Always `-G Ninja`.
  ```sh
  cmake -S . -B build -G Ninja          # defaults to RelWithDebInfo (optimized + NDEBUG)
  cmake --build build --target MultiScaleBody-vst3 MultiScaleBody-clap MultiScaleBody-lv2 MultiScaleBody-lv2-ui MultiScaleBody-jack
  ```
- Artifacts land in `build/bin/`. This folder **is** a git repo (remote `github.com/MadMcMan/multi-scale-body`); `build/`, `build-dbg/`, and `deps/` are ignored.
- `deps/{DPF,lvgl,dpf-widgets}` are junctions to `E:/dev/deps/*`; CMake fatals if `deps/DPF/CMakeLists.txt` is missing.
- Fresh MinGW libstdc++ may fail on DPF (re-opened `namespace std`) — patch/pin recipe referenced in `BUILD.md` ("Toolchain caveat").
- LV2 TTL generation works since the folder rename removed the `&` from the path (see `BUILD.md` "LV2 TTL"). UI ships inside the DSP dll.
- LVGL config comes from `plugins/MultiScaleBody/src/lv_conf.h`, forced via `LV_BUILD_CONF_PATH` cache var in the root `CMakeLists.txt`.

## Codegen / baked data

- `plugins/MultiScaleBody/src/ModalData.hpp` is **committed**; CMake never regenerates it. To rebake after changing `tools/modal_bake.py`:
  ```sh
  python tools/modal_bake.py -o plugins/MultiScaleBody/src/ModalData.hpp   # needs numpy+scipy
  ```

## Tests

No CTest. Two standalone tests, run by hand:

```sh
g++ -std=c++17 -I plugins/MultiScaleBody/src tests/test_modal_dsp.cpp plugins/MultiScaleBody/src/MultiScaleBodyEngine.cpp -o build/test_modal_dsp.exe && build/test_modal_dsp.exe
g++ -std=c++17 -O1 -DHOST_BINARY -I plugins/MultiScaleBody/src -I deps/DPF/distrho -I deps/DPF/dgl tests/test_preset_regression.cpp build/libMultiScaleBody-dsp.a deps/DPF/distrho/src/DistrhoPlugin.cpp deps/DPF/distrho/src/DistrhoUtils.cpp -DDISTRHO_IS_STANDALONE -o build/test_preset_regression.exe && build/test_preset_regression.exe
```

- Preset regression requires the plugin built first (`build/libMultiScaleBody-dsp.a`) and compiles the *plugin header* with `-DHOST_BINARY`, which exposes the `test*()` hooks (`PluginMultiScaleBody.hpp:45`).

### Golden bit-identity gate (run this after any DSP edit)

```sh
cmake --build build --target golden_check   # nonzero exit if the default render drifted
```

`tests/golden_default.bin` is the reference default-sound render. The gate was
previously write-only — nothing read the blob, so "default sound unchanged" was
unenforced. On an intended DSP change regenerate with
`cmake --build build --target regen_golden` and report old+new md5. Scope and
blind spots are documented in `BUILD.md` and the generator header.

## UI

`src/PluginUI.cpp` composes the screen ("STRIKE PLATE" design): TE-style
spec-strip header, grouped cymbal knobs (BODY/RESONATE/EXCITER/SPACE), a
playable circular strike disc (click = strike position + note-on), onset-
triggered ripple rings, 16-band spectrum, decay scope, keyboard. Restyling
happens only via the design tokens in `styles/Palette.hpp` and colors/layout in
`PluginUI.cpp`.

## UI architecture (components & styles)

The LVGL UI is organized as a small, strictly-layered design system. Two
sibling folders keep **all components together** and **all styles together**,
with a one-way dependency direction (bottom layer first):

```
plugins/MultiScaleBody/src/
  styles/
    Palette.hpp    # design tokens — the SINGLE SOURCE OF TRUTH
    UIStyles.hpp   # LVGL style pack (UIStyles struct)
  components/
    UICommon.hpp   # abstract UI interface (the component contract)
    UIWidgets.hpp  # the arc-knob component (createArcKnob / drag / sync)
  PluginUI.cpp     # the screen: composes styles + components
```

- **`styles/Palette.hpp` (tokens).** Every design decision lives here and only
  here: the color palette (`COL_*`, `PLATE_*`, `MAT_*`, `SEC_*`), the chassis
  layout arithmetic (`namespace lay`), the UI scale (`gUIScale`, `scaled()`),
  and the typography helpers (`getScaledFont`, `getDisplayFont`, …). Never
  hardcode a color, a spacing/padding value, or a font size at a use site —
  consume or add a token here so the whole UI stays consistent.
- **`styles/UIStyles.hpp` (styles).** The `lv_style_t` pack; consumes tokens
  from `Palette.hpp` and knows nothing about the component contract or the
  screen. All shared control chrome (arcs, buttons, labels, knob parts, meter)
  is styled here.
- **`components/UICommon.hpp` (contract).** Only the `AbstractMultiScaleBodyUI`
  interface the components bind to — no tokens, no styles.
- **`components/UIWidgets.hpp` (components).** The reusable arc-knob control.
  Reads tokens + styles and binds to the contract; owns no magic values.

Layering rule: `Palette` → `UIStyles` → `UICommon`/`UIWidgets` → `PluginUI`.
Lower layers never include higher ones. `src/` is on the include path, so
cross-layer includes are written `styles/…` and `components/…`; a file includes
only what it directly uses. New components go in `components/`; new shared
styling or tokens go in `styles/`.

All four were originally forked from the external **cymbals-ui** project, but
they have **diverged** (adapted to this plugin's palette, geometry tokens, and
parameter bindings) and now have **no build-time or runtime dependency on
cymbals-ui** — they include only local siblings plus `lvgl.h`/DPF. Treat them
as our own: edit them in place, and do **not** re-sync or re-copy from upstream
cymbals-ui without re-checking the MultiScaleBody bindings. If you ever need a
new cymbals-ui control, vendor it into `components/` on first use rather than
adding a cymbals-ui dependency.
