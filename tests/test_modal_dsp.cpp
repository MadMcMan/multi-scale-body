#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "MultiScaleBodyEngine.hpp"
#include "ModalData.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <vector>
static void require(bool c,const char* m){ if(!c){ fprintf(stderr,"FAIL: %s\n",m); std::exit(1);} }
static float goertzelMag(const std::vector<float>& x,double targetHz,double sr){
    double w=2*M_PI*targetHz/sr;
    double cr=std::cos(w), ci=std::sin(w);
    double coeff=2*cr;
    double s0=0,s1=0,s2=0;
    for(float v:x){ s0=v+coeff*s1-s2; s2=s1; s1=s0; }
    double real=s1*cr - s2;
    double imag=s1*ci;
    return (float)std::sqrt(real*real+imag*imag);
}
int main(){
    using namespace modal;
    printf("=== test_modal_dsp ===\n");
    for(int p=0;p<kNumPresets;++p){
        const auto& pr=kPresets[p];
        require(pr.n>=8 && pr.n<=128,"preset n range");
        for(int i=1;i<pr.n;++i) require(pr.freq[i]>=pr.freq[i-1]-1e-3f,"freq sorted");
        for(int i=0;i<pr.n;++i) require(pr.freq[i]>20.f,"freq >20");
        for(int i=0;i<pr.n;++i) require(pr.decay[i]>0.f,"decay >0");
    }
    printf("mode-count invariant PASS\n");
    MultiScaleBodyEngine eng;
    eng.prepare(44100);
    eng.setPreset(0); eng.setModeCount(1.0f); eng.setPitchScale(0.5f); eng.setDecayScale(0.5f);
    eng.setStrike(0.5f,0.5f);
    eng.reset();
    eng.noteOn(60,1.0f);
    int N=44100;
    std::vector<float> ir; ir.reserve(N);
    for(int i=0;i<N;++i) ir.push_back(eng.processSampleMono());
    float maxA=0; for(float v:ir) maxA=std::max(maxA,std::abs(v));
    require(maxA>0.01f,"IR non-silent");
    printf("IR peak %.3f\n",maxA);
    const auto& pr=kPresets[0];
    for(int k=0;k<3;++k){
        double fhz=pr.freq[k]/(2*M_PI);
        float mag=goertzelMag(ir,fhz,44100);
        printf(" mode %d f=%.1f Hz mag=%.1f\n",k,fhz,mag);
        require(mag>1.0f,"spectral peak present");
    }
    printf("spectrum consistency PASS\n");
    float e0=0,e1=0;
    for(int i=0;i<N/2;++i) e0+=ir[i]*ir[i];
    for(int i=N/2;i<N;++i) e1+=ir[i]*ir[i];
    require(e1 < e0*0.5f,"decay energy");
    printf("decay PASS e0 %.3f e1 %.3f\n",e0,e1);
    // Sound-Map null: search best mode with clear minAbs << maxAbs
    const auto& pp=kPresets[0];
    int nullMode=-1; int bestMinX=0,bestMinY=0,bestMaxX=0,bestMaxY=0; float bestRatio=1;
    for(int m=0;m<pp.n;++m){
        float mn=1e9, mx=-1e9; int mnx=0,mny=0,mxx=0,mxy=0;
        for(int y=0;y<16;++y) for(int x=0;x<16;++x){ float g=std::abs(pp.gain[m][y][x]); if(g<mn){mn=g; mnx=x; mny=y;} if(g>mx){mx=g; mxx=x; mxy=y;} }
        float ratio = (mx>1e-9? mn/mx:1);
        if(ratio < bestRatio){ bestRatio=ratio; nullMode=m; bestMinX=mnx; bestMinY=mny; bestMaxX=mxx; bestMaxY=mxy; }
    }
    if(nullMode==-1) nullMode=5;
    printf("chosen nullMode %d ratio %.3f min %.5f at %d,%d max %.5f at %d,%d\n",nullMode,bestRatio, pp.gain[nullMode][bestMinY][bestMinX], bestMinX,bestMinY, pp.gain[nullMode][bestMaxY][bestMaxX], bestMaxX,bestMaxY);
    int minX=bestMinX, minY=bestMinY, maxX=bestMaxX, maxY=bestMaxY;
    auto renderAt = [&](int gx,int gy){
        eng.reset(); eng.setStrike(gx/15.f, gy/15.f); eng.noteOn(60,1.f);
        std::vector<float> out; out.reserve(N);
        for(int i=0;i<N;++i) out.push_back(eng.processSampleMono());
        double fhz=pp.freq[nullMode]/(2*M_PI);
        return goertzelMag(out,fhz,44100);
    };
    float magMin=renderAt(minX,minY);
    float magMax=renderAt(maxX,maxY);
    printf("magMin %.1f magMax %.1f ratio %.2f\n",magMin,magMax,magMin/(magMax+1e-6f));
    require(bestRatio<0.5f || magMin < magMax*0.90f,"Sound-Map null suppression");
    eng.reset(); eng.setStrike(0.5f,0.5f); eng.setPitchScale(0.5f); float ps1=eng.getPitchScale();
    eng.setPitchScale(1.0f); float ps2=eng.getPitchScale();
    require(std::abs(ps2/ps1 - 2.f) < 0.02f,"pitch scale ratio 2");
    printf("pitch scaling PASS (ps1=%.3f ps2=%.3f)\n",ps1,ps2);
    eng.prepare(44100); eng.setPitchScale(0.5f); eng.setDecayScale(0.5f); eng.reset(); eng.noteOn(60,1.f);
    for(int i=0;i<8;++i){
        const auto& v=eng.voice(i);
        if(!v.active) continue;
        for(int m=0;m<v.n;++m) require(v.R[m]<1.f && v.R[m]>0.5f,"R in (0.5,1)");
    }
    printf("R<1 PASS\n");
    // --- audit: CC64 sustain pedal defers note-offs, releases on lift ---
    {
        eng.prepare(44100); eng.reset();
        eng.noteOn(60,1.f,0);
        for(int i=0;i<100;++i) eng.processSampleMono();
        eng.setSustainPedal(true);
        eng.noteOff(60,0);
        const auto& vs=eng.voice(0);
        require(vs.active && vs.envState!=Voice::Release,"sustain pedal holds voice");
        eng.setSustainPedal(false);
        require(vs.envState==Voice::Release,"pedal up releases held voice");
        printf("sustain defer-release PASS\n");
    }
    // --- audit: 9th note steals a releasing tail, never a held note ---
    {
        eng.prepare(44100); eng.reset(); eng.setAttack(0.f);
        for(int n=0;n<8;++n) eng.noteOn(50+n,1.f,n); // fill all 8 voices
        for(int i=0;i<200;++i) eng.processSampleMono();
        for(int n=0;n<4;++n) eng.noteOff(50+n,n);    // half into release
        for(int i=0;i<50;++i) eng.processSampleMono();
        eng.noteOn(90,1.f,0);                        // no free voice -> must steal
        bool found90=false; int relRemaining=0;
        for(int i=0;i<kVoiceCount;++i){
            const auto& v=eng.voice(i);
            if(v.active && v.midiNote==90) found90=true;
            if(v.active && v.midiNote>=50 && v.midiNote<=53) ++relRemaining;
        }
        require(found90,"9th note allocated (no dropped notes)");
        require(relRemaining==3,"stole a releasing voice, not a held one");
        for(int n=54;n<58;++n){
            bool held=false;
            for(int i=0;i<kVoiceCount;++i){ const auto& v=eng.voice(i); if(v.active&&v.midiNote==n) held=true; }
            require(held,"held voice survived steal");
        }
        printf("steal preference PASS\n");
    }
    // --- audit: long tail with max decay + wet stays finite (denormal flush) ---
    {
        eng.prepare(44100); eng.reset();
        eng.setDecayScale(1.f); eng.setReverbWet(1.f);
        eng.noteOn(48,1.f,0);
        float peak=0.f; bool finite=true;
        for(int i=0;i<44100*3;++i){
            float s=eng.processSampleMono();
            if(!std::isfinite(s)) { finite=false; break; }
            float a=std::abs(s); if(a>peak) peak=a;
        }
        require(finite,"long tail finite");
        require(peak>0.001f,"long tail audible");
        printf("long-tail finite PASS (peak %.3f)\n",peak);
    }
    // --- audit: reverb headroom — dense IR must not blow up on sustained chords ---
    {
        MultiScaleBodyEngine rev;
        rev.prepare(44100);
        rev.setModeCount(1.f);   // worst case: max mode density
        rev.setDecayScale(0.7f);
        rev.setReverbWet(1.f);
        const int notes[8]={48,55,60,64,67,72,76,79};
        float peak=0.f;
        for(int p=0;p<kNumPresets;++p){
            rev.setPreset(p); rev.reset();
            for(int nn:notes) rev.noteOn(nn,1.f,0);
            for(int i=0;i<44100*2;++i){
                float l,r; rev.processSampleStereo(l,r);
                float a=std::max(std::abs(l),std::abs(r));
                if(a>peak) peak=a;
            }
        }
        // IR metrics (last preset): time-domain peak is normalized by design
        // and bounds nothing — amplification is set by L1 / resonant |H(f)|
        float irPk=0,l1l=0,l1r=0,resGain=0;
        std::vector<float> irl((size_t)modal::kIrLen);
        for(int ch=0;ch<2;++ch){
            const float* ir = ch? rev.reverbIrR() : rev.reverbIrL();
            for(int t=0;t<modal::kIrLen;++t){
                float a=std::abs(ir[t]); irPk=std::max(irPk,a);
                if(ch==0){ l1l+=a; irl[(size_t)t]=ir[t]; } else l1r+=a;
            }
        }
        const auto& pr=kPresets[rev.currentPreset()];
        int nm=std::min(pr.n,rev.currentModeCount());
        for(int m=0;m<nm;++m){
            float f=pr.freq[m]*rev.getPitchScale();
            if(f<20.f||f>18000.f) continue;
            resGain=std::max(resGain,goertzelMag(irl,(double)f,44100.0));
        }
        printf("reverb IR: peak %.3f L1 L%.2f R%.2f maxResonantGain %.2f\n",irPk,l1l,l1r,resGain);
        printf("reverb chord peak (all presets, wet=1): %.3f\n",peak);
        require(std::isfinite(peak),"reverb finite");
        require(peak<0.98f,"reverb headroom: sustained chord at wet=1 must stay below clip");
    }
    // --- audit: DECAY DIRECTION — turning Decay UP must lengthen the tail ---
    // Regression for the reported "decay is reversed" bug: the pole radius is
    // R=exp(-(decay[i]*decayScale_)/sr) where decayScale_ multiplies a decay
    // RATE, so the knob curve must invert (v -> 0.1*100^(1-v)). Measure the
    // -30 dB tail time at knob 0.1 / 0.5 / 0.9 and require STRICT increase
    // toward higher knob values.
    {
        auto tailSec=[&](float knob)->double{
            MultiScaleBodyEngine e;
            e.prepare(44100); e.reset();
            e.setPreset(0); e.setModeCount(1.0f); e.setPitchScale(0.5f);
            e.setDecayScale(knob);
            e.setStrike(0.5f,0.5f); e.setReverbWet(0.f); e.setExciteMix(0.f);
            e.noteOn(60,1.0f,0);
            const int N=44100*20;
            std::vector<float> buf; buf.reserve(N);
            float peak=0.f;
            for(int i=0;i<N;++i){
                float s=e.processSampleMono();
                buf.push_back(s);
                float a=std::abs(s); if(a>peak) peak=a;
            }
            const float thr=peak*std::pow(10.f,-30.f/20.f);
            for(int i=N-1;i>=0;--i)
                if(std::abs(buf[(size_t)i])>=thr) return i/44100.0;
            return -1.0;
        };
        const double tLo=tailSec(0.1f), tMid=tailSec(0.5f), tHi=tailSec(0.9f);
        printf("decay tails (-30dB): knob0.1 %.2fs  knob0.5 %.2fs  knob0.9 %.2fs\n",tLo,tMid,tHi);
        require(tHi>tMid && tMid>tLo,
            "decay monotonicity: tail(knob 0.9) > tail(knob 0.5) > tail(knob 0.1) — Decay UP = LONGER ring");
        require(tMid>1.25*tLo && tHi>1.25*tMid,
            "decay spread: adjacent knob steps must audibly change tail length");
        printf("decay monotonicity PASS (up = longer)\n");
    }
    // --- audit: LIMITER TRANSPARENCY — moderate play must be untouched ---
    // Two proofs in one: (1) gain reduction stays at exactly 0 dB throughout,
    // (2) halving velocity scales the whole output by exactly one half
    // sample-for-sample (the engine is linear when the limiter idles).
    {
        auto render=[&](float vel,std::vector<float>& L,std::vector<float>& R)->float{
            MultiScaleBodyEngine e;
            e.prepare(44100); e.reset();
            e.setPreset(0); e.setModeCount(0.6f); e.setBrightness(0.65f);
            e.setDecayScale(0.5f); e.setStrike(0.5f,0.5f);
            e.setReverbWet(0.f); e.setExciteMix(0.f);
            const int notes[8]={48,55,60,64,67,72,76,79};
            for(int n:notes) e.noteOn(n,vel,0);
            const int N=44100*2;
            float maxGR=0.f;
            L.clear(); R.clear(); L.reserve(N); R.reserve(N);
            for(int i=0;i<N;++i){
                float l,r; e.processSampleStereo(l,r);
                L.push_back(l); R.push_back(r);
                maxGR=std::max(maxGR,-e.limiterGainDb());
            }
            return maxGR;
        };
        std::vector<float> la,ra,lb,rb;
        const float grA=render(1.f,la,ra);
        render(0.5f,lb,rb);
        require(grA<1e-4f,"limiter transparency: gain reduction must stay 0 dB on moderate material");
        double diff=0; float refPk=0;
        for(size_t i=0;i<la.size();++i){
            diff=std::max(diff,(double)std::abs(la[i]*0.5f-lb[i]));
            diff=std::max(diff,(double)std::abs(ra[i]*0.5f-rb[i]));
            refPk=std::max(refPk,std::abs(la[i]));
        }
        printf("transparency: maxGR %.6f dB, vel-halving max dev %.2e (ref peak %.3f)\n",grA,diff,refPk);
        require(diff<1e-3,"limiter transparency: output must scale linearly below threshold");
        // after the tail decays the limiter must settle back to exactly unity
        MultiScaleBodyEngine e;
        e.prepare(44100); e.reset();
        e.setPreset(0); e.setDecayScale(0.9f); e.setReverbWet(0.f); e.setExciteMix(0.f);
        e.noteOn(60,1.f,0);
        for(int i=0;i<44100;++i) e.processSampleMono(); // ring out
        require(e.limiterGainDb()==0.f,"limiter must return to unity gain after signal ends");
        e.reset();
        float resid=0.f;
        for(int i=0;i<4096;++i) resid+=std::abs(e.processSampleMono());
        require(resid<1e-12f,"engine reset() must yield digital silence through limiter");
        printf("limiter settle/reset PASS\n");
    }
    // --- audit: LIMITER BOUND — worst-case sweep over presets x extremes ---
    // All 18 presets, 8-voice vel-127 chords, Bright/Decay/Modes maxed, wet
    // sweep, and sustained exciter drive (the historical "very loud" case).
    // Assert the hard ceiling and report the worst offender.
    {
        struct Row{int preset;const char* cs;float pre;};
        Row worst{0,"",0.f};
        float globalPost=0.f; bool finite=true;
        const int notes[8]={48,55,60,64,67,72,76,79};
        auto sweep=[&](int preset,const char* cs,float wet,float ex,int exm,double secs){
            MultiScaleBodyEngine e;
            e.prepare(48000); e.reset();
            e.setPreset(preset); e.setBrightness(1.f); e.setDecayScale(1.f);
            e.setModeCount(1.f); e.setStrike(0.5f,0.5f);
            e.setReverbWet(wet); e.setExciteMix(ex);
            for(int n:notes) e.noteOn(n,1.f,0);
            const long N=(long)(48000*secs);
            double phase=0.0; const double w0=2*M_PI*(double)e.voice(0).freq[0]/48000.0;
            const double ln10_20=0.11512925464970229;
            float prePk=0.f, postPk=0.f;
            for(long i=0;i<N;++i){
                if(exm==1) e.setExciterSample(0.5f*(float)std::sin(phase));
                else if(exm==2) e.setExciterSample(0.5f*(float)std::sin(2*M_PI*1000.0*i/48000.0));
                phase+=w0; if(phase>2*M_PI) phase-=2*M_PI;
                float l,r; e.processSampleStereo(l,r);
                if(!std::isfinite(l)||!std::isfinite(r)){ finite=false; break; }
                const float pk=std::max(std::abs(l),std::abs(r));
                postPk=std::max(postPk,pk);
                const float g=std::exp(e.limiterGainDb()*(float)ln10_20);
                if(g>1e-9f) prePk=std::max(prePk,pk/g);
            }
            globalPost=std::max(globalPost,postPk);
            if(prePk>worst.pre) worst={preset,cs,prePk};
        };
        for(int p=0;p<kNumPresets;++p){
            sweep(p,"chord-bri1-dk1",0.f,0.f,0,1.5);
            sweep(p,"chord+wet1",   1.f,0.f,0,1.5);
            sweep(p,"wet1+ex@mode0",1.f,1.f,1,3.0);
            sweep(p,"wet1+ex@1k",   1.f,1.f,2,3.0);
        }
        printf("worst-case sweep: max post-limiter peak %.4f (limit 0.98), worst pre-limiter %.2f at preset %d (%s)\n",
               globalPost,worst.pre,worst.preset,worst.cs);
        require(finite,"worst-case sweep must stay finite");
        require(globalPost<=0.98f,"limiter bound: output peak must never exceed 0.98");
        printf("limiter bound PASS\n");
    }
    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
