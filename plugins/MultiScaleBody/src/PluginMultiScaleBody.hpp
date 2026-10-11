#pragma once
#include "DistrhoPlugin.hpp"
#include "MultiScaleBodyEngine.hpp"
#include <array>
#include <string>
#include <cmath>
START_NAMESPACE_DISTRHO
class PluginMultiScaleBody : public Plugin {
public:
    PluginMultiScaleBody();
protected:
    const char* getLabel() const override;
    const char* getMaker() const override;
    const char* getLicense() const override;
    uint32_t    getVersion() const override;
    int64_t     getUniqueId() const override;
    void initParameter(uint32_t index, Parameter& p) override;
    void setParameterValue(uint32_t index, float value) override;
    float getParameterValue(uint32_t index) const override;
    void sampleRateChanged(double newSr) override;
    void activate() override;
    void run(const float** inputs, float** outputs, uint32_t frames, const MidiEvent* midiEvents, uint32_t midiEventCount) override;
    void initState(uint32_t index, State& state) override;
    void setState(const char* key, const char* value) override;
    String getState(const char* key) const override;
public:
    enum Parameters : uint32_t {
        kParamPitch=0,kParamDecay,kParamBrightness,kParamStrikeX,kParamStrikeY,kParamModeCount,kParamWidth,kParamPreset,
        kParamBand0,kParamBand1,kParamBand2,kParamBand3,kParamBand4,kParamBand5,kParamBand6,kParamBand7,
        kParamBand8,kParamBand9,kParamBand10,kParamBand11,kParamBand12,kParamBand13,kParamBand14,kParamBand15,
        kParamRadiation,kParamAttack,kParamRelease,kParamLFORate,kParamLFODepth,
        kParamExciteMix,kParamVelStrike,kParamDetune,kParamGlide,kParamWet,kParamMono,kParamVolume,
        // wave-2 features (appended before the output block so the OUTPUT
        // indices stay contiguous; serializeParams() picks them up for free
        // because it ranges over kParameterCount and skips isOutputParameter,
        // NOT because of kNumInputParams. Every consumer names params by
        // enum, never by literal index.)
        kParamBow,            // bow/friction excitation pressure 0..1 (0 = mallet strikes)
        kParamDamper,         // felt damper depth 0..1 (frequency-dependent mute)
        kParamInharm,         // inharmonicity/spread 0..1 (partial stretch)
        kParamSlideMode,      // MPE slide routing: 0 pitch / 1 mode-bend / 2 brightness
        kParamBandDecay0,kParamBandDecay1,kParamBandDecay2,kParamBandDecay3,
        kParamBandDecay4,kParamBandDecay5,kParamBandDecay6,kParamBandDecay7,
        kParamBandDecay8,kParamBandDecay9,kParamBandDecay10,kParamBandDecay11,
        kParamBandDecay12,kParamBandDecay13,kParamBandDecay14,kParamBandDecay15,
        // wave-3 physical model (ideas 1/2/4/6/7/8/9/10)
        kParamSupport,        // boundary support 0..1 (clamped edge stiffens + damps)
        kParamHoldDamp,       // hold-point damping depth 0..1 (centre strikes damp more)
        kParamResMorph,       // FEM-resolution morph 0..1 (4^3 -> 8^3 fine tables)
        kParamMorphTarget,    // 0..1 body-morph target preset (normalized, like body)
        kParamMorphAmt,       // 0..1 crossfade toward the target body
        kParamMaterial,       // 0..1 material preset (0 = body default)
        kParamRayleighA,      // 0..1 alpha mass damping (Rayleigh C = aM + bK)
        kParamRayleighB,      // 0..1 beta stiffness damping
        kParamEcoMode,        // 0/1 scene-adaptive per-voice mode budget
        kParamEcoBudget,      // 0..1 total ringing-mode budget (64..960)
        // wave-5 second physics strip (support x band / head / scrape)
        kParamSupX,           // clamp touch X 0..1 (per-mode proximity damping)
        kParamSupY,           // clamp touch Y 0..1
        kParamStrikeW,        // mallet head size 0..1 (0.5 = nominal pulse)
        kParamScrape,         // strike friction blend 0..1 (transient stretch)
        // outputs (DSP -> UI metering; never automated, never serialized)
        kParamOutLevel,kParamOutBand0,kParamOutBand1,kParamOutBand2,kParamOutBand3,
        kParamOutBand4,kParamOutBand5,kParamOutBand6,kParamOutBand7,kParamOutBand8,
        kParamOutBand9,kParamOutBand10,kParamOutBand11,kParamOutBand12,kParamOutBand13,
        kParamOutBand14,kParamOutBand15,
        kParamContactNoise,   // appended input: preserve all existing host parameter IDs
        kParamModelMode,      // 0 Classic (unchanged), 1 Elastic FEM
        // mode params appended last: preserve all existing host IDs.
        kParamArp,            // 0/1 arpeggiator on/off (was UI-state-only; now automatable)
        kParamScrubMode,      // 0 = spectrum-drag writes band GAIN, 1 = band DECAY
        // appended OUTPUT: a free-running note-event tick. Bumped in run() on
        // every note-on so the UI (which the framework feeds output-param
        // changes) can drive the looping motion-recorder playback from live
        // notes (host MIDI + the on-screen keyboard). Never automated.
        kParamNoteTick,
        kParameterCount
    };
    // Derived, not a magic "+2+2": the outputs are the level-meter block
    // (kParamOutLevel .. kParamOutBand15) plus the appended note-event tick; every
    // other declared parameter is an input. The old expression happened to equal 74
    // but encoded nothing about the appended inputs, so adding or reordering one
    // silently broke it (and it had exactly one reader, which discarded it).
    static constexpr uint32_t kNumBandOutputs = kParamOutBand15 - kParamOutLevel + 1;
    static constexpr uint32_t kNumOutputParams = kNumBandOutputs + 1;   // + kParamNoteTick
    static constexpr uint32_t kNumInputParams  = kParameterCount - kNumOutputParams;
    static constexpr bool isOutputParameter(uint32_t i){
        return (i>=kParamOutLevel && i<=kParamOutBand15) || i==kParamNoteTick;
    }
    static constexpr bool isInputParameter(uint32_t i) { return i<kParameterCount && !isOutputParameter(i); }
private:
    modal::MultiScaleBodyEngine engine_;
    std::array<float, kParameterCount> paramBase_{};
    // DSP->UI metering values (published as output parameters, polled by DPF per block)
    float vizLevel_=0.f; float vizBins_[16]={};
    // arpeggiator — fixed up-pattern; pattern/gate tables live at the use site in run()
    bool arpOn_=false; int arpPos_=0;
    float vizNoteTick_=0.f;   // note-event tick, published as kParamNoteTick
    double arpSamplesPerStep_=0.0; double arpCounter_=0.0;
    // --- wave-2 state (idea 13/15): MIDI-learned CC map + pending learn + tuning
    // ccToParam_[cc] = parameter index, -1 = not learned. Parse/write on the
    // "ccmap" state key ("param=cc;param2=cc2;..."), consumed in run() BEFORE
    // the built-in CC dispatch (learned bindings override defaults). "learn"
    // state carries the pending param index while the UI waits for the next
    // CC on channel 0 (MIDI learn, idea 15).
    // CCs whose built-in meaning is host-safety critical and must survive MIDI
    // learn: sustain (64) and panic/all-notes-off (120/123). These are never
    // consumed by the learn window and never shadowed by a learned binding, so
    // the pedal and panic can always be reached.
    static bool isReservedCC(int cc){ return cc==64 || cc==120 || cc==123; }
    int ccToParam_[128];            // initialized to -1 in ctor
    int learnPending_=-1;
    // Microtonal tuning (idea 13): the "scale" state key holds the raw .scl
    // text (parsed here, pushed to the engine as a note->ratio table); the
    // "kbm" state key holds an optional degree->note placement.
    std::string scaleTxt_;
    std::string kbmTxt_;
    float scaleRatios_[128];        // note-relative ratios (note 60 = degree 0)
    bool  scaleActive_=false;
    void  rebuildScaleFromScl(const char* sclText);
    void  parseCcmap(const char* str);
    static String serializeCcmap(const int ccToParam[128]);
#ifdef HOST_BINARY
public:
    void testSetParameterValue(uint32_t i,float v){ setParameterValue(i,v); }
    float testGetParameterValue(uint32_t i) const { return getParameterValue(i); }
    uint32_t testGetParameterCount() const { return kParameterCount; }
    void testActivate(){ activate(); }
    void testSampleRate2(double sr){ sampleRateChanged(sr); }
    void testRun2(const float** in,float** out,uint32_t n, const MidiEvent* midi=nullptr,uint32_t mc=0){ run(in,out,n,midi,mc); }
    const modal::MultiScaleBodyEngine& testEngine() const { return engine_; }
#endif
};

// Normalized per-band decay-trim param (0..1, 0.5 = exact identity) -> the decay
// RATE multiplier MultiScaleBodyEngine::setBandDecayTrim takes.
//
// Mirrored so a HIGHER param means a LONGER tail. That matches the main Decay
// knob (setDecayScale: 0.1*pow(100,1-v) - larger v, smaller rate) and the
// DAMPING card, whose bars and "<T60> s" labels display tail TIME.
//
// The trim is a reciprocal-time multiplier; measured on the engine at the -30 dB
// point with all 16 bands set: x0.50 -> 1.533 s, x1.00 -> 0.801 s,
// x2.00 -> 0.424 s. So the previous pow(2,(v-0.5)*2) form made the card's
// left->right drag and the spectrum chart's upward decay scrub mean "shorter to
// the right / up", i.e. the control read as reversed (user-reported).
//
// Single source of truth: the plugin's parameter mapping and PluginUI's display
// both call this, so the two cannot drift apart again.
inline float bandDecayTrimFromNorm(float v){
    return std::pow(2.f,(0.5f-v)*2.f);   // 0.5 -> EXACT 1.0
}
END_NAMESPACE_DISTRHO
