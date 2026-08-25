#pragma once
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "ModalData.hpp"
#include "OutputLP.hpp"
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <array>
namespace modal {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#ifdef __SSE3__
#include <pmmintrin.h>
#define MODAL_DAZ 1
#endif
// RT-scope denormal flush: long modal tails underflow slowly without it (100x CPU spikes).
// Saves/restores MXCSR so host code on the same thread is unaffected.
struct ScopedDenormals {
    unsigned old_;
    ScopedDenormals() : old_(_mm_getcsr()) {
        _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#ifdef MODAL_DAZ
        _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
    }
    ~ScopedDenormals(){ _mm_setcsr(old_); }
};
#else
struct ScopedDenormals {};
#endif
inline constexpr int kVoiceCount = 8;
inline constexpr int kIrLen = 2048; // ~46ms @44.1k — short body IR, cheap convolve via block overlap-add
inline constexpr float kIrL1Max = 0.85f; // caps wet worst-case gain: |conv| <= L1*max|x|,
                                         // so out <= (1-.7)*tanh_bound + L1*tanh_bound
                                         //   = .3*.85 + .85*.85 = 0.978 < 0.98 always
struct Voice {
    bool active=false; int midiNote=-1; int midiChannel=0; int age=0; int n=0;
    float freq[kMaxModes]{}; float decay[kMaxModes]{}; float gain[kMaxModes]{};
    float R[kMaxModes]{}; float cosTheta[kMaxModes]{}; float radGain[kMaxModes]{};
    float s1[kMaxModes]{}; float s2[kMaxModes]{};
    int silenceCount=0;
    // exciter injection normalizer per mode: (1 - R_i). The raw audio exciter
    // drives resonators whose peak gain is ~1/(1-R_i), so without this the
    // sustained-input output scales with resonator Q (measured up to x3500
    // pre-limiter at long decay = the reported "sometimes very loud").
    // Multiplying the injected signal by (1-R_i) pins the steady-state modal
    // amplitude to ~exciterGain*gains[i]/2 regardless of Decay/Q.
    float excNorm[kMaxModes]{};
    // ADSR per voice
    enum EnvState { Idle, Attack, Decay, Sustain, Release };
    EnvState envState=Idle;
    float env=0.f;
    bool sustainHold=false; // CC64 sustain: note-off received while pedal down
};
class MultiScaleBodyEngine {
public:
    void prepare(double sr);
    void reset();
    void setPreset(int idx);
    void setPitchScale(float v);
    void setDecayScale(float v);
    void setBrightness(float v);
    void setStrike(float x,float y);
    void setModeCount(float v);
    void setWidth(float v);
    void setBandTrim(int band,float v);
    void setRadiationMix(float v);
    void setAttack(float v);
    void setReleaseParam(float v);
    void setLFORate(float v);
    void setLFODepth(float v);
    // --- new feature params ---
    void setExciteMix(float v);      // 0 = internal Dirac strikes, 1 = audio input drives modes
    void setVelStrike(float v);      // 0..1 amount velocity morphs strike position toward edge
    void setDetuneSpread(float v);   // 0..1 per-mode detune spread (cents up to ~30)
    void setGlide(float v);          // portamento seconds 0..1 -> 0..600ms
    void setMonoMode(bool m);        // mono-legato vs poly
    void setReverbWet(float v);      // convolution reverb send 0..1
    float getExciteMix() const { return exciteMix_; }
    float getVelStrike() const { return velStrike_; }
    float getDetuneSpread() const { return detuneSpread_; }
    float getGlideNorm() const { return glideNorm_; }
    bool  getMonoMode() const { return monoMode_; }
    float getReverbWet() const { return reverbWet_; }
    // pitch bend per MIDI channel (MPE)
    void setPitchBend(int channel, float semitones); // -12..+12
    // exciter: call per sample with input audio before processing
    void setExciterSample(float in) { exciterIn_ = in; }
    void noteOn(int midiNote,float vel01,int channel=0);
    void noteOff(int midiNote,int channel=-1);
    void allNotesOff();   // CC123 — release every active voice (normal tails)
    void setSustainPedal(bool down); // CC64 — defer note-offs while held, release on lift
    void allSoundOff();   // CC120 — immediate silence (panic)
    float processSampleMono();
    void processSampleStereo(float &l,float &r);
    // live analyser: Goertzel magnitudes at current mode freqs (16 bands)
    void analyseBands(float* out16);
    int currentPreset() const { return presetIdx_; }
    int currentModeCount() const { return modeCount_; }
    float getPitchScale() const { return pitchScale_; }
    double sampleRate() const { return sampleRate_; }
    const Voice& voice(int i) const { return voices_[i]; }
    // tests/diagnostics: currently baked reverb IR (populated after any wet render)
    const float* reverbIrL() const { return irL_; }
    const float* reverbIrR() const { return irR_; }
    // final-stage limiter telemetry: current gain reduction in dB (<=0) and
    // algorithmic latency in samples (lookahead depth). Latency is constant
    // between prepare() calls; report to the host via DPF setLatency().
    float limiterGainDb() const { return limGainDb_; }
    uint32_t limiterLatency() const { return limLen_ > 0 ? (uint32_t)(limLen_ - 1) : 0; }
    void recomputeVoiceCoeffs(Voice& v);
    void interpolateGainsFor(float* outGain,float sx,float sy) const;
    static float cubicInterp(float p0,float p1,float p2,float p3,float t);
    void beginIrBake();   // incremental RT-safe baking: a few modes per block
    bool stepIrBake(int budgetModes); // returns true when the IR is complete
    void rebuildDetuneTable();
    double sampleRate_=44100; int presetIdx_=0; int modeCount_=80;
    float pitchScale_=1.f, decayScale_=1.f, brightness_=0.65f, strikeX_=0.5f, strikeY_=0.5f, width_=0.3f;
    float bandTrim_[16] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
    float radiationMix_=0.45f;
    float attackNorm_=0.15f, releaseNorm_=0.45f;
    float lfoRateNorm_=0.30f, lfoDepth_=0.0f;
    double lfoPhase_=0.0;
    float attackMs_=12.f, releaseMs_=900.f, lfoHz_=1.2f;
    float exciteMix_=0.f, velStrike_=0.f, detuneSpread_=0.f;
    float glideNorm_=0.f; float glideMs_=0.f;
    bool monoMode_=false;
    float bendSemitones_[16]={}; // per MIDI channel
    float exciterIn_=0.f;
    // mono stack
    int monoTopVoice_=-1;
    // convolution reverb (block-based, small IR)
    float irL_[kIrLen]{}; float irR_[kIrLen]{};
    float wetBufL_[kIrLen*2]{}; float wetBufR_[kIrLen*2]{}; // circular history (overlap-add tail)
    int   wetPos_=0; bool irDirty_=true; float reverbWet_=0.f;
    int   irBakeCursor_=0; bool irBaking_=false;
    int   irBakeGate_=0; // sample countdown: IR bake steps run ~once per 256 samples, never per sample
    bool  sustainPedal_=false;
    // ~20ms one-pole smoothing for params consumed raw in the sample loop (width/wet/exciter)
    float widthCur_=0.3f, wetCur_=0.f, exMixCur_=0.f;
    float rtSmCoef_=0.0075f;
    int   lfoUpdateCounter_=0; // LFO coeff refresh throttle (per-sample recompute is unaffordable)
    // shared per-mode imperfection multipliers (identical across voices by construction)
    float detuneTable_[kMaxModes]{};
    // analyser state
    float anaEnv_[16]={};
    float nextGain_[kMaxModes]{};
    Voice voices_[kVoiceCount]{};
    int nextAge_=0;
    OutputLP lpL_, lpR_;
    void updateEnvelope(Voice& v);

