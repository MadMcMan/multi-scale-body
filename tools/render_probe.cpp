// render_probe — deterministic offline WAV renderer for MultiScaleBody.
//
// Drives modal::MultiScaleBodyEngine directly (no DPF host): prepares at
// 48 kHz, applies exactly the default parameter set the plugin constructor
// applies (PluginMultiScaleBody.cpp ctor), selects a preset from argv, then
// renders a fixed "gauntlet" clip and writes 16-bit PCM stereo WAV.
//
// Usage: render_probe <presetIndex> <out.wav>
//
// The gauntlet clip is fixed so every builder/critic round compares identical
// input: 10 s @48 kHz stereo —
//   C4(60) vel127 @0.0s   A3(57) vel100 @0.5s   E5(76) vel64 @1.0s
//   chord 60+64+67 vel90 @2.0s                  C2(36) vel127 @3.0s
// each note released 50 ms after its strike (percussive); silence after 4 s
// captures decay tails through the end of the file.
//
// Determinism: the engine has no RNG or wall-clock dependence (the per-mode
// detune table is an LCG hash), so byte-identical output is expected across
// runs on the same binary.

#include "MultiScaleBodyEngine.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kDurationS  = 10.0;

struct Event {
    int64_t sample;   // absolute sample position
    bool    on;       // true=noteOn, false=noteOff
    int     note;     // MIDI note number
    int     vel;      // MIDI velocity 1..127 (ignored on noteOff)
};

// The gauntlet clip. Order within one sample position is preserved
// (chord notes stack in listed order).
std::vector<Event> gauntletClip() {
    const double sr = kSampleRate;
    auto at = [&](double t) -> int64_t { return (int64_t)std::lround(t * sr); };
    std::vector<Event> ev = {
        {at(0.0),  true,  60, 127},
        {at(0.05), false, 60,   0},
        {at(0.5),  true,  57, 100},
        {at(0.55), false, 57,   0},
        {at(1.0),  true,  76,  64},
        {at(1.05), false, 76,   0},
        // chord 60+64+67 vel90 @2.0s
        {at(2.0),  true,  60,  90},
        {at(2.0),  true,  64,  90},
        {at(2.0),  true,  67,  90},
        {at(2.05), false, 60,   0},
        {at(2.05), false, 64,   0},
        {at(2.05), false, 67,   0},
        {at(3.0),  true,  36, 127},
        {at(3.05), false, 36,   0},
    };
    return ev;
}

// Apply exactly the defaults PluginMultiScaleBody::PluginMultiScaleBody()
// applies, except the body preset comes from the CLI instead of hardcoded 0.
void applyPluginDefaults(modal::MultiScaleBodyEngine& eng, int presetIndex) {
    eng.prepare(kSampleRate);
    eng.setPitchScale(0.5f);
    eng.setDecayScale(0.5f);
    eng.setBrightness(0.65f);
    eng.setStrike(0.5f, 0.5f);
    eng.setModeCount(0.60f);
    eng.setWidth(0.30f);
    eng.setPreset(presetIndex);
    for (int i = 0; i < 16; ++i) eng.setBandTrim(i, 0.5f * 2.f);
    eng.setRadiationMix(0.45f);
    eng.setAttack(0.15f);
    eng.setReleaseParam(0.45f);
    eng.setLFORate(0.30f);
    eng.setLFODepth(0.0f);
    eng.setExciteMix(0.f);
    eng.setVelStrike(0.35f);
    eng.setDetuneSpread(0.15f);
    eng.setGlide(0.15f);
    eng.setReverbWet(0.f);
    eng.setMonoMode(false);
    eng.reset(); // mirrors PluginMultiScaleBody::activate()
}

void putU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF);
    b.push_back((v >> 16) & 0xFF); b.push_back((v >> 24) & 0xFF);
}
void putU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF);
}
void putTag(std::vector<uint8_t>& b, const char* tag) {
    for (int i = 0; i < 4; ++i) b.push_back((uint8_t)tag[i]);
}

