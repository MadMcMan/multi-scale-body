# Build — MultiScaleBody

## Generator: Ninja (required)

Path contains `&` and spaces (`.../Synthesis & Sound Generation/...`). **Use Ninja, not MinGW Makefiles.**
MinGW Makefiles runs through `cmd.exe` where `&` splits commands → `cc.exe: no input files` on LVGL. Ninja passes args as array.

## Command

```sh
cmake -S . -B build -G Ninja
cmake --build build --target MultiScaleBody-vst3 MultiScaleBody-clap MultiScaleBody-lv2 MultiScaleBody-jack
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
