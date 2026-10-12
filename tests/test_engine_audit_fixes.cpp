// Regression coverage for the engine audit fixes that the golden gate and
// test_modal_dsp.cpp cannot see.
//
// Why this file exists: golden_check renders ONE program through preset 0
// (Bowl) with Body Reverb at 0, and the DSP suite's default renders keep wet=0
// too. Bowl is also the one body with no modes above 2.86 kHz -- which is
// precisely why the rad/s-vs-Hz unit bugs survived in the first place. So the
// following fixes had no coverage anywhere:
//
//   * the radiation rolloff's band edge (default-path, 19 of 25 presets)
//   * the IR bake's per-mode gain mirroring the voice path (needs wet > 0)
//   * the gain-path band denominator (needs a smaller body + raised Modes)
//   * allNotesOff dropping the latched MPE pressure
//   * the wet history being cleared by panic/reset (needs wet > 0)
//   * the mono retrigger honouring a loaded scale (needs a tuning table)
//
// Deliberately NOT added to test_modal_dsp.cpp: that file carries another
// agent's uncommitted work.
//
// Build/run (no CTest in this repo, same convention as the other two suites):
//   g++ -std=c++17 -I plugins/MultiScaleBody/src \
//       tests/test_engine_audit_fixes.cpp \
//       plugins/MultiScaleBody/src/MultiScaleBodyEngine.cpp \
//       -o build/test_engine_audit_fixes.exe && build/test_engine_audit_fixes.exe
#include "MultiScaleBodyEngine.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace modal;

namespace {

int gFails = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++gFails;
}

constexpr double kSr = 44100.0;

// Goertzel power at one frequency: a narrow band probe without an FFT.
double goertzelPow(const float* x, int n, double f) {
    const double w = 2.0 * M_PI * f / kSr;
    const double c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double s0 = (double)x[i] + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / (double)n;
}
double bandDb(const float* x, int n, double lo, double hi, int steps) {
    double e = 0.0;
    for (int k = 0; k < steps; ++k) {
        const double f = lo + (hi - lo) * (double)k / (double)(steps - 1);
        e += goertzelPow(x, n, f);
    }
    return 10.0 * std::log10(e / (double)steps + 1e-30);
}
double peakHz(const float* x, int n) {
    double best = -1.0, bf = 0.0;
    for (double f = 60.0; f < 2000.0; f *= 1.01) {
        const double p = goertzelPow(x, n, f);
        if (p > best) { best = p; bf = f; }
    }
    return bf;
}

// The plugin's default parameter block (mirrors gen_golden_volume.cpp).
void defaults(MultiScaleBodyEngine& e) {
    e.prepare(kSr);
    e.setPitchScale(0.5f); e.setDecayScale(0.60f); e.setBrightness(0.65f);
    e.setStrike(0.5f, 0.5f); e.setModeCount(0.68f); e.setWidth(0.30f);
    for (int i = 0; i < 16; ++i) e.setBandTrim(i, 1.f);
    e.setRadiationMix(0.45f); e.setAttack(0.15f); e.setReleaseParam(0.45f);
    e.setExciteMix(0.f); e.setVelStrike(0.35f); e.setDetuneSpread(0.15f);
    e.setGlide(0.15f); e.setReverbWet(0.f); e.setMonoMode(false);
}
std::vector<float> strike(MultiScaleBodyEngine& e, int preset, int frames,
                          int note = 60, float vel = 0.9f) {
    e.reset();
    e.setPreset(preset);
    e.noteOn(note, vel, 0);
    std::vector<float> out((size_t)frames, 0.f);
    for (int i = 0; i < frames; ++i) { float l, r; e.processSampleStereo(l, r); out[(size_t)i] = 0.5f*(l+r); }
    return out;
}

// ---------------------------------------------------------------------------
// 1. The radiation rolloff must not reach below 8 kHz.
// The bug compared rad/s against the 8-15 kHz hertz constants, so the rolloff
// started at 1273 Hz and saturated at 2387 Hz, attenuating every mode above the
// lower-mid by up to 0.18*radiationMix. Preset 20 (Can) is the worst affected
// body. Both numbers below were measured with THIS test's own probe (a Goertzel
// band average is a different scale from an FFT bin sum, so the reference has to
// come from the same method):
//     rad/s-vs-Hz build (build/engine_legacy.cpp, only this line reverted) -14.459 dB
//     fixed build                                                        -13.726 dB
// The +0.733 dB lift matches the analytic ceiling 20*log10(1/(1-0.18*0.45)) =
// 0.729 dB, i.e. the whole wrongly-applied attenuation is gone and no more.
void testRadiationBandEdge() {
    MultiScaleBodyEngine e; defaults(e);
    e.setPreset(20);
    const int N = 16384;
    std::vector<float> buf((size_t)N, 0.f);
    e.reset();
    e.noteOn(60, 0.9f, 0);
    for (int i = 0; i < N; ++i) { float l, r; e.processSampleStereo(l, r); buf[(size_t)i] = 0.5f*(l+r); }
    const double mid = bandDb(buf.data(), N, 100.0, 8000.0, 80);
    std::printf("     [rad] Can <8kHz band = %.3f dB (unit-bug build -14.459, fixed -13.726; -11.777 under the 2026-10 ring-fix defaults Decay 0.60 / Modes 0.68)\n", mid);
    check(mid > -12.210, "Radiation: <8 kHz band is above the rad/s-vs-Hz bug level");
    check(std::fabs(mid - -11.777) < 0.15, "Radiation: <8 kHz band matches the fixed reference");
}

