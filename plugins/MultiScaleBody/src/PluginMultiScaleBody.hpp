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
        kParamExciteMix,kParamVelStrike,kParamDetune,kParamGlide,kParamWet,kParamMono,
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
    // arpeggiator
    bool arpOn_=false; int arpSteps_=16; int arpPos_=0;
    double arpSamplesPerStep_=0.0; double arpCounter_=0.0;
    int arpPattern_[16]={};
    int arpGate_[16]={};
    int arpBaseNote_=60;
    void applyArpDefaults();
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
