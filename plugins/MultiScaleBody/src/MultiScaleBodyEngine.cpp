#include "MultiScaleBodyEngine.hpp"
#include <cstdint>
#include <cmath>
namespace modal {

void MultiScaleBodyEngine::prepare(double sr) {
    if(sr < 1000) sr = 44100;
    sampleRate_ = sr;
    lpL_.prepare(sr); lpR_.prepare(sr);
    lpL_.setCutoffHz(18000); lpR_.setCutoffHz(18000);
    attackMs_ = 1.f + std::pow(attackNorm_,1.5f)*799.f;
    releaseMs_ = 20.f + std::pow(releaseNorm_,1.2f)*7980.f;
    lfoHz_ = 0.05 * std::pow(240.f, lfoRateNorm_);
    glideMs_ = glideNorm_*600.f;
    rtSmCoef_ = 1.f - std::exp(-1.0f/(0.02f*(float)sampleRate_)); // ~20ms param smoothing
    widthCur_=width_; wetCur_=reverbWet_; exMixCur_=exciteMix_;   // start settled, no ramp-in
    irDirty_=true; irBaking_=false;
    rebuildDetuneTable();
    interpolateGainsFor(nextGain_, strikeX_, strikeY_);
    for (auto& v : voices_) if (v.active) recomputeVoiceCoeffs(v);
}

void MultiScaleBodyEngine::reset() {
    for (auto& v : voices_) {
        v.active=false; v.midiNote=-1; v.silenceCount=0; v.sustainHold=false;
        for (int i=0;i<kMaxModes;++i) v.s1[i]=v.s2[i]=0.f;
        v.envState=Voice::Idle; v.env=0.f;
    }
    monoTopVoice_=-1;
    lpL_.reset(); lpR_.reset();
    nextAge_=0; lfoPhase_=0; lfoUpdateCounter_=0; irBakeGate_=0; sustainPedal_=false;
    irBaking_=false; irDirty_=true;
    for(int i=0;i<kIrLen*2;++i){ wetBufL_[i]=0.f; wetBufR_[i]=0.f; }
    widthCur_=width_; wetCur_=reverbWet_; exMixCur_=exciteMix_;
}

float MultiScaleBodyEngine::cubicInterp(float p0,float p1,float p2,float p3,float t){
    float a0 = -0.5f*p0 + 1.5f*p1 -1.5f*p2 +0.5f*p3;
    float a1 = p0 -2.5f*p1 +2.f*p2 -0.5f*p3;
    float a2 = -0.5f*p0 +0.5f*p2;
    float a3 = p1;
    return ((a0*t + a1)*t + a2)*t + a3;
}

void MultiScaleBodyEngine::interpolateGainsFor(float* out, float sx, float sy) const {
    const auto& p = kPresets[presetIdx_];
    float fx = std::clamp(sx,0.f,1.f) * (kGainGrid-1);
    float fy = std::clamp(sy,0.f,1.f) * (kGainGrid-1);
    int ix = (int)std::floor(fx); int iy = (int)std::floor(fy);
    float tx = fx - ix; float ty = fy - iy;
    int n = std::min(p.n, modeCount_);
    for (int m=0;m<n;++m) {
        float col[4];
        for(int j=0;j<4;++j){
            int yj = std::clamp(iy-1+j, 0, kGainGrid-1);
            float r0 = p.gain[m][yj][std::clamp(ix-1,0,kGainGrid-1)];
            float r1 = p.gain[m][yj][std::clamp(ix,0,kGainGrid-1)];
            float r2 = p.gain[m][yj][std::clamp(ix+1,0,kGainGrid-1)];
            float r3 = p.gain[m][yj][std::clamp(ix+2,0,kGainGrid-1)];
            col[j]=cubicInterp(r0,r1,r2,r3,tx);
        }
        float base = cubicInterp(col[0],col[1],col[2],col[3],ty);
        base = std::clamp(base, -0.08f, 0.08f);
        int band = (m*16)/std::max(1,modeCount_);
        out[m] = base * bandTrim_[std::clamp(band,0,15)];
    }
    for (int m=n;m<kMaxModes;++m) out[m]=0.f;
}
void MultiScaleBodyEngine::setBandTrim(int band,float v){
    band=std::clamp(band,0,15); v=std::clamp(v,0.f,2.f);
    bandTrim_[band]=v;
    interpolateGainsFor(nextGain_, strikeX_, strikeY_);
    irDirty_=true;
}
float MultiScaleBodyEngine::getBandTrim(int band) const {
    band=std::clamp(band,0,15); return bandTrim_[band];
}
void MultiScaleBodyEngine::getDisplayGains(float* out16) const {
    for(int b=0;b<16;++b) out16[b]=0.f;
    for(int i=0;i<modeCount_ && i<kMaxModes;++i){
        int b = (i*16)/std::max(1,modeCount_);
        out16[b] += std::abs(nextGain_[i]);
    }
    float mx=0; for(int b=0;b<16;++b) mx=std::max(mx,out16[b]);
    if(mx>1e-9f) for(int b=0;b<16;++b) out16[b]/=mx;
}
// live analyser: Goertzel over recent output isn't stored, so approximate by
// per-mode resonator energy envelope — each active voice's s1 state IS the
// mode output; band-aggregate |s1| weighted by radGain.
void MultiScaleBodyEngine::analyseBands(float* out16){
    for(int b=0;b<16;++b) out16[b]=0.f;
    for(auto& v:voices_){
        if(!v.active) continue;
        float env=v.env;
        for(int i=0;i<v.n;++i){
            int b=(i*16)/std::max(1,v.n);
            out16[b] += std::abs(v.s1[i])*(float)v.R[i]*env*v.radGain[i];
        }
    }
    // smooth
    for(int b=0;b<16;++b){
        float target=out16[b];
        anaEnv_[b] = target>anaEnv_[b] ? anaEnv_[b]+(target-anaEnv_[b])*0.55f
                                       : anaEnv_[b]+(target-anaEnv_[b])*0.18f;
        out16[b]=anaEnv_[b];
    }
    float mx=0; for(int b=0;b<16;++b) mx=std::max(mx,out16[b]);
    if(mx>1e-9f) for(int b=0;b<16;++b) out16[b]/=mx; else for(int b=0;b<16;++b) out16[b]*=0.f;
}
void MultiScaleBodyEngine::recomputeVoiceCoeffs(Voice& v) {
    static const double presetRad[12]={0.1125,0.07,0.1625,0.08,0.125,0.12,0.175,0.15,0.13,0.09,0.195,0.17};
    double a = presetRad[std::clamp(presetIdx_,0,11)];
    const double c = 343.0;
    // channel bend (MPE)
    float bend = bendSemitones_[std::clamp(v.midiChannel,0,15)];
    float bendRatio = std::pow(2.f, bend/12.f);
    for (int i=0;i<v.n;++i) {
        float f = v.freq[i] * pitchScale_ * bendRatio * detuneTable_[i];
        if(lfoDepth_>1e-4f){
            double lfo = std::sin(lfoPhase_) * lfoDepth_ * 0.0105;
            f *= (float)(1.0 + lfo);
        }
        float d = v.decay[i] * decayScale_;
        if (f < 20.f) f=20.f;
        if (f > 18000.f) f=18000.f;
        if (d < 0.2f) d=0.2f;
        if (d > 8000.f) d=8000.f;
        double R = std::exp(- (double)d / sampleRate_);
        double theta = f / sampleRate_;
        v.R[i] = (float)R;
        v.cosTheta[i] = (float)std::cos(theta);
        double omega = (double)f;
        double ka = omega * a / c;
        double eff = (ka*ka)/(1.0+ka*ka);
        double rad = std::sqrt(eff);
        rad = (1.0 - radiationMix_) + radiationMix_*rad;
        if(f>8000) rad *= (1.0 - 0.18* std::min(1.0,(f-8000)/7000.0) * radiationMix_);
        v.radGain[i]=(float)std::clamp(rad,0.05,1.0);
    }
    double lpHz = 400.0 + brightness_ * 17600.0;
    if(lfoDepth_>1e-4f) lpHz += std::sin(lfoPhase_)*lfoDepth_*800.0;
    lpHz = std::clamp(lpHz, 200.0, 18500.0);
    lpL_.setCutoffHz(lpHz); lpR_.setCutoffHz(lpHz);
}
void MultiScaleBodyEngine::setPitchBend(int channel,float semis){
    if(channel<0||channel>15) return;
    semis = std::clamp(semis,-12.f,12.f);
    if(bendSemitones_[channel]==semis) return;
    bendSemitones_[channel]=semis;
    for(auto& v:voices_) if(v.active && v.midiChannel==channel) recomputeVoiceCoeffs(v);
}
void MultiScaleBodyEngine::setExciteMix(float v){ exciteMix_=std::clamp(v,0.f,1.f); }
void MultiScaleBodyEngine::setVelStrike(float v){ velStrike_=std::clamp(v,0.f,1.f); }
void MultiScaleBodyEngine::rebuildDetuneTable() {
    // deterministic hash-based imperfection per mode index (shared by all voices)
    unsigned seed=12345u;
    for(int i=0;i<kMaxModes;++i){
        seed = seed*1664525u + 1013904223u;
        float h = (float)((seed>>8)&0xFFFF)/65535.f; // 0..1
        float cents = (h*2.f-1.f) * 30.f * detuneSpread_; // ±30c max
        detuneTable_[i] = std::pow(2.f, cents/1200.f);
    }
}
void MultiScaleBodyEngine::setDetuneSpread(float v){
    detuneSpread_=std::clamp(v,0.f,1.f);
    rebuildDetuneTable();
    for(auto& voice:voices_)
        if(voice.active) recomputeVoiceCoeffs(voice);
    irDirty_=true;
}
void MultiScaleBodyEngine::setGlide(float v){ glideNorm_=std::clamp(v,0.f,1.f); glideMs_=glideNorm_*600.f; }
void MultiScaleBodyEngine::setMonoMode(bool m){
    monoMode_=m;
    if(m){
        // kill all but newest
        int newest=-1,bestAge=-1;
        for(int i=0;i<kVoiceCount;++i) if(voices_[i].active && voices_[i].age>bestAge){ bestAge=voices_[i].age; newest=i; }
        for(int i=0;i<kVoiceCount;++i) if(i!=newest && voices_[i].active && voices_[i].envState!=Voice::Release){
            voices_[i].envState=Voice::Release;
        }
        monoTopVoice_=newest;
    } else monoTopVoice_=-1;
}
void MultiScaleBodyEngine::setReverbWet(float v){ reverbWet_=std::clamp(v,0.f,1.f); }

// render current modal set into short IR for convolution send.
// RT strategy: the full render (n modes x kIrLen samples of sin/exp) is far too
// heavy for one audio block, so it is split: beginIrBake() clears, then
// stepIrBake(budget) bakes a few modes per call and renormalizes. The RT path
// bakes <=16 modes per block; a full 128-mode IR refresh converges over ~8 blocks.
void MultiScaleBodyEngine::beginIrBake(){
    for(int i=0;i<kIrLen;++i){ irL_[i]=0.f; irR_[i]=0.f; }
    irBakeCursor_=0;
}
bool MultiScaleBodyEngine::stepIrBake(int budgetModes){
    const auto& p=kPresets[presetIdx_];
    int n=std::min(p.n,modeCount_);
    int end=std::min(n,irBakeCursor_+budgetModes);
    float fx=std::clamp(strikeX_,0.f,1.f)*(kGainGrid-1);
    float fy=std::clamp(strikeY_,0.f,1.f)*(kGainGrid-1);
    for(int m=irBakeCursor_;m<end;++m){
        // bicubic gain at strike
        int ix=(int)fx, iy=(int)fy; float tx=fx-ix, ty=fy-iy;
        float col[4];
        for(int j=0;j<4;++j){
            int yj=std::clamp(iy-1+j,0,kGainGrid-1);
            col[j]=cubicInterp(p.gain[m][yj][std::clamp(ix-1,0,15)],
                               p.gain[m][yj][std::clamp(ix,0,15)],
                               p.gain[m][yj][std::clamp(ix+1,0,15)],
                               p.gain[m][yj][std::clamp(ix+2,0,15)],tx);
        }
        float g=cubicInterp(col[0],col[1],col[2],col[3],ty);
        float f=p.freq[m]*pitchScale_;
        if(f<20.f||f>18000.f){ continue; }
        float d=p.decay[m]*decayScale_;
        double w=f/sampleRate_;
        double rEnv=std::exp(-(double)d/sampleRate_);
        float envl=1.f;
        float phase=(float)(m*0.6180339887); // golden-ratio decorrelation
        for(int t=0;t<kIrLen;++t){
            float s=(float)std::sin(w*t+phase)*g*envl;
            envl*=(float)rEnv;
            // pan like engine: lower modes center, higher spread
            float pan=((float)m/n-0.5f)*width_*0.4f;
            float angle=(pan+1.f)*0.25f*(float)M_PI;
            irL_[t]+=s*(float)std::cos(angle);
            irR_[t]+=s*(float)std::sin(angle);
        }
    }
    irBakeCursor_=end;
    // renormalize partial IR so loudness is stable while baking converges
    float mx=0; for(int i=0;i<kIrLen;++i) mx=std::max(mx,std::max(std::abs(irL_[i]),std::abs(irR_[i])));
    if(mx>1e-9f){ float inv=0.8f/mx; for(int i=0;i<kIrLen;++i){ irL_[i]*=inv; irR_[i]*=inv; } }
    return irBakeCursor_>=n;
}
void MultiScaleBodyEngine::bakeCurrentIR(){
    beginIrBake();
    while(!stepIrBake(kMaxModes)) {}
}

void MultiScaleBodyEngine::setPreset(int idx) {
    idx = std::clamp(idx, 0, kNumPresets-1);
    presetIdx_ = idx;
    interpolateGainsFor(nextGain_, strikeX_, strikeY_);
    irDirty_=true;
}
void MultiScaleBodyEngine::setPitchScale(float v) {
    v = std::clamp(v,0.f,1.f);
    pitchScale_ = std::pow(2.f, (v - 0.5f) * 2.f);
    for (auto& voice : voices_) if (voice.active) recomputeVoiceCoeffs(voice);
    irDirty_=true;
}
void MultiScaleBodyEngine::setDecayScale(float v) {
    v = std::clamp(v,0.f,1.f);
    decayScale_ = 0.1f * std::pow(100.f, v);
    for (auto& voice : voices_) if (voice.active) recomputeVoiceCoeffs(voice);
    irDirty_=true;
}
void MultiScaleBodyEngine::setBrightness(float v) {
    brightness_ = std::clamp(v,0.f,1.f);
    double lpHz = 400.0 + brightness_ * 17600.0;
    lpL_.setCutoffHz(lpHz); lpR_.setCutoffHz(lpHz);
    for (auto& voice : voices_) if (voice.active) recomputeVoiceCoeffs(voice);
}
void MultiScaleBodyEngine::setStrike(float x, float y) {
    strikeX_ = std::clamp(x,0.f,1.f);
    strikeY_ = std::clamp(y,0.f,1.f);
    interpolateGainsFor(nextGain_, strikeX_, strikeY_);
    irDirty_=true;
}
void MultiScaleBodyEngine::setModeCount(float v) {
    v = std::clamp(v,0.f,1.f);
    modeCount_ = 8 + int(v * 120.f);
    const auto& p = kPresets[presetIdx_];
    if (modeCount_ > p.n) modeCount_ = p.n;
    interpolateGainsFor(nextGain_, strikeX_, strikeY_);
    irDirty_=true;
}
void MultiScaleBodyEngine::setRadiationMix(float v){
    // ponytail: radGain is baked per mode, so this applies at event rate — fast
    // sweeps step slightly (block-quantized). If audible on pads, smooth by
    // lerping radGain toward a retarget each block instead of a full recompute.
    radiationMix_=std::clamp(v,0.f,1.f);
    for(auto& voice:voices_) if(voice.active) recomputeVoiceCoeffs(voice);
}
void MultiScaleBodyEngine::setWidth(float v){ width_ = std::clamp(v,0.f,1.f); irDirty_=true; }
void MultiScaleBodyEngine::setAttack(float v){
    attackNorm_=std::clamp(v,0.f,1.f);
    attackMs_=1.f + std::pow(attackNorm_,1.5f)*799.f;
}
void MultiScaleBodyEngine::setReleaseParam(float v){
    releaseNorm_=std::clamp(v,0.f,1.f);
    releaseMs_=20.f + std::pow(releaseNorm_,1.2f)*7980.f;
}
void MultiScaleBodyEngine::setLFORate(float v){
    lfoRateNorm_=std::clamp(v,0.f,1.f);
    lfoHz_=0.05 * std::pow(240.f, lfoRateNorm_);
}
void MultiScaleBodyEngine::setLFODepth(float v){ lfoDepth_=std::clamp(v,0.f,1.f); }

void MultiScaleBodyEngine::updateEnvelope(Voice& v){
    double dtMs = 1000.0 / sampleRate_;
    switch(v.envState){
        case Voice::Attack:
            v.env += (float)(dtMs / std::max(0.5,(double)attackMs_));
            if(v.env>=1.f){ v.env=1.f; v.envState=Voice::Decay; }
            break;
        case Voice::Decay:
            v.env -= (float)(dtMs * 0.00375);
            if(v.env<=0.85f){ v.env=0.85f; v.envState=Voice::Sustain; }
            break;
        case Voice::Sustain: break;
        case Voice::Release:
            v.env -= (float)(dtMs / std::max(5.0,(double)releaseMs_));
            if(v.env<=0.f){ v.env=0.f; v.envState=Voice::Idle; v.active=false; v.midiNote=-1; }
            break;
        case Voice::Idle: break;
    }
}

void MultiScaleBodyEngine::noteOn(int midiNote, float vel01, int channel) {
    vel01 = std::clamp(vel01,0.f,1.f);
    if (vel01 < 0.01f) vel01 = 0.01f;
    if(channel<0||channel>15) channel=0;

    if(monoMode_ && monoTopVoice_>=0 && voices_[monoTopVoice_].active){
        Voice& v=voices_[monoTopVoice_];
        v.midiNote=midiNote; v.midiChannel=channel; v.vel=vel01; v.sustainHold=false;
        const auto& p=kPresets[presetIdx_];
        float noteShift=std::pow(2.f,(midiNote-60)/12.f);
        // velocity morph strike position
        float vx=strikeX_+(0.5f-strikeX_)*0.f; // base
        float edgeX=vel01>0.5f?1.f:0.f;
        vx=strikeX_+(edgeX-strikeX_)*(vel01-0.5f)*2.f*velStrike_;
        float vy=strikeY_+(0.9f-strikeY_)*velStrike_*vel01;
        float tmp[kMaxModes];
        interpolateGainsFor(tmp,vx,vy);
        for(int i=0;i<v.n;++i){
            float targetF=p.freq[i]*noteShift*pitchScale_*detuneTable_[i];
            // glide toward new freq over glideMs_
            float cur=v.freq[i];
            if(glideMs_>1.f && v.envState!=Voice::Idle){
                float frac=(float)(1000.0/glideMs_/sampleRate_); // per-sample approach fraction approximated per noteOn
                v.freq[i]=cur+(targetF-cur)*std::clamp(frac*64.f,0.02f,1.f); // chunked glide on retrig
            } else v.freq[i]=targetF;
            v.gain[i]=tmp[i]*vel01;
        }
        v.vel=vel01; v.strikeX=vx; v.strikeY=vy;
        recomputeVoiceCoeffs(v);
        return;
    }

    int target = -1;
    for (int i=0;i<kVoiceCount;++i) if (!voices_[i].active) { target=i; break; }
    if (target==-1) {
        // steal the quietest already-releasing tail first; only fall back to oldest
        // (never chop a fresh attack while a tail exists)
        int bestRel=-1; float bestEnv=2.f; int oldest=0, maxAge=-1;
        for (int i=0;i<kVoiceCount;++i){
            const Voice& cv=voices_[i];
            if(cv.age>maxAge){ maxAge=cv.age; oldest=i; }
            if(cv.envState==Voice::Release && cv.env<bestEnv){ bestEnv=cv.env; bestRel=i; }
        }
        target = (bestRel>=0) ? bestRel : oldest;
    }
    Voice& v = voices_[target];
    v.active = true;
    v.midiNote = midiNote;
    v.sustainHold=false;
    v.midiChannel = channel;
    v.vel = vel01;
    v.age = nextAge_++;
    v.n = modeCount_;
    const auto& p = kPresets[presetIdx_];
    if (v.n > p.n) v.n = p.n;
    // velocity → strike position morph: soft=center/dark, hard=edge/bright
    float vx = strikeX_ + (vel01-0.5f)*velStrike_*0.6f;      // X spreads with velocity
    float vy = strikeY_ + (vel01-0.5f)*velStrike_*0.5f;      // Y drifts up (brighter rim)
    vx=std::clamp(vx,0.f,1.f); vy=std::clamp(vy,0.f,1.f);
    v.strikeX=vx; v.strikeY=vy;
    float gains[kMaxModes];
    interpolateGainsFor(gains,vx,vy);
    float noteShift = std::pow(2.f, (midiNote - 60) / 12.f);
    for (int i=0;i<v.n;++i) {
        v.freq[i] = p.freq[i]*noteShift;
        v.decay[i] = p.decay[i];
        v.gain[i] = gains[i] * vel01;
        v.s1[i]=v.s2[i]=0.f;
    }
    for (int i=v.n;i<kMaxModes;++i) { v.gain[i]=0.f; v.s1[i]=v.s2[i]=0.f; }
    v.env=0.f; v.envState=Voice::Attack;
    recomputeVoiceCoeffs(v);
    v.silenceCount = -1;
    monoTopVoice_=target;
}

void MultiScaleBodyEngine::noteOff(int midiNote, int channel) {
    for(auto& v:voices_){
        if(!v.active) continue;
        bool match = (channel<0)? (v.midiNote==midiNote) : (v.midiNote==midiNote && v.midiChannel==channel);
        if(match && v.envState!=Voice::Idle && v.envState!=Voice::Release){
            if(sustainPedal_) v.sustainHold=true; else v.envState=Voice::Release;
        }
    }
}

void MultiScaleBodyEngine::allNotesOff() {
    for(auto& v:voices_){
        if(!v.active) continue;
        if(v.envState!=Voice::Idle && v.envState!=Voice::Release){
            v.envState=Voice::Release; v.sustainHold=false;   // natural release tail
        }
    }
    monoTopVoice_=-1;
}

void MultiScaleBodyEngine::allSoundOff() {
    for(auto& v:voices_){
        v.active=false; v.midiNote=-1; v.envState=Voice::Idle; v.env=0.f; v.silenceCount=0; v.sustainHold=false;
        for(int i=0;i<kMaxModes;++i){ v.s1[i]=0.f; v.s2[i]=0.f; }
    }
    monoTopVoice_=-1;
}

void MultiScaleBodyEngine::setSustainPedal(bool down) {
    if(sustainPedal_==down) return;
    sustainPedal_=down;
    if(!down) for(auto& v:voices_)
        if(v.sustainHold){ v.sustainHold=false;
            if(v.envState!=Voice::Idle && v.envState!=Voice::Release) v.envState=Voice::Release; }
}

float MultiScaleBodyEngine::processSampleMono() {
    float l,r;
    processSampleStereo(l,r);
    return 0.5f*(l+r);
}
void MultiScaleBodyEngine::processSampleStereo(float &outL, float &outR) {
    outL=0.f; outR=0.f;
    lfoPhase_ += 2.0*M_PI*lfoHz_/sampleRate_;
    if(lfoPhase_>2*M_PI) lfoPhase_-=2*M_PI;
    // ~20ms one-pole glide on params consumed directly in the sample loop:
    // prevents zipper/clicks on fast automation (branch thresholds use the
    // smoothed value too, so switching on/off crossfades instead of stepping)
    widthCur_ += (width_-widthCur_)*rtSmCoef_;
    wetCur_   += (reverbWet_-wetCur_)*rtSmCoef_;
    exMixCur_ += (exciteMix_-exMixCur_)*rtSmCoef_;
    // LFO modulates mode freqs + LP cutoff; refreshing all voice coefficients
    // per sample is unaffordable, so throttle to every 32 samples (~1.5 kHz at
    // 48k) - far above the <=12 Hz LFO band, inaudibly stepped
    if(lfoDepth_>1e-4f && ++lfoUpdateCounter_>=32){
        lfoUpdateCounter_=0;
        for(auto& v:voices_) if(v.active) recomputeVoiceCoeffs(v);
    }
    float excite = exciterIn_ * exMixCur_;
    for (auto& v : voices_) {
        if (!v.active) continue;
        updateEnvelope(v);
        if(!v.active) continue;
        float env = v.env;
        float voiceL=0.f, voiceR=0.f;
        bool impulse = (v.silenceCount==-1);
        int vIdx=0; for(int k=0;k<kVoiceCount;++k) if(&voices_[k]==&v) vIdx=k;
        float voicePan = ((float)vIdx / (kVoiceCount-1) - 0.5f) * widthCur_ * 0.6f;
        for (int i=0;i<v.n;++i) {
            float R = v.R[i];
            float c = v.cosTheta[i];
            float a1 = 2.f * R * c;
            float a2 = -R * R;
            float exc;
            if(exMixCur_>1e-4f){
                // audio exciter: input drives modes through their gain weights
                exc = excite * v.gain[i] * 40.f;
                if(impulse) exc += v.gain[i]; // keep click transient on noteOn
            } else {
                exc = impulse ? v.gain[i] : 0.f;
            }
            float y = a1 * v.s1[i] + a2 * v.s2[i] + exc;
            v.s2[i] = v.s1[i];
            v.s1[i] = y;
            float modePan = ((float)i / v.n - 0.5f) * widthCur_ * 0.4f;
            float pan = std::clamp(voicePan + modePan, -0.9f, 0.9f);
            float angle = (pan + 1.f) * 0.25f * (float)M_PI;
            float gl = std::cos(angle);
            float gr = std::sin(angle);
            float rad = v.radGain[i];
            voiceL += y * gl * rad * env;
            voiceR += y * gr * rad * env;
        }
        if (impulse) v.silenceCount = 0;
        outL += voiceL;
        outR += voiceR;
        float e = (voiceL*voiceL+voiceR*voiceR)*0.5f;
        if (v.envState==Voice::Idle) { v.active=false; v.midiNote=-1; }
        else if (v.envState==Voice::Sustain || v.envState==Voice::Decay){
            if (e < 1e-10f) v.silenceCount++; else v.silenceCount=0;
            if (v.silenceCount > 2048) { v.active=false; v.midiNote=-1; v.envState=Voice::Idle; }
        } else if (v.envState==Voice::Release){
            if(v.env<=1e-4f){ v.active=false; v.midiNote=-1; v.envState=Voice::Idle; }
        }
    }
    outL = lpL_.process(outL);
    outR = lpR_.process(outR);
    outL = std::tanh(outL * 1.2f) * 0.85f;
    outR = std::tanh(outR * 1.2f) * 0.85f;

    // === convolution reverb send (cheap block-free overlap via running tail) ===
    if(wetCur_>1e-4f){
        if(irDirty_){ beginIrBake(); irDirty_=false; irBaking_=true; irBakeGate_=0; }
        // budgeted: <=16 modes per ~256-sample window, never per sample — a full
        // IR refresh spreads over a few blocks instead of spiking one block
        if(irBaking_ && --irBakeGate_<=0){ irBakeGate_=256; irBaking_=!stepIrBake(16); }
        // push dry into history
        wetBufL_[wetPos_]=outL; wetBufR_[wetPos_]=outR;
        // convolve last kIrLen samples against IR
        float wl=0.f, wr=0.f;
        int idx=wetPos_;
        for(int t=0;t<kIrLen;++t){
            wl += wetBufL_[idx]*irL_[t];
            wr += wetBufR_[idx]*irR_[t];
            idx--; if(idx<0) idx=kIrLen*2-1;
        }
        outL = outL*(1.f-wetCur_*0.7f) + wl*wetCur_;
        outR = outR*(1.f-wetCur_*0.7f) + wr*wetCur_;
        wetPos_=(wetPos_+1)%(kIrLen*2);
    }
}
void MultiScaleBodyEngine::processBlockStereo(float* outL, float* outR, uint32_t frames) {
    for (uint32_t i=0;i<frames;++i) {
        float l,r; processSampleStereo(l,r);
        outL[i]=l; outR[i]=r;
    }
}
void MultiScaleBodyEngine::processBlock(const float** inputs, float** outputs, uint32_t frames){
    const float* in = (inputs&&inputs[0])?inputs[0]:nullptr;
    for(uint32_t i=0;i<frames;++i){
        if(in) setExciterSample(in[i]);
        float l,r; processSampleStereo(l,r);
        if(outputs){ outputs[0][i]=l; outputs[1][i]=r; }
    }
}

} // namespace modal