// ---------------------------------------------------------------------------
// 2. The IR bake must take each mode's gain from the voice path.
// Pre-fix the bake built its own raw bicubic, so the 16-band level trims never
// reached the wet tail: raising a band trim could not change the IR at all.
void testIrMirrorsBandTrim() {
    MultiScaleBodyEngine e; defaults(e);
    e.setReverbWet(1.f);
    e.setPreset(0);
    const int N = 8192;             // >> the 8 x 256-sample amortized bake
    auto bake = [&]() {
        std::vector<float> buf((size_t)N, 0.f);
        e.reset();
        e.noteOn(60, 0.9f, 0);
        for (int i = 0; i < N; ++i) { float l, r; e.processSampleStereo(l, r); buf[(size_t)i] = l; }
        return buf;
    };
    bake();                                     // settle the bake at unity trims
    std::vector<float> irUnity((size_t)2048, 0.f);
    std::memcpy(irUnity.data(), e.reverbIrL(), sizeof(float) * 2048);
    const double eUnity = bandDb(irUnity.data(), 2048, 100.0, 8000.0, 40);

    e.setBandTrim(3, 2.f);                      // modes 15..19 of an 80-mode body
    bake();
    std::vector<float> irTrim((size_t)2048, 0.f);
    std::memcpy(irTrim.data(), e.reverbIrL(), sizeof(float) * 2048);
    const double eTrim = bandDb(irTrim.data(), 2048, 100.0, 8000.0, 40);

    bool differs = false;
    double dsum = 0.0, rsum = 0.0;
    for (int i = 0; i < 2048; ++i) {
        if (irUnity[(size_t)i] != irTrim[(size_t)i]) differs = true;
        const double d = (double)irTrim[(size_t)i] - (double)irUnity[(size_t)i];
        dsum += d * d; rsum += (double)irUnity[(size_t)i] * (double)irUnity[(size_t)i];
    }
    const double relDb = 10.0 * std::log10(dsum / (rsum + 1e-30) + 1e-30);
    std::printf("     [ir]  band-3 trim raised: IR differs by %.2f dB rel. (band %.3f -> %.3f dB)\n",
                relDb, eUnity, eTrim);
    // The pre-fix bake built its own bicubic and never saw bandTrim_, so the two
    // IRs were byte-identical; that identity is the discriminator here. The band
    // average above is deliberately not asserted: the bake renormalises the IR,
    // so raising one band can move energy between bands without raising the total.
    check(differs, "IR bake: a raised band level trim changes the baked impulse response");
}

// ---------------------------------------------------------------------------
// 3. CC123 must drop the latched per-member pressure.
// noteOn folds a latched Z into the strike drive (vel 0.3 + Z 1.0 -> 0.65),
// while the latch survives until a matching note-off. Pre-fix allNotesOff left it
// set, so a later strike on that channel was struck ~6.7 dB harder.
// The first note is deliberately NOT struck: a sounded note would leave a ~3 s
// release tail that makes the two renders differ for an unrelated reason.
void testAllNotesOffDropsLatches() {
    auto run = [](bool withStaleLatch) {
        MultiScaleBodyEngine e; defaults(e);
        e.setPreset(0);
        e.reset();
        if (withStaleLatch) {
            e.setMpePressure(2, 1.f);   // controller pressure latched on member ch 2
            e.allNotesOff();            // CC123 must drop it (pre-fix: did not)
        }
        std::vector<float> out((size_t)4096, 0.f);
        e.noteOn(60, 0.3f, 2);
        for (int i = 0; i < 4096; ++i) { float l, r; e.processSampleStereo(l, r); out[(size_t)i] = 0.5f*(l+r); }
        return out;
    };
    const std::vector<float> plain = run(false), latched = run(true);
    bool same = (plain.size() == latched.size());
    double dsum = 0.0, rsum = 0.0;
    for (size_t i = 0; same && i < plain.size(); ++i) {
        same = (plain[i] == latched[i]);
        const double d = (double)latched[i] - (double)plain[i];
        dsum += d * d; rsum += (double)plain[i] * (double)plain[i];
    }
    std::printf("     [latch] stale-latch render differs by %.2f dB rel.\n",
                10.0 * std::log10(dsum / (rsum + 1e-30) + 1e-30));
    check(same, "allNotesOff: a stale MPE pressure latch cannot inflate the next strike");
}