bool writeWav16(const char* path, const std::vector<float>& L,
                const std::vector<float>& R, uint32_t sampleRate) {
    const size_t n = L.size();
    if (R.size() != n || n == 0) return false;
    const uint32_t dataBytes = (uint32_t)(n * 4);

    std::vector<uint8_t> hdr;
    hdr.reserve(44);
    putTag(hdr, "RIFF");           putU32(hdr, 36 + dataBytes);
    putTag(hdr, "WAVE");
    putTag(hdr, "fmt ");           putU32(hdr, 16);
    putU16(hdr, 1);                // PCM
    putU16(hdr, 2);                // stereo
    putU32(hdr, sampleRate);
    putU32(hdr, sampleRate * 4);   // byte rate
    putU16(hdr, 4);                // block align
    putU16(hdr, 16);               // bits per sample
    putTag(hdr, "data");           putU32(hdr, dataBytes);

    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    bool ok = std::fwrite(hdr.data(), 1, hdr.size(), f) == hdr.size();
    std::vector<uint8_t> frame(4);
    for (size_t i = 0; i < n && ok; ++i) {
        float cl = std::clamp(L[i], -1.0f, 1.0f);
        float cr = std::clamp(R[i], -1.0f, 1.0f);
        int16_t sl = (int16_t)std::lrintf(cl * 32767.0f);
        int16_t sr_ = (int16_t)std::lrintf(cr * 32767.0f);
        frame[0] = (uint8_t)(sl & 0xFF); frame[1] = (uint8_t)((sl >> 8) & 0xFF);
        frame[2] = (uint8_t)(sr_ & 0xFF); frame[3] = (uint8_t)((sr_ >> 8) & 0xFF);
        ok = std::fwrite(frame.data(), 1, 4, f) == 4;
    }
    ok = (std::fclose(f) == 0) && ok;
    return ok;
}

