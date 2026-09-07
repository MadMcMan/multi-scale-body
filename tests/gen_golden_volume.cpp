// Golden bit-identity generator — pre/post kParamVolume surgery.
//
// Renders a deterministic MIDI program through MultiScaleBodyEngine at the
// plugin's legacy default parameters and writes raw interleaved stereo float
// output to tests/golden_default.bin. The engine's volume stage is added with
// default = exact unity (gain multiply skipped at volCur_==1.0f), so this same
// generator must produce byte-identical output before and after the change.
#include "MultiScaleBodyEngine.hpp"
#include <cstdio>
#include <cstdint>
#include <cstdlib>

int main() {
    modal::MultiScaleBodyEngine eng;
    eng.prepare(44100.0);

    // legacy plugin defaults (PluginMultiScaleBody ctor + sampleRateChanged)
    eng.setPitchScale(0.5f); eng.setDecayScale(0.5f); eng.setBrightness(0.65f);
    eng.setStrike(0.5f, 0.5f); eng.setModeCount(0.60f); eng.setWidth(0.30f);
    eng.setPreset(0);
    for (int i = 0; i < 16; ++i) eng.setBandTrim(i, 0.5f * 2.f);
    eng.setRadiationMix(0.45f); eng.setAttack(0.15f); eng.setReleaseParam(0.45f);
    eng.setLFORate(0.30f); eng.setLFODepth(0.0f);
    eng.setExciteMix(0.f); eng.setVelStrike(0.35f); eng.setDetuneSpread(0.15f);
    eng.setGlide(0.15f); eng.setReverbWet(0.35f); eng.setMonoMode(false);

    constexpr uint32_t N = 24000; // ~0.54 s
    float* L = (float*)std::malloc(sizeof(float) * N);
    float* R = (float*)std::malloc(sizeof(float) * N);
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
            eng.processSampleStereo(L[done + i], R[done + i]);
        done += nfr;
    }
    FILE* f = std::fopen("tests/golden_default.bin", "wb");
    if (!f) { std::fprintf(stderr, "golden: cannot open output\n"); return 1; }
    std::fwrite(L, sizeof(float), N, f);
    std::fwrite(R, sizeof(float), N, f);
    std::fclose(f);
    std::free(L); std::free(R);
    std::printf("wrote tests/golden_default.bin (%u frames)\n", N);
    return 0;
}
