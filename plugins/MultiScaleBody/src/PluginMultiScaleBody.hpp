#pragma once
#include "DistrhoPlugin.hpp"
#include "MultiScaleBodyEngine.hpp"
#include <array>
#include <string>
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
        // wave-2 features (inserted before kParamOutLevel so serializeParams —
        // which iterates exactly kNumInputParams — saves them with zero extra
        // work; every consumer names them by enum, never by literal index)
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
        kParameterCount
    };
    static constexpr uint32_t kNumInputParams = kParamOutLevel;
private:
    modal::MultiScaleBodyEngine engine_;
    std::array<float, kParameterCount> paramBase_{};
    // DSP->UI metering values (published as output parameters, polled by DPF per block)
    float vizLevel_=0.f; float vizBins_[16]={};
    // arpeggiator — fixed up-pattern; pattern/gate tables live at the use site in run()
    bool arpOn_=false; int arpPos_=0;
    double arpSamplesPerStep_=0.0; double arpCounter_=0.0;
    // --- wave-2 state (idea 13/15): MIDI-learned CC map + pending learn + tuning
    // ccToParam_[cc] = parameter index, -1 = not learned. Parse/write on the
    // "ccmap" state key ("param=cc;param2=cc2;..."), consumed in run() BEFORE
    // the built-in CC dispatch (learned bindings override defaults). "learn"
    // state carries the pending param index while the UI waits for the next
    // CC on channel 0 (MIDI learn, idea 15).
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
END_NAMESPACE_DISTRHO
