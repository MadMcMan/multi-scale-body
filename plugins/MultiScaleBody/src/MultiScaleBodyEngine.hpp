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
    bool active=false; int midiNote=-1; int midiChannel=0; float vel=0.f; int age=0; int n=0;
    float freq[kMaxModes]{}; float decay[kMaxModes]{}; float gain[kMaxModes]{};
    float R[kMaxModes]{}; float cosTheta[kMaxModes]{}; float radGain[kMaxModes]{};
    float s1[kMaxModes]{}; float s2[kMaxModes]{};
    float strikeX=0.5f, strikeY=0.5f; // per-voice strike position (vel-morphed)
    int silenceCount=0;
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
    float getBandTrim(int band) const;
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
    void processBlockStereo(float* outL,float* outR,uint32_t frames);
    void processBlock(const float** inputs, float** outputs, uint32_t frames);
    void getDisplayGains(float* out16) const;
    // live analyser: Goertzel magnitudes at current mode freqs (16 bands)
    void analyseBands(float* out16);
    int currentPreset() const { return presetIdx_; }
    int currentModeCount() const { return modeCount_; }
    float getPitchScale() const { return pitchScale_; }
    double sampleRate() const { return sampleRate_; }
    const Voice& voice(int i) const { return voices_[i]; }
    float strikeX() const { return strikeX_; }
    float strikeY() const { return strikeY_; }
    float lfoPhase() const { return (float)lfoPhase_; }
    // tests/diagnostics: currently baked reverb IR (populated after any wet render)
    const float* reverbIrL() const { return irL_; }
    const float* reverbIrR() const { return irR_; }
private:
    void recomputeVoiceCoeffs(Voice& v);
    void interpolateGainsFor(float* outGain,float sx,float sy) const;
    void interpolateGains(float* outGain,float sx,float sy) const { interpolateGainsFor(outGain,sx,sy); }
    static float cubicInterp(float p0,float p1,float p2,float p3,float t);
    void bakeCurrentIR(); // full synchronous render (offline/tests only)
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
};
}