// ---------------------------------------------------------------------------
// 4. CC120 panic must be silent with Body Reverb up.
// The convolver reads up to kIrLen samples of dry history; leaving it meant the
// "immediate silence" panic kept emitting the pre-panic signal as a wet tail.
void testPanicSilencesWetTail() {
    MultiScaleBodyEngine e; defaults(e);
    e.setReverbWet(1.f);
    e.setPreset(0);
    e.reset();
    e.noteOn(60, 0.9f, 0);
    for (int i = 0; i < 8192; ++i) { float l, r; e.processSampleStereo(l, r); }
    e.allSoundOff();
    float worst = 0.f;
    for (int i = 0; i < 2048; ++i) {
        float l, r; e.processSampleStereo(l, r);
        if (i >= 512) {                     // let the look-ahead limiter flush
            const float m = std::fabs(l) > std::fabs(r) ? std::fabs(l) : std::fabs(r);
            if (m > worst) worst = m;
        }
    }
    std::printf("     [panic] peak after CC120 (samples 512..2048) = %.3e\n", (double)worst);
    // Measured with this test: 4.440e-03 with the wet-history clear reverted
    // (build/engine_nowet.cpp) versus 7.5e-06 fixed -- the 7.5e-06 floor is the
    // tone-LP/limiter state decaying, not the convolution. The threshold sits
    // 13x above the floor and 44x below the pre-fix tail.
    check(worst < 1e-4f, "Panic: CC120 with Body Reverb up decays to silence");
}

// ---------------------------------------------------------------------------
// 5. A mono retrigger must honour a loaded scale.
// The poly path used noteRatio_[], the mono branch re-derived pow(2,(n-60)/12):
// with a .scl loaded, every mono retrigger jumped off the scale.
void testMonoHonoursScale() {
    // ratio[] = 12-TET shifted up a perfect fifth: every note is 1.5x, so the
    // mono strike's fundamental must move by exactly that factor.
    float ratio[128];
    for (int i = 0; i < 128; ++i) ratio[i] = std::pow(2.f, (i - 60) / 12.f) * 1.5f;

    auto run = [&](bool scaled) {
        MultiScaleBodyEngine e; defaults(e);
        e.setPreset(0);
        e.setMonoMode(true);
        e.setTuning(ratio, scaled);
        std::vector<float> out((size_t)16384, 0.f);
        e.reset();
        e.noteOn(60, 0.9f, 0);
        for (int i = 0; i < 16384; ++i) { float l, r; e.processSampleStereo(l, r); out[(size_t)i] = 0.5f*(l+r); }
        return out;
    };
    const std::vector<float> tet = run(false), scaled = run(true);
    const double fTet = peakHz(tet.data(), 16384), fScaled = peakHz(scaled.data(), 16384);
    const double got = fScaled / fTet;
    std::printf("     [mono] fundamental 12-TET %.1f Hz -> scaled %.1f Hz (ratio %.3f, expect 1.500)\n",
                fTet, fScaled, got);
    check(std::fabs(got - 1.5) < 0.05, "Mono: a retrigger under a loaded scale plays at the scaled pitch");
}

// ---------------------------------------------------------------------------
// 6. The gain path and the decay path must agree on the band index.
// Both derive it from n = min(preset n, modeCount_). setPreset does not
// re-clamp modeCount_, so after raising Modes on a 128-mode body and switching
// to an 84-mode one, modeCount_ stayed 128 while only 84 modes sounded: the
// gain path mapped modes onto bands 0-10, so the band-15 level slider was dead.
void testBandDenominatorMatchesSmallerBody() {
    auto run = [](float bandTrim15) {
        MultiScaleBodyEngine e; defaults(e);
        e.setPreset(0);              // Bowl, n = 128
        e.setModeCount(1.f);         // 128 modes
        e.setPreset(6);              // Bar, n = 84 -> n stays 84, modeCount_ stays 128
        e.setBandTrim(15, bandTrim15);
        std::vector<float> out((size_t)8192, 0.f);
        e.reset();
        e.noteOn(60, 0.9f, 0);
        for (int i = 0; i < 8192; ++i) { float l, r; e.processSampleStereo(l, r); out[(size_t)i] = 0.5f*(l+r); }
        return out;
    };
    const std::vector<float> unity = run(1.f), boosted = run(2.f);
    double diff = 0.0, ref = 0.0;
    for (size_t i = 0; i < unity.size(); ++i) {
        const double d = (double)boosted[i] - (double)unity[i];
        diff += d * d; ref += (double)unity[i] * (double)unity[i];
    }
    const double relDb = 10.0 * std::log10(diff / (ref + 1e-30) + 1e-30);
    std::printf("     [band] band-15 trim on an 84-mode body: change %.2f dB rel.\n", relDb);
    check(diff > 0.0, "Band index: the band-15 level slider reaches the top modes of a smaller body");
}

} // namespace

int main() {
    std::printf("== engine audit fix regressions ==\n");
    testRadiationBandEdge();
    testIrMirrorsBandTrim();
    testAllNotesOffDropsLatches();
    testPanicSilencesWetTail();
    testMonoHonoursScale();
    testBandDenominatorMatchesSmallerBody();
    std::printf(gFails ? "== %d FAILED ==\n" : "== ALL TESTS PASSED ==\n", gFails);
    return gFails ? 1 : 0;
}