    // --- final output stage: look-ahead brickwall limiter -------------------
    // RT-safe by construction: every state member below is fixed-size, sized
    // once in prepare(); run()/processSampleStereo() only reads/writes them.
    static constexpr int   kLimBuf  = 1024;     // pow2 ring; >= lookahead @192 kHz
    static constexpr float kLimCeil = 0.95f;    // output ceiling (~ -0.45 dBFS)
    float    limDelayL_[kLimBuf]{}; float limDelayR_[kLimBuf]{};
    int      limLen_ = 0;                   // lookahead length in samples (3 ms)
    int      limPos_ = 0;                   // delay-ring write cursor
    // sliding max over the lookahead window: monotonic wedge (values strictly
    // decreasing from head to tail), amortized O(1) per sample
    float    limWedgeV_[kLimBuf]{};
    unsigned limWedgeI_[kLimBuf]{};         // absolute sample index per entry
    unsigned limWh_ = 0, limWt_ = 0;        // wedge head/tail counters
    unsigned limAbs_ = 0;                   // absolute sample counter
    float    limGainDb_ = 0.f;              // smoothed gain reduction (<=0 dB)
    float    limRelCoef_ = 0.f;             // release smoothing coef (per sample)
    void limPrepare(double sr);
    void limReset();
};
}
