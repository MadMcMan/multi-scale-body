# Build — MultiScaleBody

## Generator: Ninja (required)

Path contains `&` and spaces (`.../Synthesis & Sound Generation/...`). **Use Ninja, not MinGW Makefiles.**
MinGW Makefiles runs through `cmd.exe` where `&` splits commands → `cc.exe: no input files` on LVGL. Ninja passes args as array.


## Build type: optimized by default (required)

With no `CMAKE_BUILD_TYPE`, CMake compiles every edge at **`-O0` with `assert()` live**. That is not a safe shipping config for this plugin:

- **Real-time:** the engine is a per-voice bank of up to 128 biquad resonators. Measured (clean run, 8 voices + reverb at wet=1.0): **~96% of realtime** at `-O0` — right at the dropout threshold — vs **~228%** at `-O2`. The `-O0` build could not sustain the heaviest case in a DAW.
- **Crash surface:** 319 `assert()`s live in DPF/dgl; a failed assert calls `abort()` and takes the host down.

`CMakeLists.txt` therefore defaults to **`RelWithDebInfo`** when no build type is given. Override deliberately with `-DCMAKE_BUILD_TYPE=Debug`. `tests/golden_default.bin` is bit-identical across `-O0`/`-O2` (verified), so the golden gate is build-type independent.
## Command

```sh
cmake -S . -B build -G Ninja          # defaults to RelWithDebInfo; see "Build type" below
cmake --build build --target MultiScaleBody-vst3 MultiScaleBody-clap MultiScaleBody-lv2 MultiScaleBody-lv2-ui MultiScaleBody-jack

# `MultiScaleBody-lv2` only pulls in the DSP dll. The UI dll is a SEPARATE
# target (`MultiScaleBody-lv2-ui`) and is NOT in the lv2 alias, so omitting it
# silently leaves a stale `MultiScaleBody_ui.dll` in build/bin/MultiScaleBody.lv2/
# after a PluginUI.cpp / ui header edit. Keep it in the list.
# optional full: cmake --build build
```

Artifacts land in `build/bin/` (`MultiScaleBody.exe`, `.clap`, `.vst3/`, `.lv2/`).

Deps via junctions in `deps/` → `E:/dev/deps/{DPF,lvgl,dpf-widgets}`.

## LV2 TTL

Works out of the box since the parent folder was renamed to `Synthesis-Sound-Generation`
(no more `&`): `cmake --build build --target MultiScaleBody-lv2` generates
`manifest.ttl`, `MultiScaleBody_dsp.ttl`, `MultiScaleBody_ui.ttl` into `build/bin/MultiScaleBody.lv2/`.
(If a path with `&` ever returns, the old workaround was: run
`build/lv2_ttl_generator.exe <dll>` from PowerShell and move the ttls into the bundle.)

## Toolchain caveat

Fresh MinGW libstdc++ (GCC 13/15) may not compile DPF from source (DPF re-opens `namespace std` inside `namespace DISTRHO`). If you see `std::atof`/`std::vector` unresolved under `DISTRHO::std` or `ClipboardDataOffer` missing, apply the two-line patch to `E:/dev/deps/DPF` described in `E:/dev/dafxpaper/ranked/Filters & EQ/0001-Sphere Echo/BUILD.md` ("To actually build today" option 2), or pin the toolchain that built `SphereEcho.exe` (2026-08-18).

## Offline bake

Modal data is baked: `python tools/modal_bake.py -o plugins/MultiScaleBody/src/ModalData.hpp` (requires `numpy`+`scipy`). The header is committed; CMake does NOT run the bake.

## Tests

```sh
g++ -std=c++17 -I plugins/MultiScaleBody/src tests/test_modal_dsp.cpp plugins/MultiScaleBody/src/MultiScaleBodyEngine.cpp -o build/test_modal_dsp.exe && build/test_modal_dsp.exe
g++ -std=c++17 -O1 -DHOST_BINARY -I plugins/MultiScaleBody/src -I deps/DPF/distrho -I deps/DPF/dgl tests/test_preset_regression.cpp build/libMultiScaleBody-dsp.a deps/DPF/distrho/src/DistrhoPlugin.cpp deps/DPF/distrho/src/DistrhoUtils.cpp -DDISTRHO_IS_STANDALONE -o build/test_preset_regression.exe && build/test_preset_regression.exe
```

(The modal DSP test links `MultiScaleBodyEngine.cpp` — engine methods live in the .cpp, not the header. `OutputLP` is header-only.)

## Golden bit-identity gate

`tests/golden_default.bin` is the reference render of the plugin's **default**
sound. The gate re-renders it and memcmps:

```sh
cmake --build build --target golden_check   # gate: nonzero exit on drift
```

If a DSP change is intentional, regenerate and report the old+new md5:

```sh
cmake --build build --target regen_golden
```

The generator's parameter block must track the `PluginMultiScaleBody` ctor +
`sampleRateChanged` defaults. It previously pinned reverb wet at `0.35f` while
the plugin default is `0.f`, so the blob guarded a non-default state; the blob
was regenerated when that was corrected.

**Scope:** this proves the *default render* is unchanged, not that no branch
changed. It is blind to paths that are exact-identity at defaults (bow, damper,
inharm, support, … all start at 0) and to anything that only engages above the
default peak (−8.8 dBFS — e.g. the limiter ceiling never fires). Those stay
covered by the behavioural tests above. See the header comment in
`tests/gen_golden_volume.cpp` for the injection-verified boundary.
