#include "PluginMultiScaleBody.hpp"
#include "ModalData.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
START_NAMESPACE_DISTRHO
static const int kNumParams = PluginMultiScaleBody::kParameterCount;
static float bandDecayCurve(float v){ return std::pow(2.f,(v-0.5f)*2.f); } // 0.5 -> EXACT 1.0
PluginMultiScaleBody::PluginMultiScaleBody() : Plugin(kNumParams, 0, 6) {
    paramBase_.fill(0.f);
    std::fill(ccToParam_, ccToParam_+128, -1);
    learnPending_=-1;
    paramBase_[kParamPitch]=0.5f; paramBase_[kParamDecay]=0.5f; paramBase_[kParamBrightness]=0.65f;
    paramBase_[kParamStrikeX]=0.5f; paramBase_[kParamStrikeY]=0.5f; paramBase_[kParamModeCount]=0.60f;
    paramBase_[kParamWidth]=0.30f; paramBase_[kParamPreset]=0.0f;
    for(int i=0;i<16;++i) paramBase_[kParamBand0+i]=0.5f;
    paramBase_[kParamRadiation]=0.45f; paramBase_[kParamAttack]=0.15f; paramBase_[kParamRelease]=0.45f;
    paramBase_[kParamLFORate]=0.30f; paramBase_[kParamLFODepth]=0.0f;
    paramBase_[kParamExciteMix]=0.f; paramBase_[kParamVelStrike]=0.35f; paramBase_[kParamDetune]=0.15f;
    paramBase_[kParamGlide]=0.15f; paramBase_[kParamWet]=0.f; paramBase_[kParamMono]=0.f; paramBase_[kParamVolume]=1.f;
    // wave-2 defaults: every feature off (identity)
    paramBase_[kParamBow]=0.f; paramBase_[kParamDamper]=0.f; paramBase_[kParamInharm]=0.f; paramBase_[kParamSlideMode]=0.f;
    for(int i=0;i<16;++i) paramBase_[kParamBandDecay0+i]=0.5f; // curve -> trim 1.0 exact
    // wave-3 defaults: physical model off (identity)
    paramBase_[kParamSupport]=0.f; paramBase_[kParamHoldDamp]=0.f; paramBase_[kParamResMorph]=0.f;
    paramBase_[kParamMorphTarget]=0.f; paramBase_[kParamMorphAmt]=0.f; paramBase_[kParamMaterial]=0.f;
    paramBase_[kParamRayleighA]=0.f; paramBase_[kParamRayleighB]=0.f;
    paramBase_[kParamEcoMode]=0.f; paramBase_[kParamEcoBudget]=0.5f;
    paramBase_[kParamSupX]=0.5f; paramBase_[kParamSupY]=0.5f;
    paramBase_[kParamStrikeW]=0.5f; paramBase_[kParamScrape]=0.f;
    double sr=getSampleRate(); if(sr<1000) sr=44100;
    engine_.prepare(sr);
    engine_.setPitchScale(paramBase_[kParamPitch]); engine_.setDecayScale(paramBase_[kParamDecay]);
    engine_.setBrightness(paramBase_[kParamBrightness]); engine_.setStrike(paramBase_[kParamStrikeX],paramBase_[kParamStrikeY]);
    engine_.setModeCount(paramBase_[kParamModeCount]); engine_.setWidth(paramBase_[kParamWidth]); engine_.setPreset(0);
    for(int i=0;i<16;++i) engine_.setBandTrim(i, paramBase_[kParamBand0+i]*2.f);
    engine_.setRadiationMix(paramBase_[kParamRadiation]);
    engine_.setAttack(paramBase_[kParamAttack]); engine_.setReleaseParam(paramBase_[kParamRelease]);
    engine_.setLFORate(paramBase_[kParamLFORate]); engine_.setLFODepth(paramBase_[kParamLFODepth]);
    engine_.setExciteMix(paramBase_[kParamExciteMix]);
    engine_.setVelStrike(paramBase_[kParamVelStrike]);
    engine_.setDetuneSpread(paramBase_[kParamDetune]);
    engine_.setGlide(paramBase_[kParamGlide]);
    engine_.setReverbWet(paramBase_[kParamWet]);
    engine_.setVolume(paramBase_[kParamVolume]);
    engine_.setBow(paramBase_[kParamBow]);
    engine_.setDamper(paramBase_[kParamDamper]);
    engine_.setInharmSpread(paramBase_[kParamInharm]);
    engine_.setSlideMode((int)std::lround(paramBase_[kParamSlideMode]*2.f));
    for(int i=0;i<16;++i) engine_.setBandDecayTrim(i, bandDecayCurve(paramBase_[kParamBandDecay0+i]));
    engine_.setSupport(paramBase_[kParamSupport]);
    engine_.setHoldDamp(paramBase_[kParamHoldDamp]);
    engine_.setResMorph(paramBase_[kParamResMorph]);
    engine_.setMorphTarget((int)std::lround(paramBase_[kParamMorphTarget]*(float)(modal::kNumPresets-1)));
    engine_.setMorphAmt(paramBase_[kParamMorphAmt]);
    engine_.setMaterial((int)std::lround(paramBase_[kParamMaterial]*(float)(modal::MultiScaleBodyEngine::kNumMaterials-1)));
    engine_.setRayleigh(paramBase_[kParamRayleighA], paramBase_[kParamRayleighB]);
    engine_.setEco(paramBase_[kParamEcoMode]>0.5f, paramBase_[kParamEcoBudget]);
    engine_.setSupPos(paramBase_[kParamSupX], paramBase_[kParamSupY]);
    engine_.setStrikeW(paramBase_[kParamStrikeW]);
    engine_.setScrape(paramBase_[kParamScrape]);
    // look-ahead limiter delay: hosts compensate when aligning PDC.
    // Reporting requires DISTRHO_PLUGIN_WANT_LATENCY=1 in DistrhoPluginInfo.h
    // (left off for now; guarded so enabling the flag just works).
#if DISTRHO_PLUGIN_WANT_LATENCY
    setLatency(engine_.limiterLatency());
#endif
}
const char* PluginMultiScaleBody::getLabel() const { return "MultiScaleBody"; }
const char* PluginMultiScaleBody::getMaker() const { return "cymbals"; }
const char* PluginMultiScaleBody::getLicense() const { return "MIT"; }
uint32_t PluginMultiScaleBody::getVersion() const { return d_version(2,0,0); }
int64_t PluginMultiScaleBody::getUniqueId() const { return d_cconst('M','S','B','d'); }
void PluginMultiScaleBody::initParameter(uint32_t index, Parameter& p){
    p.hints=kParameterIsAutomatable;
    switch(index){
        case kParamPitch: p.name="Tune"; p.symbol="tune"; p.ranges.def=0.5f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamDecay: p.name="Decay"; p.symbol="decay"; p.ranges.def=0.5f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamBrightness: p.name="Brightness"; p.symbol="brightness"; p.ranges.def=0.65f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamStrikeX: p.name="Strike X"; p.symbol="strikex"; p.ranges.def=0.5f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamStrikeY: p.name="Strike Y"; p.symbol="strikey"; p.ranges.def=0.5f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamModeCount: p.name="Modes"; p.symbol="modes"; p.ranges.def=0.60f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamWidth: p.name="Width"; p.symbol="width"; p.ranges.def=0.30f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamPreset: p.name="Body"; p.symbol="body"; p.ranges.def=0.f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamRadiation: p.name="Radiation"; p.symbol="radiation"; p.ranges.def=0.45f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamAttack: p.name="Attack"; p.symbol="attack"; p.ranges.def=0.15f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamRelease: p.name="Release"; p.symbol="release"; p.ranges.def=0.45f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamLFORate: p.name="LFO Rate"; p.symbol="lforate"; p.ranges.def=0.30f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamLFODepth: p.name="LFO Depth"; p.symbol="lfodepth"; p.ranges.def=0.0f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamExciteMix: p.name="Exciter"; p.symbol="excite"; p.ranges.def=0.0f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamVelStrike: p.name="Vel Strike"; p.symbol="velstrike"; p.ranges.def=0.35f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamDetune: p.name="Imperfection"; p.symbol="detune"; p.ranges.def=0.15f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamGlide: p.name="Glide"; p.symbol="glide"; p.ranges.def=0.15f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamWet: p.name="Body Reverb"; p.symbol="wet"; p.ranges.def=0.0f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamMono: p.name="Mono"; p.symbol="mono"; p.hints|=kParameterIsBoolean|kParameterIsInteger; p.ranges.def=0.f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamVolume: p.name="Volume"; p.symbol="volume"; p.ranges.def=1.0f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamBow: p.name="Bow"; p.symbol="bow"; p.ranges.def=0.f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamDamper: p.name="Damper"; p.symbol="damper"; p.ranges.def=0.f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamInharm: p.name="Inharm"; p.symbol="inharm"; p.ranges.def=0.f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamSlideMode: p.name="Slide Mode"; p.symbol="slidemode"; p.hints|=kParameterIsInteger; p.ranges.def=0.f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamSupport: p.name="Support"; p.symbol="support"; p.ranges.def=0.f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamHoldDamp: p.name="Hold Damp"; p.symbol="holddamp"; p.ranges.def=0.f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamResMorph: p.name="Resolution"; p.symbol="resmorph"; p.ranges.def=0.f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamMorphTarget: p.name="Morph Target"; p.symbol="morphtarget"; p.ranges.def=0.f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamMorphAmt: p.name="Morph"; p.symbol="morph"; p.ranges.def=0.f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamSupX: p.name="Sup X"; p.symbol="supx"; p.ranges.def=0.5f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamSupY: p.name="Sup Y"; p.symbol="supy"; p.ranges.def=0.5f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamStrikeW: p.name="Head"; p.symbol="strikew"; p.ranges.def=0.5f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamScrape: p.name="Scrape"; p.symbol="scrape"; p.ranges.def=0.f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamMaterial: p.name="Material"; p.symbol="material"; p.ranges.def=0.f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamRayleighA: p.name="Rayl A"; p.symbol="rayla"; p.ranges.def=0.f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamRayleighB: p.name="Rayl B"; p.symbol="raylb"; p.ranges.def=0.f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamEcoMode: p.name="Eco"; p.symbol="eco"; p.hints|=kParameterIsBoolean|kParameterIsInteger; p.ranges.def=0.f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        case kParamEcoBudget: p.name="Eco Budget"; p.symbol="ecobudget"; p.ranges.def=0.5f; p.ranges.min=0.f; p.ranges.max=1.f; break;
        default:
            if(index>=kParamBandDecay0 && index<=kParamBandDecay15){
                int band=index-kParamBandDecay0;
                char name[20]; snprintf(name,sizeof(name),"Band %d Decay",band+1);
                char sym[20]; snprintf(sym,sizeof(sym),"band%ddecay",band+1);
                p.name=String(name); p.symbol=String(sym); p.ranges.def=0.5f; p.ranges.min=0.f; p.ranges.max=1.f;
            }
            else if(index>=kParamBand0 && index<=kParamBand15){
                int band=index-kParamBand0;
                char name[16]; snprintf(name,sizeof(name),"Band %d",band+1);
                char sym[16]; snprintf(sym,sizeof(sym),"band%d",band+1);
                p.name=String(name); p.symbol=String(sym); p.ranges.def=0.5f; p.ranges.min=0.f; p.ranges.max=1.f;
            }
            else if(index>=kParamOutLevel && index<kParameterCount){
                // metering outputs: DSP -> host -> UI only, never automatable
                p.hints=kParameterIsOutput;
                p.ranges.def=0.f; p.ranges.min=0.f; p.ranges.max=1.f;
                if(index==kParamOutLevel){ p.name="Level Out"; p.symbol="out_level"; }
                else {
                    int band=index-kParamOutBand0;
                    char name[20]; snprintf(name,sizeof(name),"Band %d Out",band+1);
                    char sym[20]; snprintf(sym,sizeof(sym),"out_band%d",band+1);
                    p.name=String(name); p.symbol=String(sym);
                }
            }
            break;
    }
}
void PluginMultiScaleBody::setParameterValue(uint32_t idx,float v){
    if(idx>=kParameterCount) return;
    if(idx>=kParamOutLevel) return; // outputs are written by run(), not the host
    v=std::clamp(v,0.f,1.f); paramBase_[idx]=v;
    switch(idx){
        case kParamPitch: engine_.setPitchScale(v); break;
        case kParamDecay: engine_.setDecayScale(v); break;
        case kParamBrightness: engine_.setBrightness(v); break;
        case kParamStrikeX: case kParamStrikeY: engine_.setStrike(paramBase_[kParamStrikeX],paramBase_[kParamStrikeY]); break;
        case kParamModeCount: engine_.setModeCount(v); break;
        case kParamWidth: engine_.setWidth(v); break;
        case kParamPreset:{ int mx = modal::kNumPresets - 1; int pr=(int)std::round(v*(float)mx); pr=std::clamp(pr,0,mx); paramBase_[idx]= mx? (float)pr/(float)mx : 0.f; engine_.setPreset(pr); break; }
        case kParamRadiation: engine_.setRadiationMix(v); break;
        case kParamAttack: engine_.setAttack(v); break;
        case kParamRelease: engine_.setReleaseParam(v); break;
        case kParamLFORate: engine_.setLFORate(v); break;
        case kParamLFODepth: engine_.setLFODepth(v); break;
        case kParamExciteMix: engine_.setExciteMix(v); break;
        case kParamVelStrike: engine_.setVelStrike(v); break;
        case kParamDetune: engine_.setDetuneSpread(v); break;
        case kParamGlide: engine_.setGlide(v); break;
        case kParamWet: engine_.setReverbWet(v); break;
        case kParamVolume: engine_.setVolume(v); break;
        case kParamMono: engine_.setMonoMode(v>0.5f); break;
        case kParamBow: engine_.setBow(v); break;
        case kParamDamper: engine_.setDamper(v); break;
        case kParamInharm: engine_.setInharmSpread(v); break;
        case kParamSlideMode: {
            // discrete 3-way: 0 pitch / 0.5 mode-bend / 1.0 brightness.
            // Snap the stored value so the UI echoes clean positions.
            const int m=std::clamp((int)std::lround(v*2.f),0,2);
            paramBase_[idx]=(float)m*0.5f;
            engine_.setSlideMode(m);
            break; }
        case kParamSupport: engine_.setSupport(v); break;
        case kParamHoldDamp: engine_.setHoldDamp(v); break;
        case kParamResMorph: engine_.setResMorph(v); break;
        case kParamMorphTarget: {
            const int t=std::clamp((int)std::lround(v*(float)(modal::kNumPresets-1)),0,modal::kNumPresets-1);
            paramBase_[idx]=(float)t/(float)(modal::kNumPresets-1);
            engine_.setMorphTarget(t);
            break; }
        case kParamMorphAmt: engine_.setMorphAmt(v); break;
        case kParamMaterial: {
            const int nm=modal::MultiScaleBodyEngine::kNumMaterials;
            const int m=std::clamp((int)std::lround(v*(float)(nm-1)),0,nm-1);
            paramBase_[idx]=(float)m/(float)(nm-1);
            engine_.setMaterial(m);
            // wave-5: selecting a real material snaps the Rayleigh knobs to
            // that material's baked defaults (DEFAULT leaves knobs alone).
            // Recursion is one level and terminates (Rayleigh cases below
            // never touch Material).
            if(m>0){
                setParameterValue(kParamRayleighA, modal::MultiScaleBodyEngine::kMatRay[m][0]);
                setParameterValue(kParamRayleighB, modal::MultiScaleBodyEngine::kMatRay[m][1]);
            }
            break; }
        case kParamSupX: case kParamSupY:
            engine_.setSupPos(paramBase_[kParamSupX],paramBase_[kParamSupY]); break;
        case kParamStrikeW: engine_.setStrikeW(v); break;
        case kParamScrape: engine_.setScrape(v); break;
        case kParamRayleighA: engine_.setRayleigh(v,paramBase_[kParamRayleighB]); break;
        case kParamRayleighB: engine_.setRayleigh(paramBase_[kParamRayleighA],v); break;
        case kParamEcoMode: engine_.setEco(v>0.5f,paramBase_[kParamEcoBudget]); break;
        case kParamEcoBudget: engine_.setEco(paramBase_[kParamEcoMode]>0.5f,v); break;
        default:
            if(idx>=kParamBandDecay0 && idx<=kParamBandDecay15){
                int band=idx-kParamBandDecay0;
                engine_.setBandDecayTrim(band, bandDecayCurve(v));
            }
            else if(idx>=kParamBand0 && idx<=kParamBand15){
                int band=idx-kParamBand0;
                engine_.setBandTrim(band, v*2.f);
            }
            break;
    }
}
float PluginMultiScaleBody::getParameterValue(uint32_t idx) const {
    if(idx==kParamOutLevel) return vizLevel_;
    if(idx>=kParamOutBand0 && idx<kParameterCount) return vizBins_[idx-kParamOutBand0];
    if(idx<kNumInputParams) return paramBase_[idx];
    return 0.f;
}
void PluginMultiScaleBody::sampleRateChanged(double sr){
    engine_.prepare(sr);
    engine_.setPitchScale(paramBase_[kParamPitch]); engine_.setDecayScale(paramBase_[kParamDecay]);
    engine_.setBrightness(paramBase_[kParamBrightness]); engine_.setStrike(paramBase_[kParamStrikeX],paramBase_[kParamStrikeY]);
    engine_.setModeCount(paramBase_[kParamModeCount]); engine_.setWidth(paramBase_[kParamWidth]);
    { int mx = modal::kNumPresets - 1; int p=(int)std::round(paramBase_[kParamPreset]*(float)mx); engine_.setPreset(std::clamp(p,0,mx)); }
    for(int i=0;i<16;++i) engine_.setBandTrim(i, paramBase_[kParamBand0+i]*2.f);
    engine_.setRadiationMix(paramBase_[kParamRadiation]);
    engine_.setAttack(paramBase_[kParamAttack]); engine_.setReleaseParam(paramBase_[kParamRelease]);
    engine_.setLFORate(paramBase_[kParamLFORate]); engine_.setLFODepth(paramBase_[kParamLFODepth]);
    engine_.setExciteMix(paramBase_[kParamExciteMix]);
    engine_.setVelStrike(paramBase_[kParamVelStrike]);
    engine_.setDetuneSpread(paramBase_[kParamDetune]);
    engine_.setGlide(paramBase_[kParamGlide]);
    engine_.setReverbWet(paramBase_[kParamWet]);
    engine_.setVolume(paramBase_[kParamVolume]);
    engine_.setMonoMode(paramBase_[kParamMono]>0.5f);
    engine_.setBow(paramBase_[kParamBow]);
    engine_.setDamper(paramBase_[kParamDamper]);
    engine_.setInharmSpread(paramBase_[kParamInharm]);
    engine_.setSlideMode((int)std::lround(paramBase_[kParamSlideMode]*2.f));
    for(int i=0;i<16;++i) engine_.setBandDecayTrim(i, bandDecayCurve(paramBase_[kParamBandDecay0+i]));
    engine_.setSupport(paramBase_[kParamSupport]);
    engine_.setHoldDamp(paramBase_[kParamHoldDamp]);
    engine_.setResMorph(paramBase_[kParamResMorph]);
    engine_.setMorphTarget((int)std::lround(paramBase_[kParamMorphTarget]*(float)(modal::kNumPresets-1)));
    engine_.setMorphAmt(paramBase_[kParamMorphAmt]);
    engine_.setMaterial((int)std::lround(paramBase_[kParamMaterial]*(float)(modal::MultiScaleBodyEngine::kNumMaterials-1)));
    engine_.setRayleigh(paramBase_[kParamRayleighA], paramBase_[kParamRayleighB]);
    engine_.setEco(paramBase_[kParamEcoMode]>0.5f, paramBase_[kParamEcoBudget]);
    engine_.setSupPos(paramBase_[kParamSupX], paramBase_[kParamSupY]);
    engine_.setStrikeW(paramBase_[kParamStrikeW]);
    engine_.setScrape(paramBase_[kParamScrape]);
    if(!scaleTxt_.empty()) engine_.setTuning(scaleRatios_, scaleActive_); // survives SR change
#if DISTRHO_PLUGIN_WANT_LATENCY
    setLatency(engine_.limiterLatency());
#endif
}
void PluginMultiScaleBody::activate(){ engine_.reset(); }
void PluginMultiScaleBody::run(const float** inputs,float** outputs,uint32_t frames,const MidiEvent* midiEvents,uint32_t midiEventCount){
    float* outL=outputs[0]; float* outR=outputs[1];
    uint32_t mi=0;
    float blockPeak=0.f;
    double sr=getSampleRate(); if(sr<1000) sr=44100;
    const TimePosition& tp=getTimePosition();
    bool playing = tp.playing;
    double bpm=120.0;
    if(playing && tp.bbt.valid && tp.bbt.beatsPerMinute>1.0) bpm=tp.bbt.beatsPerMinute;
    // 16th notes
    arpSamplesPerStep_ = (60.0/bpm/4.0)*sr;
    auto handleMidi=[this](const MidiEvent& ev){
        if(ev.size<2) return; // guard: need status + data1 minimum
        uint8_t st=ev.data[0]&0xF0, d1=ev.data[1];
        int ch=ev.data[0]&0x0F;
        if(st==0xD0){ // channel pressure (2-byte message)
            if(ch>=1) engine_.setMpePressure(ch, std::clamp(d1/127.f,0.f,1.f)); // MPE member: latch per-note Z
            else engine_.setBrightness(std::clamp(0.5f+0.5f*d1/127.f,0.f,1.f)); // legacy ch0 brightness
            return;
        }
        if(ev.size<3) return; // guard: data[2] only valid for 3-byte messages
        uint8_t d2=ev.data[2];
        if(st==0x90 && d2>0) engine_.noteOn(d1,d2/127.f,ch);
        else if(st==0x80 || (st==0x90 && d2==0)) engine_.noteOff(d1,ch);
        else if(st==0xE0){ // pitch bend (MPE-aware per channel)
            int val=((int)d2<<7)|d1; // 0..16383
            float semis=(val-8192)/8192.f*2.f;
            engine_.setPitchBend(ch,semis);
        }
        else if(st==0xB0){
            // MIDI learn (idea 15) + learned-CC dispatch run on channel 0
            // BEFORE the built-in CC semantics (a learned binding overrides
            // the default behavior of that controller).
            if(ch==0){
                if(learnPending_>=0 && learnPending_<(int)PluginMultiScaleBody::kNumInputParams){
                    ccToParam_[d1]=learnPending_;
                    learnPending_=-1;   // binding consumed; value untouched
                    return;
                }
                const int mapped=ccToParam_[d1];
                if(mapped>=0 && mapped<(int)PluginMultiScaleBody::kNumInputParams){
                    setParameterValue((uint32_t)mapped, d2/127.f);
                    return;
                }
            }
            if(d1==1){ engine_.setBrightness(std::clamp(d2/127.f,0.f,1.f)); }
            else if(d1==64){ engine_.setSustainPedal(d2/127.f); } // continuous: half-pedal felt + deferral
            else if(d1==120){ engine_.allSoundOff(); }   // panic: immediate silence
            else if(d1==123){ engine_.allNotesOff(); }   // host panic / all-notes-off
        }
        else if(st==0xA0){ engine_.setBrightness(std::clamp(0.5f+0.5f*d2/127.f,0.f,1.f)); } // poly AT value = data2
    };
    modal::ScopedDenormals denormGuard; // FTZ/DAZ for the RT stretch below, restored on scope exit
    for(uint32_t i=0;i<frames;++i){
        while(mi<midiEventCount && midiEvents[mi].frame <= i){
            handleMidi(midiEvents[mi]);
            ++mi;
        }
        // arpeggiator clock
        if(arpOn_ && playing){
            arpCounter_ += 1.0;
            if(arpCounter_ >= arpSamplesPerStep_){
                arpCounter_ -= arpSamplesPerStep_;
                static constexpr int kArpPattern[16]={0,3,7,12,7,3,0,-5,0,3,7,12,15,12,7,3};
                static constexpr int kArpGate[16]={85,85,60,85}; // (i%4==2)?60:85 tiled
                int pat=kArpPattern[arpPos_];
                if(pat>-50){
                    int note=std::clamp(60+pat,0,127);
                    float gate=(float)kArpGate[arpPos_%4]/100.f;
                    engine_.noteOn(note,std::clamp(0.45f+gate*0.55f,0.f,1.f),0);
                    engine_.noteOff(note,0); // struck percussive — env release handles tail via releaseMs
                }
                arpPos_=(arpPos_+1)%16;
            }
        }
        // exciter from input
        float l,r;
        if(inputs && inputs[0]){
            engine_.setExciterSample(inputs[0][i]);
        } else engine_.setExciterSample(0.f);
        engine_.processSampleStereo(l,r);
        outL[i]=l; outR[i]=r;
        const float pk=std::max(std::abs(l),std::abs(r));
        if(pk>blockPeak) blockPeak=pk;
    }
    // stragglers: hosts may emit frame >= frames at block end — apply now, never drop
    while(mi<midiEventCount){ handleMidi(midiEvents[mi]); ++mi; }
    // publish metering outputs (DPF polls getParameterValue each block -> UI parameterChanged)
    vizLevel_ = std::clamp(blockPeak*1.4f, 0.f, 1.f);
    engine_.analyseBands(vizBins_);
}
// --- state save/recall: one key holding all params (inputs only; outputs are ephemeral) ---
static String serializeParams(const std::array<float,kNumParams>& pb){
    String s;
    char buf[24];
    for(uint32_t i=0;i<PluginMultiScaleBody::kNumInputParams;++i){
        snprintf(buf,sizeof(buf),"%u=%.4f;",i,pb[i]);
        s+=buf;
    }
    return s;
}
void PluginMultiScaleBody::initState(uint32_t index, State& state){
    switch(index){
        case 0:
            state.key="patch";
            state.defaultValue=serializeParams(paramBase_);
            state.hints=kStateIsHostReadable|kStateIsBase64Blob;
            break;
        case 1:
            state.key="arpon";
            state.defaultValue="0";
            state.hints=0;
            break;
        case 2:
            state.key="scale";       // microtonal .scl text (idea 13)
            state.defaultValue="";
            state.hints=kStateIsHostReadable;
            break;
        case 3:
            state.key="kbm";         // optional degree->note map "first,last[;n0,n1,...]"
            state.defaultValue="";
            state.hints=kStateIsHostReadable;
            break;
        case 4:
            state.key="ccmap";       // MIDI-learned bindings "param=cc;..." (idea 15)
            state.defaultValue="";
            state.hints=kStateIsHostReadable;
            break;
        case 5:
            state.key="learn";       // pending MIDI-learn target ("5" or "")
            state.defaultValue="";
            state.hints=0;
            break;
        default: break;
    }
}
String PluginMultiScaleBody::getState(const char* key) const {
    if(key && std::string(key)=="arpon") return String(arpOn_?"1":"0");
    if(key && std::string(key)=="scale") return String(scaleTxt_.c_str());
    if(key && std::string(key)=="kbm") return String(kbmTxt_.c_str());
    if(key && std::string(key)=="ccmap") return serializeCcmap(ccToParam_);
    if(key && std::string(key)=="learn") return String(learnPending_>=0 ? "1" : "");
    return serializeParams(paramBase_);
}
void PluginMultiScaleBody::setState(const char* key, const char* value){
    if(!key||!value) return;
    const std::string k(key);
    if(k=="arpon"){ arpOn_=(value[0]=='1'); return; }
    if(k=="scale"){ rebuildScaleFromScl(value); return; }
    if(k=="kbm"){ kbmTxt_=value; if(!scaleTxt_.empty()) rebuildScaleFromScl(scaleTxt_.c_str()); return; }
    if(k=="ccmap"){ parseCcmap(value); return; }
    if(k=="learn"){
        learnPending_ = (value[0]>='0' && value[0]<='9') ? std::atoi(value) : -1;
        if(learnPending_<0 || learnPending_>=(int)PluginMultiScaleBody::kNumInputParams) learnPending_=-1;
        return;
    }
    if(k!="patch") return;
    const char* c=value;
    while(*c){
        char* end=nullptr;
        long idx=strtol(c,&end,10);
        if(end==c || *end!='=') break;
        c=end+1;
        float v=strtof(c,&end);
        if(end==c) break;
        if(idx>=0 && idx<(long)PluginMultiScaleBody::kNumInputParams) setParameterValue((uint32_t)idx,v);
        c=end;
        if(*c==';') ++c; else break;
    }
}
// --- MIDI-learned CC map persistence ("ccmap" = "param=cc;...", idea 15) ---
String PluginMultiScaleBody::serializeCcmap(const int ccToParam[128]){
    String s; char buf[16];
    for(int cc=0;cc<128;++cc){
        const int p=ccToParam[cc];
        if(p>=0){ snprintf(buf,sizeof(buf),"%d=%d;",p,cc); s+=buf; }
    }
    return s;
}
void PluginMultiScaleBody::parseCcmap(const char* str){
    std::fill(ccToParam_,ccToParam_+128,-1);
    if(!str) return;
    const char* c=str;
    while(*c){
        char* end=nullptr;
        long p=strtol(c,&end,10);
        if(end==c || *end!='=') break;
        c=end+1;
        long cc=strtol(c,&end,10);
        if(end==c) break;
        if(p>=0 && p<(long)PluginMultiScaleBody::kNumInputParams && cc>=0 && cc<128)
            ccToParam_[cc]=(int)p;
        c=end;
        if(*c==';') ++c; else break;
    }
}
// --- microtonal tuning (idea 13): .scl text + optional .kbm-light map ------
// .scl subset every real scale uses: '!'-prefixed comments, a count line N
// (2..128), then N degree lines — "a/b" ratio, decimal cents (contains a
// dot), "N\K" equal-division cents, or an integer ratio n/1. Degrees are
// octave-reduced into [1,2). The optional "kbm" state ("first,last[;n0,n1,…]",
// map width must equal N, explicit lists must be consecutive) places degree
// 0 on MIDI note `first` (default 60); higher notes repeat the scale every N
// MIDI notes and notes below the map stay 12-EDO. noteRatio[] is
// NOTE-RELATIVE, so the Tune knob (pitchScale_) remains a global offset
// applied after — loading a 12-TET .scl reproduces the classic ratios.
void PluginMultiScaleBody::rebuildScaleFromScl(const char* sclText){
    scaleTxt_ = sclText ? sclText : "";
    scaleActive_=false;
    for(int n=0;n<128;++n)
        scaleRatios_[n]=std::pow(2.f,(n-60)/12.f);   // identity fallback
    if(scaleTxt_.empty()){ engine_.setTuning(scaleRatios_,false); return; }
    std::vector<double> deg;
    int count=0; bool haveCount=false, skippedName=false;
    const std::string& t=scaleTxt_;
    size_t pos=0;
    while(pos<t.size()){
        size_t e=t.find('\n',pos);
        if(e==std::string::npos) e=t.size();
        std::string line=t.substr(pos,e-pos);
        pos=e+1;
        size_t s0=line.find_first_not_of(" \t\r");
        if(s0==std::string::npos) continue;
        size_t s1=line.find_last_not_of(" \t\r");
        line=line.substr(s0,s1-s0+1);
        if(line.empty() || line[0]=='!') continue;
        if(!haveCount){
            // the count line: an integer in [2,128]. Some files carry a
            // plain-text description line BEFORE it ('!' comments are already
            // skipped); a non-numeric first data line is treated as that
            // description and skipped once.
            const int c=std::atoi(line.c_str());
            if(c>=2 && c<=128){ count=c; haveCount=true; continue; }
            if(!skippedName){ skippedName=true; continue; }
            return;                     // two non-numeric lines: invalid file
        }
        if((int)deg.size()>=count) break;             // trailing lines ignored
        double ratio=0.0; bool ok=false;
        const size_t bs=line.find('\\');
        const size_t sl=line.find('/');
        if(bs!=std::string::npos){                    // "N\K": equal division
            const int num=std::atoi(line.c_str());
            const int den=std::atoi(line.c_str()+(long)bs+1);
            if(den>0){ ratio=std::pow(2.0,(double)num/(double)den); ok=true; }
        } else if(sl!=std::string::npos){             // ratio a/b
            const int a=std::atoi(line.c_str());
            const int b=std::atoi(line.c_str()+(long)sl+1);
            if(b>0){ ratio=(double)a/(double)b; ok=true; }
        } else if(line.find('.')!=std::string::npos){ // decimal cents
            ratio=std::pow(2.0,std::atof(line.c_str())/1200.0); ok=true;
        } else {                                       // integer ratio n/1
            ratio=(double)std::atoi(line.c_str()); ok=true;
        }
        if(!ok || !(ratio>0.0)) return;               // parse failure -> 12-EDO
        while(ratio>=2.0) ratio*=0.5;
        while(ratio<1.0) ratio*=2.0;
        deg.push_back(ratio);
    }
    if(!haveCount || (int)deg.size()<count) return;
    // Convention: if the FILE lists the tonic itself (first degree parses to
    // 1.0), degree k sits on MIDI note (first+k) and the scale repeats every
    // N notes. Otherwise the tonic is implicit on note `first` (ratio 1.0)
    // and the listed degrees occupy first+1..first+N, where the last listed
    // degree is the octave boundary. The common 12-TET/19-EDO files (100.0,
    // 200.0, ... 1200.0) are implicit — so note 60 = C = 1.0 and the table
    // reproduces the classic ratios exactly.
    const bool tonicListed = std::fabs(deg[0]-1.0)<1e-4;
    const int M = tonicListed ? count : count+1;   // positions per octave
    std::vector<double> eff(M,1.0);
    if(tonicListed){ for(int i=0;i<count;++i) eff[i]=deg[i]; }
    else {
        for(int i=0;i<count;++i) eff[i+1]=deg[i];
        if(std::fabs(eff[count]-1.0)<1e-9) eff[count]=2.0; // trailing octave line
    }
    // optional kbm: first (base) note + optional consecutive note list
    int first=60;
    if(!kbmTxt_.empty()){
        int f=60,l=119;
        if(std::sscanf(kbmTxt_.c_str(),"%d,%d",&f,&l)>=1 && f>=0 && f<128) first=f;
        const char* semi=strchr(kbmTxt_.c_str(),';');
        if(semi){
            std::vector<int> notes;
            const char* c=semi+1;
            while(*c){
                int nn=0;
                if(std::sscanf(c,"%d",&nn)!=1) break;
                if(nn<0||nn>127){ notes.clear(); break; }
                notes.push_back(nn);
                const char* nc=strchr(c,',');
                if(!nc) break; c=nc+1;
            }
            bool consec=((int)notes.size()==M);
            if(consec) for(int i=0;i<M;++i) if(notes[i]!=first+i){ consec=false; break; }
            if(!consec) notes.clear(); // non-consecutive lists unsupported -> consecutive default
        }
    }
    for(int midi=0;midi<128;++midi){
        const int rel=midi-first;
        if(rel<0){ scaleRatios_[midi]=std::pow(2.f,(midi-60)/12.f); continue; }
        const int oct=rel/M;
        const int dIdx=rel-oct*M;
        scaleRatios_[midi]=(float)(eff[dIdx]*std::pow(2.0,(double)oct));
    }
    scaleActive_=true;
    engine_.setTuning(scaleRatios_,true);
}
Plugin* createPlugin(){ return new PluginMultiScaleBody(); }
END_NAMESPACE_DISTRHO