// |FFT| of a Hann-windowed segment, zero-padded to 1024 points. Mirrors the
// critic-rig definition (np.fft.rfft of np.hanning-windowed slice).
void fftMag(const std::vector<float>& segIn, std::vector<double>& mag) {
    constexpr int N = 1024;
    std::vector<double> re(N, 0.0), im(N, 0.0);
    const size_t n = std::min(segIn.size(), (size_t)N);
    for (size_t i = 0; i < n; ++i) {
        const double w = (n > 1) ? 0.5 - 0.5 * std::cos(2.0 * M_PI * (double)i / (double)(n - 1))
                                 : 1.0; // np.hanning convention
        re[i] = (double)segIn[i] * w;
    }
    for (int i = 1, j = 0; i < N; ++i) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= N; len <<= 1) {
        const double ang = -2.0 * M_PI / (double)len;
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < N; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int k = 0; k < len / 2; ++k) {
                const double ur = re[i + k], ui = im[i + k];
                const double vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                const double vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr;               im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr;     im[i + k + len / 2] = ui - vi;
                const double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
    mag.assign((size_t)N / 2, 0.0);
    for (int k = 0; k < N / 2; ++k) mag[(size_t)k] = std::sqrt(re[k] * re[k] + im[k] * im[k]);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <presetIndex> <out.wav>\n", argv[0]);
        std::fprintf(stderr, "presets:\n");
        for (int i = 0; i < modal::kNumPresets; ++i)
            std::fprintf(stderr, "  %2d  %s\n", i, modal::kPresets[i].name);
        return 2;
    }
    char* end = nullptr;
    long idx = std::strtol(argv[1], &end, 10);
    if (!end || *end || idx < 0 || idx >= modal::kNumPresets) {
        std::fprintf(stderr, "error: preset index '%s' out of range 0..%d\n",
                     argv[1], modal::kNumPresets - 1);
        return 2;
    }

    modal::MultiScaleBodyEngine eng;
    applyPluginDefaults(eng, (int)idx);

    const int64_t total = (int64_t)std::lround(kDurationS * kSampleRate);
    std::vector<Event> events = gauntletClip();
    size_t ei = 0;

    std::vector<float> L((size_t)total), R((size_t)total);
    modal::ScopedDenormals denormGuard; // same FTZ/DAZ guard the plugin run() uses

    for (int64_t i = 0; i < total; ++i) {
        while (ei < events.size() && events[ei].sample <= i) {
            const Event& e = events[ei++];
            if (e.on) eng.noteOn(e.note, e.vel / 127.0f, 0);
            else      eng.noteOff(e.note, 0);
        }
        eng.processSampleStereo(L[(size_t)i], R[(size_t)i]);
    }

    // --- PDC alignment: apply the DECLARED limiter latency ----------------
    // The look-ahead limiter reports its constant algorithmic delay to hosts
    // via DPF setLatency(), and every DAW compensates it, so the AUDIBLE
    // stream has no leading hole. A raw offline bounce keeps that hole (the
    // first limiterLatency() samples are the delay ring's zeros), which
    // biased every round-2 onset metric by a constant ~3 ms — most of the
    // reported "rise 2.5-3.8 ms vs bar 0.0-0.9 ms" gap. Roll the render
    // left by exactly eng.limiterLatency() samples: byte-for-byte the same
    // compensation the host applies, nothing else changes.
    {
        const int64_t lat = (int64_t)eng.limiterLatency();
        if (lat > 0 && lat < total) {
            for (int64_t i = 0; i + lat < total; ++i) {
                L[(size_t)i] = L[(size_t)(i + lat)];
                R[(size_t)i] = R[(size_t)(i + lat)];
            }
            for (int64_t i = total - lat; i < total; ++i) { L[(size_t)i] = 0.f; R[(size_t)i] = 0.f; }
        }
    }

    if (!writeWav16(argv[2], L, R, (uint32_t)kSampleRate)) {
        std::fprintf(stderr, "error: cannot write '%s'\n", argv[2]);
        return 1;
    }

    float peak = 0.f;
    double sumSq = 0.0;
    for (size_t i = 0; i < L.size(); ++i) {
        peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
        sumSq += (double)L[i] * L[i] + (double)R[i] * R[i];
    }
    const double rms = std::sqrt(sumSq / (2.0 * (double)L.size()));
    // --- gauntlet A/B metrics, identical definitions to the bar renderer ---
    // attack_rise_ms : 10->90% rise of the 24-sample box-smoothed |mono|
    //                  envelope around the first onset (render_pigments.py).
    // ring_s         : last sample above -60 dBFS absolute (seconds).
    {
        std::vector<float> mono((size_t)total);
        for (int64_t i = 0; i < total; ++i)
            mono[(size_t)i] = 0.5f * (L[(size_t)i] + R[(size_t)i]);
        const int64_t n = (int64_t)mono.size();
        std::vector<double> pre((size_t)n + 1, 0.0);
        for (int64_t i = 0; i < n; ++i)
            pre[(size_t)(i + 1)] = pre[(size_t)i] + (double)std::fabs(mono[(size_t)i]);
        auto boxAvg = [&](int64_t i) {
            const int64_t h = 12; // centered 24-sample box, matches np.convolve 'same'
            const int64_t a = i - h < 0 ? 0 : i - h;
            const int64_t b = i + h + 1 > n ? n : i + h + 1;
            return (pre[(size_t)b] - pre[(size_t)a]) / (double)(b - a);
        };
        float envMax = 0.f;
        for (int64_t i = 0; i < n; ++i)
            envMax = std::max(envMax, (float)boxAvg(i));
        const float thresh = 0.02f * envMax;
        int64_t iOn = 0;
        while (iOn < n && (float)boxAvg(iOn) <= thresh) ++iOn;
        const float lo = boxAvg(std::max<int64_t>(iOn - 1, 0));
        float hi = lo;
        for (int64_t i = iOn; i < std::min(n, iOn + (int64_t)kSampleRate); ++i)
            hi = std::max(hi, (float)boxAvg(i));
        int64_t t10 = iOn; while (t10 > 0 && (float)boxAvg(t10) > lo + 0.1f * (hi - lo)) --t10;
        int64_t t90 = iOn;
        while (t90 < n - 1 && (float)boxAvg(t90) < lo + 0.9f * (hi - lo)) ++t90;
        const double attackRiseMs = (double)(t90 - t10) / kSampleRate * 1000.0;
        int64_t tLast = -1;
        const float thr60 = 1e-3f; // -60 dBFS
        for (int64_t i = n - 1; i >= 0; --i)
            if (std::fabs(mono[(size_t)i]) >= thr60) { tLast = i; break; }
        const double ringS = tLast < 0 ? 0.0 : (double)tLast / kSampleRate;
        std::printf("metrics: attack_rise %.2f ms, ring(-60dB) %.2f s\n",
                    attackRiseMs, ringS);
    }
    // --- gauntlet onset metrics, definitions identical to the critic rig --
    // (build/gauntlet/critic2_metrics.py): rise_to_pk20 = first crossing of
    // (peak - 20 dB) by a 1 ms box-smoothed |mono| envelope inside a 50 ms
    // window; centroid / >8 kHz energy fraction over the first 15 ms with a
    // Hann window. Computed on the PDC-aligned render above.
    {
        std::vector<float> mono((size_t)total);
        for (int64_t i = 0; i < total; ++i)
            mono[(size_t)i] = 0.5f * (L[(size_t)i] + R[(size_t)i]);
        const int64_t n = (int64_t)mono.size();
        // 1 ms box-smoothed RMS envelope ('same' convolution), dB domain
        std::vector<double> envDb((size_t)n, -300.0);
        {
            const int64_t h = (int64_t)std::lround(0.0005 * kSampleRate); // half window
            std::vector<double> pre((size_t)n + 1, 0.0);
            for (int64_t i = 0; i < n; ++i)
                pre[(size_t)(i + 1)] = pre[(size_t)i] + (double)mono[(size_t)i] * mono[(size_t)i];
            for (int64_t i = 0; i < n; ++i) {
                const int64_t a = i - h < 0 ? 0 : i - h;
                const int64_t b = i + h + 1 > n ? n : i + h + 1;
                const double p = (pre[(size_t)b] - pre[(size_t)a]) / (double)(b - a);
                envDb[(size_t)i] = 10.0 * std::log10(p + 1e-20);
            }
        }
        std::printf("onset-metrics (PDC-aligned, critic definitions):\n");
        for (const double t0 : {0.0, 0.5, 1.0, 2.0, 3.0}) {
            const int64_t i0 = (int64_t)std::lround(t0 * kSampleRate);
            if (i0 >= n) continue;
            const int64_t i50 = std::min(n, i0 + (int64_t)(0.05 * kSampleRate));
            double pk = -1e30;
            for (int64_t i = i0; i < i50; ++i) pk = std::max(pk, envDb[(size_t)i]);
            double riseMs = -1.0;
            const double thr = pk - 20.0;
            for (int64_t i = i0; i < i50; ++i)
                if (envDb[(size_t)i] >= thr) { riseMs = (double)(i - i0) / kSampleRate * 1000.0; break; }
            const int64_t i15 = std::min(n, i0 + (int64_t)(0.015 * kSampleRate));
            std::vector<float> seg(mono.begin() + (std::ptrdiff_t)i0, mono.begin() + (std::ptrdiff_t)i15);
            std::vector<double> mag;
            fftMag(seg, mag);
            const double fres = kSampleRate / 1024.0;
            double sm = 0.0, sfm = 0.0, sAll = 0.0, sHf = 0.0;
            for (int k = 0; k < 512; ++k) {
                const double fbin = (double)k * fres;
                sm += mag[(size_t)k]; sfm += fbin * mag[(size_t)k];
                sAll += mag[(size_t)k] * mag[(size_t)k];
                if (fbin >= 8000.0 && fbin < 20000.0) sHf += mag[(size_t)k] * mag[(size_t)k];
            }
            std::printf("  t=%.1fs rise_to_pk20 %.2f ms | centroid_15ms %.0f Hz | hf8k_frac %.4f\n",
                        t0, riseMs, sfm / (sm + 1e-20), sHf / (sAll + 1e-20));
        }
    }
    std::printf("rendered %s : preset %d (%s), %d modes, %.1f s @ %.0f Hz, peak %.4f (%.2f dBFS), rms %.4f\n",
                argv[2], (int)idx, modal::kPresets[idx].name, eng.currentModeCount(),
                kDurationS, kSampleRate, peak,
                peak > 0 ? 20.0 * std::log10((double)peak) : -999.0, rms);
    return 0;
}
