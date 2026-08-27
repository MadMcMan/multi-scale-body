#include "MultiScaleBodyEngine.hpp"
#include <cstdint>
#include <cmath>
namespace modal {

void MultiScaleBodyEngine::prepare(double sr) {
    if(sr < 1000) sr = 44100;
    sampleRate_ = sr;
    lpL_.prepare(sr); lpR_.prepare(sr);
    lpL_.setCutoffHz(18000); lpR_.setCutoffHz(18000);
    setAttack(attackNorm_); // attack knob->ms curve lives only in setAttack()
    releaseMs_ = 20.f + std::pow(releaseNorm_,1.2f)*7980.f;
    lfoHz_ = 0.05 * std::pow(240.f, lfoRateNorm_);
    glideMs_ = glideNorm_*600.f;
    rtSmCoef_ = 1.f - std::exp(-1.0f/(0.02f*(float)sampleRate_)); // ~20ms param smoothing
    exFollowRel_ = std::exp(-1.f/(0.080f*(float)sampleRate_)); // ~80 ms follower release
    widthCur_=width_; wetCur_=reverbWet_; exMixCur_=exciteMix_;   // start settled, no ramp-in
    irDirty_=true; irBaking_=false;
    rebuildDetuneTable();
    computeDecayRef(); // uniform-damping ring anchor for the current preset
    interpolateGainsFor(nextGain_, strikeX_, strikeY_);
    for (auto& v : voices_) if (v.active) recomputeVoiceCoeffs(v);
    limPrepare(sr);
}

void MultiScaleBodyEngine::reset() {
    for (auto& v : voices_) {
        v.active=false; v.midiNote=-1; v.silenceCount=0; v.sustainHold=false;
        v.burstLen=0; v.burstLeft=0; v.transLen=0; v.transLeft=0; v.transLP=0.f;
        for (int i=0;i<kMaxModes;++i) v.s1[i]=v.s2[i]=0.f;
        v.envState=Voice::Idle; v.env=0.f;
    }
    monoTopVoice_=-1;
    lpL_.reset(); lpR_.reset();
    nextAge_=0; lfoPhase_=0; lfoUpdateCounter_=0; irBakeGate_=0; sustainPedal_=false; exFollow_=0.f;
    strikeSeq_=0; transPeak_=0.f; // fresh deterministic transient sequence + telemetry per reset
    limReset();
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
    // Static gain staging (measured): 8-voice vel-127 chords at Modes/Bright
    // maxed peak 1.0..3.1 pre-limiter because per-mode gains stack with mode
    // count. Trim above 64 modes by sqrt(64/n) (-3 dB at 128 modes, unity
    // below) so the output limiter only engages on the hardest corner hits;
    // normal play (measured 0.45 peak pre-limiter) is unaffected beyond the
    // ~1 dB this costs at the default 80-mode bodies.
    const float densTrim = n > 64 ? 1.f / std::sqrt((float)n / 64.f) : 1.f;
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
        out[m] = base * bandTrim_[std::clamp(band,0,15)] * densTrim;
    }
    for (int m=n;m<kMaxModes;++m) out[m]=0.f;
}
void MultiScaleBodyEngine::setBandTrim(int band,float v){
    band=std::clamp(band,0,15); v=std::clamp(v,0.f,2.f);
    bandTrim_[band]=v;
    interpolateGainsFor(nextGain_, strikeX_, strikeY_);
    irDirty_=true;
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
// Ring-shaping anchor: the slowest baked rate of THIS preset divided by a
// fixed factor. Paper 47 (sec 3.2) damps each sounding object uniformly; our
// bake stores strongly varying per-mode rates, so fast modes die long before
// the body's defining slow ring. Anchoring below dmin lets every mode be
// pulled toward a longer, more uniform ring without touching ModalData.hpp.
void MultiScaleBodyEngine::computeDecayRef(){
    const auto& p=kPresets[presetIdx_];
    float mn=p.decay[0];
    for(int i=1;i<p.n;++i) if(p.decay[i]<mn) mn=p.decay[i];
    presetDecayRef_=mn/kDecayAnchorDiv;
}
// Geometric pull of a baked decay RATE toward the anchor: monotone in d, and
// <= d whenever d >= anchor, so rings only lengthen, the relative order of
// mode lifetimes is preserved, and the Decay-knob direction law (applied
// afterwards as a pure multiplier) is untouched. Shared by voice setup and
// the reverb IR bake so both paths carry the same decay character.
float MultiScaleBodyEngine::shapeDecayRate(float d) const {
    if(!(d>0.f)) return d;
    return std::pow(d,1.f-kDecayPullBeta)*std::pow(presetDecayRef_,kDecayPullBeta);
}
// Arm the finite mallet-contact pulse (replaces the literal single-sample
// Dirac injection). Rim strikes = harder contact => shorter, brighter force
// pulse; center hits get the softest, longest one. Net impulse area stays
// unity like the old Dirac (raised cosine sums to len/2, amplitude 2/len),
// so low-frequency excitation is unchanged. Velocity-independent BY DESIGN:
// the limiter-transparency contract requires output to scale exactly with
// velocity below threshold, which forbids velocity-dependent spectra.
void MultiScaleBodyEngine::startStrikeBurst(Voice& v,float vx,float vy){
    float edge = std::max(std::fabs(vx-0.5f),std::fabs(vy-0.5f))*2.f; // 0 center..1 rim
    int bl=(int)std::lround(kStrikeBurstS*(float)sampleRate_*(1.25f-0.5f*edge));
    v.burstLen=std::clamp(bl,4,32);
    // --- pulse-shape compensation (round A3; see kStrikeProbeSafe) --------
    // Re-run every mode's exact 2-pole recursion driven by THIS strike's
    // raised-cosine burst (unit amplitude: kStrikeTrim/gain/strikeNorm are
    // linear factors that cancel in a normalizer) and take the effective
    // strike normalization from the MEASURED response peak, so each mode's
    // summit contribution equals |gain[i]| for the real drive — the same
    // pinned-summit guarantee the analytic 1/M(R,th) only provided for a
    // literal Dirac. Scan window = burstLen + quarter period (+ safety),
    // which is exactly where the burst response of a decaying resonator
    // peaks (the last drive sample rings up within one quarter cycle);
    // capped at 4 bursts because a mode whose quarter period exceeds that
    // cannot distinguish this unity-area pulse from a Dirac and KEEPS the
    // analytic value (low end bit-comparable to before). Runs once per
    // strike at event rate; <= ~5*burstLen iterations/mode, no allocation.
    {
        const int L=v.burstLen;
        const float invL=1.f/(float)L;
        for(int i=0;i<v.n;++i){
            const float c=std::clamp(v.cosTheta[i],-1.f,1.f);
            const float th=std::acos(c);
            const float qp=(th>1e-9f)?(0.5f*(float)M_PI)/th:1e9f;
            const float R=v.R[i];
            if(!(qp<4.f*(float)L) || !(R>0.f) || !(R<1.f)) continue;
            const int tail=(int)std::min(qp,4.f*(float)L)+kStrikeProbeSafe;
            const float a1=2.f*R*c, a2=-R*R;
            float s1=0.f,s2=0.f,pk=0.f;
            for(int m=0;m<L+tail;++m){
                const float u=(m<L)?(2.f*invL*(0.5f-0.5f*std::cos((float)(2.0*M_PI)*(float)m*invL))):0.f;
                const float y=a1*s1+a2*s2+u;
                s2=s1; s1=y;
                pk=std::max(pk,std::fabs(y));
            }
            if(pk>1e-9f) v.strikeEff[i]=1.f/pk;
        }
    }
    // Contact-transient layer (DELIBERATE extension beyond paper Eq. (1)'s
    // Dirac strike; consistent with the finite-contact-pulse deviation
    // above): real mallet contact radiates a few ms of broadband chatter
    // co-incident with the force pulse. Length scales with edge like the
    // pulse (rim = harder contact = shorter); amplitude is a SMALL FRACTION
    // of the tonal excitation (kContactTrim x mean |mode gain| of this very
    // strike, so it tracks velocity/position/preset automatically); the
    // one-pole LP coefficient shapes bandwidth by strike position
    // (center = dark, rim = bright). Noise source: xorshift32 seeded from a
    // monotonic strike counter — fully deterministic, RT-safe, no allocation.
    int tl=(int)std::lround(kContactTransS*(float)sampleRate_*(1.15f-0.3f*edge));
    // clamp ceiling 800 samples: 10 ms base x (1.15..0.85) needs headroom at
    // 96/192 kHz too (the old 192-sample cap silently re-shortened the layer)
    v.transLen=std::clamp(tl,32,800);
    v.transLeft=v.transLen;
    v.transLP=0.f;
    float gsum=0.f;
    for(int i=0;i<v.n;++i) gsum+=std::fabs(v.gain[i]);
    v.transAmp=kContactTrim*(v.n>0? gsum/(float)v.n : 0.f);
    // one-pole LP for y+=a*(x-y): a = 1-exp(-2*pi*fc/sr) puts -3 dB at fc.
    // Round A3 re-staging: the old 1.2k center left default (center-ish)
    // strikes with nothing above ~4 kHz — onsets could not carry the
    // measurable 4-12 kHz content that makes a strike "land". The baked
    // Bowl body has no tonal modes above ~2.9 kHz, so this layer IS the
    // instrument's onset brightness carrier: 4.2 kHz center / 11 kHz rim
    // keeps the position law (rim = brighter) while putting real energy in
    // the 4-12 kHz band (rig: hf8k_frac 0 -> measurable), still an octave
    // below hi-hat brightness.
    float lpHz=4800.f+edge*6200.f; // center ~4.8 kHz .. rim ~11 kHz
    v.transCoef=1.f-(float)std::exp(-2.0*M_PI*(double)lpHz/(double)sampleRate_);
    uint32_t seed=(uint32_t)++strikeSeq_;
    seed^=seed>>16; seed*=2654435761u; seed^=seed>>16;   // splitmix-style scramble
    v.rngState=seed|1u;                                  // xorshift32 state must be nonzero
    v.burstLeft=v.burstLen;
}
void MultiScaleBodyEngine::recomputeVoiceCoeffs(Voice& v) {
    // Radiation aperture per body (~L/4). Sized implicitly: adding/removing a preset
    // in ModalData.hpp without updating this list now fails to compile instead of
    // silently clamping new bodies onto an old radius.
    static const double presetRad[] = {0.1125,0.07,0.1625,0.08,0.125,0.12,0.175,0.15,0.13,0.09,0.195,0.17,
                                       0.15,   0.16,  0.13,  0.10, 0.08,  0.07};
    static_assert(int(sizeof(presetRad)/sizeof(presetRad[0])) == kNumPresets,
                  "presetRad out of sync with modal::kNumPresets");
    double a = presetRad[presetIdx_];
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
        // exciter Q-compensation: injection scaled by (1-R_i) so the
        // steady-state driven amplitude ~ exciterGain*gains[i]/2 is
        // independent of Decay/Q (see Voice::excNorm comment).
        v.excNorm[i]=(float)(1.0 - R);
        // strike Q-compensation (audit #7, DELIBERATE paper-alignment):
        // the mallet force pulse is impulsive, so each mode's summitscale
        // is the peak of its impulse response h[n]=R^n*sin((n+1)t)/sin(t),
        // NOT 1/(1-R): closed-form maximum at (n*+1)t = atan2(t, eps)
        // (eps=-ln R), falling back to h[0]=1 when that lands before n=0.
        // Injecting the pulse through 1/M pins every mode's summit
        // contribution regardless of Decay/Q; plain excNorm would
        // over-attenuate long-decay modes (measured 29.7 dB spread).
        {
            const double eps = -std::log(R);
            const double phi = std::atan2(theta, eps);
            double m = 1.0; // impulse-response peak (h[0] = 1 fallback)
            if(phi > theta){
                const double nStar = phi/theta - 1.0;
                m = std::exp(-eps*nStar)*std::sin(phi)/std::sin(theta);
            }
            v.strikeNorm[i]=(float)(m > 1e-9 ? 1.0/m : 0.0);
            // Seed the effective normalizer with the analytic Dirac value.
            // startStrikeBurst() refines it per mode for the real pulse at
            // arm time; refreshing here (every coeff recompute: note-on,
            // pitch bend, decay/brightness/LFO changes) guarantees a stale
            // measured value can never survive a coefficient change.
            v.strikeEff[i]=v.strikeNorm[i];
        }
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
        float d=shapeDecayRate(p.decay[m])*decayScale_; // same ring shaping as voices
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
    // renormalize partial IR so loudness is stable while baking converges.
    // Peak normalization alone bounds nothing: a dense long-decay modal IR
    // correlates strongly with the engine's own ringing output over the whole
    // window (matched-filter gain ~ L1), so the wet send burst ~10x past clip
    // on every strike (= reported distortion). Cap per-channel L1 as well:
    // |conv| <= L1*max|x| <= kIrL1Max*0.85 keeps dry+wet below clipping for
    // ANY input, correlated or not.
    float mx=0,l1l=0,l1r=0;
    for(int i=0;i<kIrLen;++i){
        float al=std::abs(irL_[i]), ar=std::abs(irR_[i]);
        mx=std::max(mx,std::max(al,ar)); l1l+=al; l1r+=ar;
    }
    float inv=0.f;
    if(mx>1e-9f) inv=0.8f/mx;                       // loudness anchor when sparse
    if(std::max(l1l,l1r)>1e-6f) inv=std::min(inv,kIrL1Max/std::max(l1l,l1r));
    if(inv>0.f){ for(int i=0;i<kIrLen;++i){ irL_[i]*=inv; irR_[i]*=inv; } }
    return irBakeCursor_>=n;
}

void MultiScaleBodyEngine::setPreset(int idx) {
    idx = std::clamp(idx, 0, kNumPresets-1);
    presetIdx_ = idx;
    computeDecayRef(); // ring anchor is per-body — must follow the preset
    interpolateGainsFor(nextGain_, strikeX_, strikeY_);
    irDirty_=true;
}
void MultiScaleBodyEngine::setPitchScale(float v) {
    v = std::clamp(v,0.f,1.f);
    // Tune knob curve: +-24 ST total span (v=0.5 stays unity). The old +-12 ST
    // span (x2) rendered each body's ABSOLUTE modal spectrum at note 60 --
    // Bowl's lowest tonal mode is 559.6 Hz, so scored C4 was unreachable on
    // every preset (it needs -13.2..-17.7 ST; x4 reaches it at v=0.2256).
    pitchScale_ = std::pow(2.f, (v - 0.5f) * 4.f);
    for (auto& voice : voices_) if (voice.active) recomputeVoiceCoeffs(voice);
    irDirty_=true;
}
void MultiScaleBodyEngine::setDecayScale(float v) {
    v = std::clamp(v,0.f,1.f);
    // Per-mode pole radius is R = exp(-(decay[i]*decayScale_)/sr): decayScale_
    // multiplies the decay RATE, so a larger scale SHORTENS the tail. Invert
    // the knob curve so turning Decay UP lengthens the ring (user-reported:
    // "decay is reversed"). v=0.5 still maps to 1.0, so the default sound and
    // the 0.1x..10x range are unchanged — only the direction is mirrored.
    // stepIrBake() shares these semantics via the same decayScale_ multiply,
    // so the reverb send tracks the knob identically.
    decayScale_ = 0.1f * std::pow(100.f, 1.f - v);
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
    // Quartic taper (round A3): the DEFAULT position (0.15) lands at
    // ~1.40 ms. The paper's modal response (Eq. 1) has no VCA ramp — all
    // modes speak as soon as they are struck — and the bar render's onsets
    // rise in 0-0.9 ms; the round-2 cubic taper put the default at 3.7 ms,
    // which dominated the measured onset-rise metric (2.5-3.8 ms) even after
    // the drive itself was sharpened. Knob span unchanged: 1..800 ms,
    // strictly increasing, so every position keeps a useable time.
    attackMs_=1.f + std::pow(attackNorm_,4.f)*799.f;
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
            // DELIBERATE DEVIATION from paper 47 Eq. (1), documented (audit
            // #12b): the modeled body is free-ringing — nothing in the paper
            // damps it at note-off. As a PLAYABLE instrument we shape the
            // ring with a percussive release gate here: note-off ramps env
            // to zero over the Release knob time (20 ms..8 s, default
            // ~900 ms). The modes themselves keep ringing underneath; only
            // this VCA closes. Free-ring character survives via long
            // Release settings, CC64 sustain deferral and CC123 tails;
            // CC120 (allSoundOff) stays the hard mute. Chosen over removing
            // the gate because MIDI keyboards expect note-off to matter.
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
        v.midiNote=midiNote; v.midiChannel=channel; v.sustainHold=false;
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
        recomputeVoiceCoeffs(v);
        startStrikeBurst(v,vx,vy); // re-strike the contact pulse on mono retrigger
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
    v.age = nextAge_++;
    v.n = modeCount_;
    const auto& p = kPresets[presetIdx_];
    if (v.n > p.n) v.n = p.n;
    // velocity → strike position morph: soft=center/dark, hard=edge/bright
    float vx = strikeX_ + (vel01-0.5f)*velStrike_*0.6f;      // X spreads with velocity
    float vy = strikeY_ + (vel01-0.5f)*velStrike_*0.5f;      // Y drifts up (brighter rim)
    vx=std::clamp(vx,0.f,1.f); vy=std::clamp(vy,0.f,1.f);
    float gains[kMaxModes];
    interpolateGainsFor(gains,vx,vy);
    float noteShift = std::pow(2.f, (midiNote - 60) / 12.f);
    for (int i=0;i<v.n;++i) {
        v.freq[i] = p.freq[i]*noteShift;
        v.decay[i] = shapeDecayRate(p.decay[i]); // uniform-damping ring pull
        v.gain[i] = gains[i] * vel01;
        v.s1[i]=v.s2[i]=0.f;
    }
    for (int i=v.n;i<kMaxModes;++i) { v.gain[i]=0.f; v.s1[i]=v.s2[i]=0.f; }
    v.env=0.f; v.envState=Voice::Attack;
    recomputeVoiceCoeffs(v);
    startStrikeBurst(v,vx,vy);
    v.silenceCount = 0; // strike is carried by the force pulse, not a Dirac flag
    monoTopVoice_=target;
}

    // DELIBERATE DEVIATION from paper 47 Eq. (1) (audit #12b): the paper's
    // body rings freely — a strike simply excites Eq. (1) and physical
    // damping ends it. Here note-off arms the percussive Release VCA (see
    // updateEnvelope, Voice::Release): a documented playability feature,
    // not an oversight. The ring itself is untouched below the gate.
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
        v.burstLen=0; v.burstLeft=0; v.transLen=0; v.transLeft=0; v.transLP=0.f;
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
    // Exciter transient tracking into the Q-compensated resonators: a
    // peak-holding follower (instant attack, ~80 ms release) raises the drive
    // for the moment after an input rise, so percussive material kicks the
    // modes harder than steady tone. Deliberate extension beyond paper 47
    // (internal impulsive strikes only); bounded to (1+kExciteTrk)x.
    {
        const float ax = std::fabs(exciterIn_);
        exFollow_ = (ax>=exFollow_) ? ax : exFollow_*exFollowRel_;
    }
    const float exDrive = 1.f + kExciteTrk*exFollow_;
    for (auto& v : voices_) {
        if (!v.active) continue;
        updateEnvelope(v);
        if(!v.active) continue;
        float env = v.env;
        float voiceL=0.f, voiceR=0.f;
        // finite mallet-contact force pulse: raised cosine over burstLen
        // samples, unit net area (identical LF kick to the old Dirac, gently
        // band-limited top end = physical contact-time filtering)
        float force = 0.f;
        if(v.burstLeft>0){
            float ph = 1.f - (float)v.burstLeft/(float)v.burstLen; // 0..<1
            force = kStrikeTrim*(2.f/(float)v.burstLen)*(0.5f-0.5f*std::cos((float)(2.0*M_PI)*ph));
            --v.burstLeft;
        }
        int vIdx=0; for(int k=0;k<kVoiceCount;++k) if(&voices_[k]==&v) vIdx=k;
        float voicePan = ((float)vIdx / (kVoiceCount-1) - 0.5f) * widthCur_ * 0.6f;
        for (int i=0;i<v.n;++i) {
            float R = v.R[i];
            float c = v.cosTheta[i];
            float a1 = 2.f * R * c;
            float a2 = -R * R;
            float exc;
            // Mallet-contact pulse: injected through the per-mode impulsive
            // summit normalizer (audit #7 fix, DELIBERATE paper-alignment).
            // A raw pulse into a high-Q resonator builds a summit
            // proportional to the modal impulse-response peak, coupling
            // loudness to the Decay knob / preset decay table — the same bug
            // class cured on the exciter path. Round A3: the RT path uses
            // v.strikeEff[i], the pulse-shape-compensated normalizer that
            // startStrikeBurst() MEASURED from this very burst, so the
            // pinned-summit guarantee holds for the real raised-cosine
            // drive (v.strikeNorm[i] remains the analytic Dirac base value
            // strikeEff is seeded from). kStrikeTrim stages it once for
            // every Decay position. (excNorm=(1-R) is the SUSTAINED-drive
            // normalizer and would overshoot here by ~26 dB.)
            const float strikeExc = force * v.gain[i] * v.strikeEff[i];
            if(exMixCur_>1e-4f){
                // audio exciter: input drives modes through their gain weights,
                // normalized by (1-R_i) so sustained-input loudness does not
                // scale with resonator Q (Decay knob / preset decay table).
                // exDrive layers the transient tracking boost on top.
                exc = excite * v.gain[i] * v.excNorm[i] * 40.f * exDrive;
                exc += strikeExc; // mallet-contact pulse rides the same modes
            } else {
                exc = strikeExc;
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
        // Contact-transient layer: xorshift32 white noise -> one-pole LP
        // (bandwidth set by strike position at arm time) -> linear fade,
        // added to the voice sum co-incident with the force pulse. Gated by
        // the same env as the modes so release/steal also mutes it. Bounded
        // by construction (|tr| <= transAmp = kContactTrim*mean|gain|);
        // telemetry peak feeds contactTransientPeak() for tests.
        if(v.transLeft>0){
            uint32_t s=v.rngState; s^=s<<13; s^=s>>17; s^=s<<5; v.rngState=s;
            const float nz=(float)s*(1.f/2147483648.f)-1.f; // [-1,1)
            v.transLP+=v.transCoef*(nz-v.transLP);
            const float fade=(float)v.transLeft/(float)v.transLen;
            const float tr=v.transAmp*v.transLP*fade;
            const float atr=std::fabs(tr);
            if(atr>transPeak_) transPeak_=atr;
            const float angV=(voicePan+1.f)*0.25f*(float)M_PI;
            voiceL+=tr*std::cos(angV)*env;
            voiceR+=tr*std::sin(angV)*env;
            --v.transLeft;
        }
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
    // Tone LP last on the dry path (unchanged). The old tanh(1.2x)*0.85
    // soft-clip is REMOVED: it was the distortion the user reported when the
    // engine ran hot. Level safety now belongs entirely to the look-ahead
    // limiter at the very end of the signal chain.
    outL = lpL_.process(outL);
    outR = lpR_.process(outR);

    // === convolution reverb send (cheap block-free overlap via running tail) ===
    // Deliberate extension beyond paper 47: the body's own modal response doubles as the space IR - see README, Reverb as self-IR convolution.
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
    // === final stage: look-ahead brickwall limiter (stereo-linked) ====
    {
        // 1) slide the sample into the delay ring; the stereo-linked peak of
        //    the FRESH input feeds the detector
        limDelayL_[limPos_] = outL;
        limDelayR_[limPos_] = outR;
        const float x = std::max(std::abs(outL), std::abs(outR));
        // 2) monotonic wedge push: pop smaller-or-equal values off the back,
        //    then expire entries that fell out of the lookahead window.
        //    Amortized O(1): every index enters/leaves the wedge once.
        while (limWh_ > limWt_ && limWedgeV_[(limWh_ - 1u) & (unsigned)(kLimBuf - 1)] <= x)
            --limWh_;
        limWedgeV_[limWh_ & (unsigned)(kLimBuf - 1)] = x;
        limWedgeI_[limWh_ & (unsigned)(kLimBuf - 1)] = limAbs_;
        ++limWh_;
        while ((unsigned)(limAbs_ - limWedgeI_[limWt_ & (unsigned)(kLimBuf - 1)]) >= (unsigned)limLen_)
            ++limWt_;
        const float winPk = limWedgeV_[limWt_ & (unsigned)(kLimBuf - 1)];
        ++limAbs_;
        // 3) log-domain gain smoothing: instant attack (the lookahead window
        //    already contains everything this gain will be applied to),
        //    ~120 ms release toward unity. Below the ceiling the requested
        //    gain is exactly 1.0, so normal play is bit-transparent apart
        //    from the constant lookahead delay.
        const float gReqDb = (winPk > kLimCeil)
            ? 20.f * std::log10(kLimCeil / winPk) : 0.f;
        if (gReqDb < limGainDb_) limGainDb_ = gReqDb;
        else {
            limGainDb_ += (0.f - limGainDb_) * limRelCoef_;
            if (limGainDb_ > -1e-5f) limGainDb_ = 0.f; // denormal-safe snap
        }
        const float g = std::exp(limGainDb_ * 0.11512925464970229f); // ln(10)/20
        // 4) apply to the delayed signal: y[n] = g[n] * x[n-(len-1)]. The
        //    detector window ends at n and reaches back len samples, so every
        //    potentially clipping sample is attenuated by a gain computed
        //    AFTER it entered the window — mathematically bounded output.
        const unsigned mask = (unsigned)(kLimBuf - 1);
        const int rd = (int)(((unsigned)limPos_ + 1u - (unsigned)limLen_) & mask);
        outL = limDelayL_[rd] * g;
        outR = limDelayR_[rd] * g;
        limPos_ = (int)(((unsigned)limPos_ + 1u) & mask);
        // True-peak clamp at 0.98, kept AFTER the limiter purely as belt-and-
        // braces: it only ever engages on float rounding at the ceiling and
        // guarantees the hard 0.98 contract even if a future change breaks
        // the window/gain invariant. It is NOT a soft-clipper; no drive.
        outL = std::clamp(outL, -0.98f, 0.98f);
        outR = std::clamp(outR, -0.98f, 0.98f);
    }
}

// Limiter sizing/state reset. Called from prepare() (any SR change) — the only
// place allocation-free fixed state is re-derived; run() never allocates.
void MultiScaleBodyEngine::limPrepare(double sr) {
    // 3 ms lookahead: inside the 1..5 ms spec, covers the fastest modal
    // strike rise while keeping latency low. kLimBuf=1024 covers 192 kHz.
    int len = (int)std::ceil(0.003 * sr);
    limLen_ = std::clamp(len, 8, kLimBuf - 1);
    limRelCoef_ = 1.f - std::exp(-1.f / (0.120f * (float)sr)); // 120 ms release
    limReset();
}

void MultiScaleBodyEngine::limReset() {
    for (int i = 0; i < kLimBuf; ++i) { limDelayL_[i] = 0.f; limDelayR_[i] = 0.f; limWedgeV_[i] = 0.f; }
    for (int i = 0; i < kLimBuf; ++i) limWedgeI_[i] = 0u;
    limPos_ = 0; limWh_ = 0; limWt_ = 0; limAbs_ = 0;
    limGainDb_ = 0.f;
}

} // namespace modal
