#include "PluginMultiScaleBody.hpp"
#include "ModalData.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
START_NAMESPACE_DISTRHO
static const int kNumParams = PluginMultiScaleBody::kParameterCount;
PluginMultiScaleBody::PluginMultiScaleBody() : Plugin(kNumParams, 0, 2) {
    paramBase_.fill(0.f);
    paramBase_[kParamPitch]=0.5f; paramBase_[kParamDecay]=0.5f; paramBase_[kParamBrightness]=0.65f;
    paramBase_[kParamStrikeX]=0.5f; paramBase_[kParamStrikeY]=0.5f; paramBase_[kParamModeCount]=0.60f;
    paramBase_[kParamWidth]=0.30f; paramBase_[kParamPreset]=0.0f;
    for(int i=0;i<16;++i) paramBase_[kParamBand0+i]=0.5f;
    paramBase_[kParamRadiation]=0.45f; paramBase_[kParamAttack]=0.15f; paramBase_[kParamRelease]=0.45f;
    paramBase_[kParamLFORate]=0.30f; paramBase_[kParamLFODepth]=0.0f;
    paramBase_[kParamExciteMix]=0.f; paramBase_[kParamVelStrike]=0.35f; paramBase_[kParamDetune]=0.15f;
    paramBase_[kParamGlide]=0.15f; paramBase_[kParamWet]=0.f; paramBase_[kParamMono]=0.f;
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
    engine_.setMonoMode(paramBase_[kParamMono]>0.5f);
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
        default:
            if(index>=kParamBand0 && index<=kParamBand15){
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
        case kParamMono: engine_.setMonoMode(v>0.5f); break;
        default:
            if(idx>=kParamBand0 && idx<=kParamBand15){
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
    engine_.setMonoMode(paramBase_[kParamMono]>0.5f);
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
        if(ev.size<3) return; // guard: data[2] only valid for 3-byte messages
        uint8_t st=ev.data[0]&0xF0, d1=ev.data[1], d2=ev.data[2];
        int ch=ev.data[0]&0x0F;
        if(st==0x90 && d2>0) engine_.noteOn(d1,d2/127.f,ch);
        else if(st==0x80 || (st==0x90 && d2==0)) engine_.noteOff(d1,ch);
        else if(st==0xE0){ // pitch bend (MPE-aware per channel)
            int val=((int)d2<<7)|d1; // 0..16383
            float semis=(val-8192)/8192.f*2.f;
            engine_.setPitchBend(ch,semis);
        }
        else if(st==0xB0){
            if(d1==1){ engine_.setBrightness(std::clamp(d2/127.f,0.f,1.f)); }
            else if(d1==64){ engine_.setSustainPedal(d2>=64); } // sustain: defer note-offs while held
            else if(d1==120){ engine_.allSoundOff(); }   // panic: immediate silence
            else if(d1==123){ engine_.allNotesOff(); }   // host panic / all-notes-off
        }
        else if(st==0xD0){ engine_.setBrightness(std::clamp(0.5f+0.5f*d1/127.f,0.f,1.f)); }
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
        default: break;
    }
}
String PluginMultiScaleBody::getState(const char* key) const {
    if(key && std::string(key)=="arpon") return String(arpOn_?"1":"0");
    return serializeParams(paramBase_);
}
void PluginMultiScaleBody::setState(const char* key, const char* value){
    if(!key||!value) return;
    if(std::string(key)=="arpon"){ arpOn_=(value[0]=='1'); return; }
    if(std::string(key)!="patch") return;
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
Plugin* createPlugin(){ return new PluginMultiScaleBody(); }
END_NAMESPACE_DISTRHO
