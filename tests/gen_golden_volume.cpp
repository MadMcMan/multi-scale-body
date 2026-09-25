// Golden bit-identity generator/checker for the shipped default sound.
//
// Renders a deterministic MIDI program through MultiScaleBodyEngine at the
// plugin's CURRENT default parameters and compares/writes the raw interleaved
// stereo output.
//
//   gen_golden          -> writes tests/golden_default.bin (regenerate ONLY
//                           when a change is intended, and report old+new md5)
//   gen_golden --check  -> reads the committed blob and memcmps; exits
//                           nonzero + prints the first differing frame.
//                           This is the gate. It is wired as the CMake
//                           target `golden_check` (see BUILD.md) so the
//                           invariant claimed in PLAN.md and enforced by
//                           `tests/golden_default.bin` is actually runnable
//                           and CI-able. It was previously write-only: nothing
//                           ever read the blob, so "default sound unchanged"
//                           was unenforced.
//
// The parameter block below MUST track PluginMultiScaleBody's ctor +
// sampleRateChanged. A mismatch here means the golden stops being the
// default sound (it previously pinned wet=0.35 while the plugin default is
// 0.f -- so the blob guarded a non-default reverb state).
// Params not set below are safe to omit ONLY while the engine's own internal
// default equals the plugin default (identity): bow/damper/inharm/slide/
// support/holddamp/resmorph/morph/material/rayleigh/eco/scrape are 0 in the
// engine, supX/supY/strikeW are 0.5, band-decay trims are 1.0.
//
// COVERAGE BOUNDARY (verified by injection, don't over-trust this gate):
// It protects the paths that carry signal at defaults -- the modal bank,
// strike excitation, stereo width, reverb-mix blend, note/voice lifecycle.
//   * PASSES (catches a change): width gain x1.001 -> fails at sample 132.
//   * BLIND (cannot see a change): anything whose default is exact identity
//     (bow/damper/inharm/support/... all start at 0, so scaling them is a
//     no-op) and anything below the -8.8 dBFS default peak that only
//     engages above it (the limiter ceiling 0.95 never fires here, so
//     changing kLimCeil does NOT fail this gate).
// Those stay covered by the behavioural tests in test_modal_dsp.cpp, not
// here. This gate is "the DEFAULT RENDER did not change", not "no branch
// changed".
#include "MultiScaleBodyEngine.hpp"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

// Render the plugin's default sound. Returns malloc'd L/R (caller frees).
struct Render { float* L; float* R; uint32_t frames; };

Render renderDefaultSound() {
    modal::MultiScaleBodyEngine eng;
    eng.prepare(44100.0);

    // EXACT copy of PluginMultiScaleBody ctor + sampleRateChanged defaults.
    eng.setPitchScale(0.5f); eng.setDecayScale(0.5f); eng.setBrightness(0.65f);
    eng.setStrike(0.5f, 0.5f); eng.setModeCount(0.60f); eng.setWidth(0.30f);
    eng.setPreset(0);
    for (int i = 0; i < 16; ++i) eng.setBandTrim(i, 0.5f * 2.f);
    eng.setRadiationMix(0.45f); eng.setAttack(0.15f); eng.setReleaseParam(0.45f);
    eng.setLFORate(0.30f); eng.setLFODepth(0.0f);
    eng.setExciteMix(0.f); eng.setVelStrike(0.35f); eng.setDetuneSpread(0.15f);
    eng.setGlide(0.15f); eng.setReverbWet(0.f); eng.setMonoMode(false);
    // Volume default is exact unity -> the multiply is skipped in the sample
    // loop, so it needs no explicit set (matches paramBase_[kParamVolume]=1.f).

    constexpr uint32_t N = 24000; // ~0.54 s
    Render r{ (float*)std::malloc(sizeof(float) * N), (float*)std::malloc(sizeof(float) * N), N };
    uint32_t lcg = 0x12345678u;
    auto vel = [&lcg]() { lcg = lcg * 1664525u + 1013904223u; return ((lcg >> 8) & 0xFF) / 255.f; };

    uint32_t done = 0; int nStrikes = 0;
    while (done < N) {
        uint32_t nfr = (N - done < 256) ? N - done : 256;
        if (nStrikes < 8 && done >= (uint32_t)(nStrikes * 2500)) {
            eng.noteOn(48 + (nStrikes % 5), vel(), 0);
            ++nStrikes;
        }
        for (uint32_t i = 0; i < nfr; ++i)
            eng.processSampleStereo(r.L[done + i], r.R[done + i]);
        done += nfr;
    }
    return r;
}

} // namespace

int main(int argc, char** argv) {
    const bool check = (argc > 1 && std::strcmp(argv[1], "--check") == 0);
    Render r = renderDefaultSound();
    const uint32_t N = r.frames;
    const size_t bytes = sizeof(float) * 2 * N; // interleaved L then R

    if (check) {
        FILE* f = std::fopen("tests/golden_default.bin", "rb");
        if (!f) {
            std::fprintf(stderr,
                "golden: tests/golden_default.bin not found -- generate it first "
                "(cmake --build build --target gen_golden)\n");
            return 1;
        }
        std::fseek(f, 0, SEEK_END);
        const long have = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (have != (long)bytes) {
            std::fprintf(stderr,
                "golden: SIZE MISMATCH committed=%ld bytes, rendered=%zu\n", have, bytes);
            std::fclose(f);
            return 1;
        }
        float* got = (float*)std::malloc(bytes);
        const size_t rd = std::fread(got, 1, bytes, f);
        std::fclose(f);
        if (rd != bytes) {
            std::fprintf(stderr, "golden: short read (%zu of %zu)\n", rd, bytes);
            std::free(got); std::free(r.L); std::free(r.R);
            return 1;
        }
        const float* mineL = r.L;
        const float* mineR = r.R;
        int bad = -1;
        for (size_t i = 0; i < 2 * N; ++i) {
            if (std::memcmp(&got[i], i < N ? &mineL[i] : &mineR[i - N], sizeof(float)) != 0) {
                bad = (int)i; break;
            }
        }
        if (bad >= 0) {
            const float* mine = (bad < (int)N) ? &mineL[bad] : &mineR[bad - N];
            const float* ref  = &got[bad];
            std::fprintf(stderr,
                "golden: DEFAULT SOUND CHANGED at sample %d "
                "(committed %.9g, rendered %.9g)\n"
                "         if this change is intended, regenerate with "
                "`cmake --build build --target gen_golden` and report old+new md5.\n",
                bad, (double)*ref, (double)*mine);
            std::free(got); std::free(r.L); std::free(r.R);
            return 1;
        }
        std::free(got); std::free(r.L); std::free(r.R);
        std::printf("golden: default sound is bit-identical (%u frames)\n", N);
        return 0;
    }

    FILE* f = std::fopen("tests/golden_default.bin", "wb");
    if (!f) { std::fprintf(stderr, "golden: cannot open output\n");
              std::free(r.L); std::free(r.R); return 1; }
    std::fwrite(r.L, sizeof(float), N, f);
    std::fwrite(r.R, sizeof(float), N, f);
    std::fclose(f);
    std::free(r.L); std::free(r.R);
    std::printf("wrote tests/golden_default.bin (%u frames)\n", N);
    return 0;
}
