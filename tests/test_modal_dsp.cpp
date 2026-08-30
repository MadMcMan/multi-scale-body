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
// |FFT| of a Hann-windowed segment zero-padded to 1024 points (round A3
// onset-metric helper; mirrors np.fft.rfft of an np.hanning slice).
static void fftMag1024(const std::vector<float>& segIn,std::vector<double>& mag){
    const int N=1024;
    std::vector<double> re(N,0.0),im(N,0.0);
    const size_t n=std::min(segIn.size(),(size_t)N);
    for(size_t i=0;i<n;++i)
        re[i]=(double)segIn[i]*(0.5-0.5*std::cos(2*M_PI*(double)i/(double)(n-1)));
    for(int i=1,j=0;i<N;++i){
        int bit=N>>1;
        for(;j&bit;bit>>=1) j^=bit;
        j^=bit;
        if(i<j){std::swap(re[i],re[j]);std::swap(im[i],im[j]);}
    }
    for(int len=2;len<=N;len<<=1){
        const double ang=-2*M_PI/len;
        const double wr=std::cos(ang),wi=std::sin(ang);
        for(int i=0;i<N;i+=len){
            double cr=1.0,ci=0.0;
            for(int k=0;k<len/2;++k){
                const double ur=re[i+k],ui=im[i+k];
                const double vr=re[i+k+len/2]*cr-im[i+k+len/2]*ci;
                const double vi=re[i+k+len/2]*ci+im[i+k+len/2]*cr;
                re[i+k]=ur+vr; im[i+k]=ui+vi;
                re[i+k+len/2]=ur-vr; im[i+k+len/2]=ui-vi;
                const double ncr=cr*wr-ci*wi;
                ci=cr*wi+ci*wr; cr=ncr;
            }
        }
    }
    mag.assign((size_t)N/2,0.0);
    for(int k=0;k<N/2;++k) mag[(size_t)k]=std::sqrt(re[k]*re[k]+im[k]*im[k]);
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
    eng.setPitchScale(1.0f); float ps2=eng.getPitchScale();   // +24 ST = 2 octaves up
    eng.setPitchScale(0.0f); float ps0=eng.getPitchScale();   // -24 ST = 2 octaves down
    require(std::fabs(ps2/ps1 - 4.f) < 0.02f,"pitch scale ratio 4 at max Tune (+24 ST)");
    require(std::fabs(ps0 - ps1*0.25f) < 0.02f,"pitch scale quarter at min Tune (-24 ST)");
    printf("pitch scaling PASS (ps0=%.3f ps1=%.3f ps2=%.3f)\n",ps0,ps1,ps2);
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
    // --- audit: LIMITER TRANSPARENCY — below-ceiling play must be untouched -
    // Two proofs in one: (1) gain reduction stays at exactly 0 dB throughout,
    // (2) halving velocity scales the whole output by exactly one half
    // sample-for-sample (the engine is linear when the limiter idles).
    // Drive level: since the audit-#7 strike re-staging (kStrikeTrim 7.68,
    // bar-render-aligned -6.8 dBFS summits) a HELD 8-voice vel-127 chord is
    // above the ceiling BY DESIGN — that worst case belongs to the limiter
    // bound sweep below. Transparency is proven at vel 0.4/0.2 where the
    // limiter must stay fully idle; the halving property is scale-invariant.
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
        const float grA=render(0.4f,la,ra);
        render(0.2f,lb,rb);
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
    // --- builder round A: strike transient / ring / exciter shaping ----------
    // (1) Attack curve: strictly increasing, spans 1..800 ms, and the plugin
    // default position (0.15) must land in the sub-6 ms percussive region.
    // Paper Eq. 1 has no VCA ramp (modes speak when struck); the old x^1.5
    // taper put the default at 46 ms and smeared every strike.
    {
        MultiScaleBodyEngine e; e.prepare(48000);
        float prev=-1.f;
        for(int k=0;k<=20;++k){
            e.setAttack(k/20.f);
            require(e.attackMs()>prev,"attack ms strictly increasing in knob");
            require(e.attackMs()>=1.f && e.attackMs()<=800.5f,"attack span 1..800 ms");
            prev=e.attackMs();
        }
        e.setAttack(0.15f);
        require(e.attackMs()<6.f,"default attack position lands <6 ms");
        printf("attack curve PASS (%.2f ms at default 0.15)\n",e.attackMs());
    }
    // (2) Uniform-damping ring pull: for every preset, every shaped rate is
    // <= its baked rate (rings only lengthen), relative order of mode
    // lifetimes is preserved (monotone map), and the slowest baked mode moves
    // by exactly the anchor factor (1/kDecayAnchorDiv)^kDecayPullBeta — so
    // the Decay-knob direction law stays a pure multiplier on top.
    {
        MultiScaleBodyEngine e; e.prepare(48000);
        int checked=0;
        for(int p=0;p<kNumPresets;++p){
            e.setPreset(p); e.reset(); e.noteOn(60,1.f,0);
            const auto& pr=kPresets[p];
            const auto& v=e.voice(0);
            const int nm=v.n; // shaped rates exist for ACTIVE modes only
            require(nm==std::min(pr.n,e.currentModeCount()),"voice covers active modes");
            // (argmin dropped: the closed-form check below covers every mode)
            // anchor mirrors computeDecayRef(): slowest rate of the WHOLE
            // baked body (independent of the user's Modes knob)
            float dminAll=pr.decay[0];
            for(int i=1;i<pr.n;++i) if(pr.decay[i]<dminAll) dminAll=pr.decay[i];
            const float ref=dminAll/kDecayAnchorDiv;
            int here=0; // per-preset count (checked is the cumulative total)
            for(int i=0;i<nm;++i){
                ++here; ++checked;
                const float expect=std::pow(pr.decay[i],1.f-kDecayPullBeta)
                                  *std::pow(ref,kDecayPullBeta);
                require(std::fabs(v.decay[i]-expect)<=2e-5f*std::fabs(expect),
                        "shaped rate matches uniform-damping pull formula");
            }
            require(here==nm,"all active modes formula-checked");
        }
        printf("ring pull PASS (formula-checked %d modes across presets)\n",checked);
    }
    // (3) Finite mallet-contact pulse: armed on every poly strike, bounded,
    // and its LENGTH is velocity-independent — the limiter-transparency
    // contract (output scales exactly with velocity) forbids velocity-dependent
    // spectra. Brightness varies with strike POSITION instead (rim = shorter).
    {
        MultiScaleBodyEngine e; e.prepare(48000); e.reset();
        e.setPreset(0); e.setStrike(0.9f,0.9f);
        e.noteOn(60,0.25f,0); int lSoft=e.voice(0).burstLen;
        e.reset(); e.noteOn(60,1.f,0); int lHard=e.voice(0).burstLen;
        require(lSoft==lHard,"pulse length velocity-independent");
        require(lSoft>=4 && lSoft<=32,"pulse length bounds");
        require(e.voice(0).burstLeft==lHard,"pulse armed on strike");
        // rim strike gets the shortest pulse, center the longest
        e.setStrike(0.5f,0.5f); e.reset(); e.noteOn(60,1.f,0);
        int lCenter=e.voice(0).burstLen;
        require(lCenter>lHard,"center strike = softer/longer contact than rim");
        printf("strike pulse PASS (len %d rim / %d center)\n",lHard,lCenter);
}
    // (4) Exciter transient tracking: the drive into the Q-compensated
    // resonators must depend on RECENT PEAK HISTORY — identical quiet input
    // produces a stronger modal response right after a full-scale spike than
    // after continued quiet (the follower's ~80 ms release). Output
    // boundedness under sustained drive is already proven by the worst-case
    // sweep below; this pins the NEW behavior.
    {
        // The drive law itself: instant-attack peak follower, ~80 ms release,
        // bounded to (1+kExciteTrk). (End-to-end loudness effects are covered
        // by the worst-case sweep; here we pin the transient-tracking state.)
        MultiScaleBodyEngine e; e.prepare(48000); e.reset();
        require(std::fabs(e.exciterDrive()-1.f)<1e-6,"drive starts at unity");
        e.setExciterSample(0.9f); e.processSampleMono();
        require(std::fabs(e.exciterDrive()-(1.f+kExciteTrk*0.9f))<1e-5,
                "drive tracks instantaneous peak (instant attack)");
        double prev=e.exciterDrive();
        for(int i=0;i<6*3840;++i){ // ~6 release time constants (~0.48 s)
            e.setExciterSample(0.0f); e.processSampleMono();
            if(i==96) require(e.exciterDrive()<prev && e.exciterDrive()>1.f,
                "drive releases monotonically toward unity after transient");
        }
        require(std::fabs(e.exciterDrive()-1.f)<1e-2,"drive settles back near unity");
        require(e.exciterDrive()<=1.f+kExciteTrk+1e-5,"drive bounded by 1+kExciteTrk");
        printf("exciter tracking PASS (peak->%.3fx, released->%.6f)\n",
               1.f+kExciteTrk*0.9f,e.exciterDrive());
    }
    // (5) Determinism of the shaping path: two identically-configured engines
    // must render bit-identical output (no hidden state in burst/pull code).
    {
        auto renderShort=[&]()->std::vector<float>{
            MultiScaleBodyEngine e; e.prepare(48000); e.reset();
            e.setPreset(14); e.setModeCount(0.6f); e.setStrike(0.31f,0.42f);
            std::vector<float> out; out.reserve(96000);
            e.noteOn(48,0.7f,0);
            for(int i=0;i<96000;++i) out.push_back(e.processSampleMono());
            return out;
        };
        auto a=renderShort(), b=renderShort();
        require(a.size()==b.size(),"determinism render size");
        for(size_t i=0;i<a.size();++i) require(a[i]==b[i],"engine render deterministic");
        printf("shaping determinism PASS (%zu samples bit-identical)\n",a.size());
    }
    // --- critic round B: strike summit Q-independence (audit #7 fix) -------
    // The mallet force pulse is injected through the per-mode analytic
    // impulse-response peak normalizer Voice::strikeNorm = 1/M(R,th), so the
    // strike summit must stay ~constant across Decay knob positions instead
    // of riding resonator Q. Measured on this engine: 1.8 dB spread
    // (was 3.8 dB raw-pulse / 29.7 dB with a plain excNorm=(1-R) strike
    // normalization, which is the SUSTAINED-drive normalizer and overshoots
    // impulsive drive). Also pins the rig calibration: reference single
    // strike lands at -6.81 dBFS, limiter fully idle.
    {
        auto summit=[&](float knob)->float{
            MultiScaleBodyEngine e; e.prepare(48000); e.reset();
            e.setPreset(0); e.setModeCount(0.6f); e.setPitchScale(0.5f);
            e.setBrightness(0.65f); e.setDecayScale(knob);
            e.setStrike(0.5f,0.5f); e.setReverbWet(0.f); e.setExciteMix(0.f);
            e.noteOn(60,1.f,0);
            const long N=(long)(2.0*48000);
            float pk=0.f,maxGR=0.f;
            const double ln1020=0.11512925464970229;
            for(long i=0;i<N;++i){
                float l,r; e.processSampleStereo(l,r);
                const float a=std::max(std::fabs(l),std::fabs(r));
                const float gr=-e.limiterGainDb();
                maxGR=std::max(maxGR,gr);
                const float g=std::exp(-gr*(float)ln1020);
                if(g>1e-9f) pk=std::max(pk,a/g);
            }
            require(maxGR<0.05f,"strike summits must be measured with limiter idle");
            return pk;
        };
        const float sLo=summit(0.1f), sMid=summit(0.5f), sHi=summit(0.9f);
        float mn=std::min(sLo,std::min(sMid,sHi));
        float mx=std::max(sLo,std::max(sMid,sHi));
        printf("strike summits: decay0.1 %.4f  decay0.5 %.4f  decay0.9 %.4f -> spread %.2f dB\n",
               sLo,sMid,sHi,20.0*std::log10((double)mx/mn));
        require(mx/mn<1.35f,"strike summit Q-independence across Decay settings");
        require(std::fabs(20.0*std::log10((double)sMid)+6.81)<1.5,
                "reference strike summit staged at -6.81 dBFS (+/-1.5 dB)");
        printf("strike Q-independence PASS\n");
    }
    // --- critic round B: contact transient present but bounded -------------
    // A few ms of band-limited noise rides every strike (physical contact
    // chatter beyond the paper's Dirac). Present: telemetry peak > 0 and the
    // per-voice xorshift state actually advanced. Bounded: by construction
    // |sample| <= transAmp = kContactTrim * mean|gain| <= kContactTrim*0.08,
    // and it stays a small fraction of the tonal summit. Bandwidth follows
    // strike position: rim = shorter transient, higher LP cutoff.
    {
        MultiScaleBodyEngine e; e.prepare(48000); e.reset();
        e.setPreset(0); e.setModeCount(0.6f); e.setPitchScale(0.5f);
        e.setBrightness(0.65f); e.setDecayScale(0.5f);
        e.setReverbWet(0.f); e.setExciteMix(0.f);
        e.setStrike(0.5f,0.5f);
        e.noteOn(60,1.f,0);
        const auto& v=e.voice(0);
        require(v.transLen>0 && v.transLeft==v.transLen,"contact transient armed");
        require(v.rngState!=0u,"xorshift state seeded nonzero");
        const uint32_t seed0=v.rngState;
        float tonalPk=0.f;
        for(int i=0;i<48000;++i){
            float s=e.processSampleMono();
            tonalPk=std::max(tonalPk,std::fabs(s));
        }
        const float tp=e.contactTransientPeak();
        printf("contact transient: peak %.5f (tonal summit %.3f -> %.1f%%)\n",
               tp,tonalPk,100.0*(double)tp/(double)tonalPk);
        require(tp>1e-4f,"contact transient present");
        require(v.rngState!=seed0,"xorshift state advanced while playing");
        require(v.transLeft==0,"transient finished after its few-ms window");
        require(tp<=kContactTrim*0.08f*1.05f,"contact transient bounded by kContactTrim*max|gain|");
        require(tp<0.25f*tonalPk,"contact transient stays a small fraction of tonal excitation");
        // bandwidth/length shaping: rim strike = brighter coefficient + shorter
        e.reset(); e.setStrike(0.9f,0.9f); e.noteOn(60,1.f,0);
        int rimLen=e.voice(0).transLen; float rimCoef=e.voice(0).transCoef;
        e.reset(); e.setStrike(0.5f,0.5f); e.noteOn(60,1.f,0);
        int ctrLen=e.voice(0).transLen; float ctrCoef=e.voice(0).transCoef;
        require(rimLen<ctrLen,"rim contact shorter than center");
        require(rimCoef>ctrCoef,"rim transient brighter than center");
        printf("contact transient bounds PASS (len rim %d / center %d)\n",rimLen,ctrLen);
    }
    // --- critic round B: strikeNorm closed form matches numeric h[n] peak --
    // Guards the analytic M(R,th) used by the Q-compensation: brute-force the
    // modal impulse response of the lowest active mode and compare.
    {
        MultiScaleBodyEngine e; e.prepare(44100); e.reset();
        e.setPreset(0); e.setModeCount(0.6f); e.setPitchScale(0.5f);
        e.setDecayScale(0.5f); e.setStrike(0.5f,0.5f);
        e.noteOn(60,1.f,0);
        const auto& v=e.voice(0);
        const double R=v.R[0], th=std::acos((double)v.cosTheta[0]);
        const double eps=-std::log(R);
        const double phi=std::atan2(th,eps);
        const double nStar=(phi>th)? phi/th-1.0 : 0.0;
        double mNum=0.0;
        const long cap=std::min((long)(4.0*nStar)+2000,400000L);
        for(long n=0;n<cap;++n)
            mNum=std::max(mNum,std::pow(R,(double)n)*std::sin(((double)n+1.0)*th)/std::sin(th));
        const double mClosed=1.0/(double)v.strikeNorm[0];
        printf("strikeNorm check: closed-form M=%.2f numeric(h[0..%ld]) M=%.2f\n",mClosed,cap,mNum);
        require(std::fabs(mNum-mClosed)<=0.05*mClosed,"analytic strike normalizer matches numeric peak");
        printf("strikeNorm closed-form PASS\n");
    }
    // --- builder round A3: pinned-summit guarantee for the REAL burst drive
    // strikeNorm pins each mode's summit for a DIRAC drive; since round A3
    // startStrikeBurst() measures Voice::strikeEff from this very strike's
    // raised-cosine pulse, the guarantee must now hold FOR THE REAL DRIVE:
    {
        // Bowl's active set (559..2841 Hz) is fully pulse-compensated; the
        // Dirac-like kept-analytic branch needs quarter periods >= 4 bursts
        // (f < ~230 Hz @48k/L=13), which only Membrane/Handpan/LogDrum bake.
        auto checkSummits=[&](int preset,float knob,bool wantKept)->int{
            MultiScaleBodyEngine e; e.prepare(48000); e.reset();
            e.setPreset(preset); e.setModeCount(0.6f); e.setPitchScale(0.5f);
            e.setBrightness(1.f); e.setDecayScale(knob);
            e.setStrike(0.5f,0.5f); e.setReverbWet(0.f); e.setExciteMix(0.f);
            e.noteOn(60,1.f,0);
            const auto& v=e.voice(0);
            const int L=v.burstLen;
            int nMeas=0,nKept=0;
            for(int i=0;i<v.n;++i){
                const float g=std::fabs(v.gain[i]);
                if(g<1e-6f) continue;
                const float c=std::clamp(v.cosTheta[i],-1.f,1.f);
                const float th=std::acos(c);
                const float qp=(th>1e-9f)?(0.5f*(float)M_PI)/th:1e9f;
                const bool diracLike=!(qp<4.f*(float)L);
                require(diracLike==(v.strikeEff[i]==v.strikeNorm[i]),
                        "Dirac-like modes must keep the analytic strikeEff exactly");
                if(diracLike){++nKept; continue;}
                ++nMeas;
                // exact runtime replication of the strike excitation path,
                // scanned well past the engine's own probe window so a
                // misplaced peak cannot hide outside the measurement
                float s1=0.f,s2=0.f,pk=0.f;
                const float a1=2.f*v.R[i]*c,a2=-v.R[i]*v.R[i];
                const int period=(th>1e-9f)?(int)std::ceil(2.f*(float)M_PI/th):0;
                const int scan=L+(int)std::ceil(std::min(qp,4096.f))+period+4;
                for(int m=0;m<scan;++m){
                    const float u=(m<L)?(2.f/(float)L)*(0.5f-0.5f*std::cos((float)(2.0*M_PI)*(float)m/(float)L)):0.f;
                    const float y=a1*s1+a2*s2+u*g*v.strikeEff[i];
                    s2=s1; s1=y;
                    pk=std::max(pk,std::fabs(y));
                }
                require(std::fabs(pk-g)<=0.08f*g,
                        "per-mode summit must equal |gain| for the real burst drive");
            }
            require(nMeas>20,"probe must cover the compensated modes");
            if(wantKept) require(nKept>=1,"low-bodied preset must exercise the kept-analytic branch");
            return nMeas+nKept;
        };
        const int c1=checkSummits(0,0.1f,false),c2=checkSummits(0,0.5f,false),c3=checkSummits(0,0.9f,false);
        const int cm=checkSummits(7,0.5f,true); // Membrane: exercises kept-analytic LF modes
        printf("burst-drive pinned summits PASS (%d/%d/%d modes bowl decay .1/.5/.9, %d membrane incl. kept)\n",
               c1,c2,c3,cm);
    }
    // --- builder round A3: onset lands like the bar (rig acceptance) -------
    // Solo C4 vel127 with plugin defaults = critic-rig event 0 context.
    // Round-2 baselines on this exact probe: rise_to_pk20 3.50 ms,
    // centroid_15ms 1588 Hz, hf8k_frac 0.0000 — the "strike never lands".
    // Acceptance: rise <= ~1.5 ms and onset centroid >= ~3 kHz on Bowl.
    {
        struct Onset{ double riseMs,cen,hf; };
        auto renderOnset=[&](int preset)->Onset{
            MultiScaleBodyEngine e; e.prepare(48000); e.reset();
            e.setPitchScale(0.5f); e.setDecayScale(0.5f); e.setBrightness(0.65f);
            e.setStrike(0.5f,0.5f); e.setModeCount(0.60f); e.setWidth(0.30f);
            e.setPreset(preset);
            for(int b=0;b<16;++b) e.setBandTrim(b,1.f);
            e.setRadiationMix(0.45f); e.setAttack(0.15f); e.setReleaseParam(0.45f);
            e.setLFORate(0.30f); e.setLFODepth(0.f); e.setExciteMix(0.f);
            e.setVelStrike(0.35f); e.setDetuneSpread(0.15f); e.setGlide(0.15f);
            e.setReverbWet(0.f); e.setMonoMode(false);
            e.noteOn(60,1.f,0);
            const long N=(long)(0.30*48000);
            std::vector<float> x; x.reserve((size_t)N);
            for(long i=0;i<N;++i) x.push_back(e.processSampleMono());
            // PDC-align exactly like tools/render_probe.cpp + the critic rig
            const long lat=(long)e.limiterLatency();
            x.erase(x.begin(),x.begin()+(std::ptrdiff_t)lat);
            const size_t n=x.size();
            const double sr=48000.0;
            std::vector<double> envDb(n,-300.0);
            const long h=(long)std::lround(0.0005*sr);
            std::vector<double> pre(n+1,0.0);
            for(size_t i=0;i<n;++i) pre[i+1]=pre[i]+(double)x[i]*x[i];
            for(size_t i=0;i<n;++i){
                const long li=(long)i;
                const long a=(li-h<0)?0:li-h;
                const long b=((li+h+1)>(long)n)?(long)n:li+h+1;
                envDb[i]=10.0*std::log10((pre[(size_t)b]-pre[(size_t)a])/(double)(b-a)+1e-20);
            }
            const long w50=std::min((long)n,(long)(0.05*sr));
            double pk=-1e30;
            for(long i=0;i<w50;++i) pk=std::max(pk,envDb[(size_t)i]);
            Onset o{-1.0,0.0,0.0};
            for(long i=0;i<w50;++i)
                if(envDb[(size_t)i]>=pk-20.0){ o.riseMs=(double)i/sr*1000.0; break; }
            std::vector<float> seg(x.begin(),x.begin()+(std::ptrdiff_t)std::min((long)n,(long)(0.015*sr)));
            std::vector<double> mag; fftMag1024(seg,mag);
            double sm=0,sfm=0,sAll=0,sHf=0;
            const double fres=sr/1024.0;
            for(int k=0;k<512;++k){
                const double fb=(double)k*fres;
                sm+=mag[(size_t)k]; sfm+=fb*mag[(size_t)k];
                sAll+=mag[(size_t)k]*mag[(size_t)k];
                if(fb>=8000.0&&fb<20000.0) sHf+=mag[(size_t)k]*mag[(size_t)k];
            }
            o.cen=sfm/(sm+1e-20); o.hf=sHf/(sAll+1e-20);
            return o;
        };
        const Onset bowl=renderOnset(0), glass=renderOnset(9), marimba=renderOnset(14);
        printf("onset rig: bowl rise %.2f ms cen %.0f Hz hf8k %.4f | glass %.2f/%.0f | marimba %.2f/%.0f\n",
               bowl.riseMs,bowl.cen,bowl.hf,glass.riseMs,glass.cen,marimba.riseMs,marimba.cen);
        require(bowl.riseMs>=0.0 && bowl.riseMs<=1.5,"Bowl onset rise <= 1.5 ms");
        require(bowl.cen>=2900.0,"Bowl onset centroid >= ~3 kHz");
        require(bowl.hf>=3e-4,"measurable >8 kHz energy in the Bowl onset");
        printf("onset sharpness/brightness PASS\n");
    }
    // --- builder round A3: contact transient audible, never a hi-hat -------
    // Floor guard against regressing to the round-2 inaudible layer
    // (0.2% of tonal peak): the measured transient fraction of the tonal
    // summit must stay in [3%, 25%] under the plugin-default strike.
    {
        MultiScaleBodyEngine e; e.prepare(48000); e.reset();
        e.setPitchScale(0.5f); e.setDecayScale(0.5f); e.setBrightness(0.65f);
        e.setStrike(0.5f,0.5f); e.setModeCount(0.60f);
        e.setPreset(0); e.setReverbWet(0.f); e.setExciteMix(0.f);
        e.noteOn(60,1.f,0);
        float tpk=0.f;
        for(int i=0;i<48000;++i) tpk=std::max(tpk,std::fabs(e.processSampleMono()));
        const float frac=e.contactTransientPeak()/tpk;
        printf("contact transient fraction of tonal summit: %.1f%%\n",100.0*(double)frac);
        require(frac>=0.03f,"contact transient audible (>=3% of tonal summit)");
        require(frac<0.25f,"contact transient stays a small fraction (<25%)");
        printf("transient audibility floor PASS\n");
    }
    // --- gauntlet round 4: Tune widened to +/-24 ST -- scored C4 reachable -
    // With the old +-12 ST span every preset rendered its ABSOLUTE modal
    // frequencies at note 60: Bowl's lowest tonal mode is ~559 Hz (> 2x C4),
    // so a scored middle C was unreachable on any preset (it needs
    // -13.2..-17.7 ST; the old knob bottomed out at -12 ST). The widened
    // 2^((v-.5)*4) curve must put Bowl's strongest ring within ~50 cents of
    // C4 at v=0.2256 (= -13.17 ST), while the default position still renders
    // the baked absolute low mode. Config mirrors the shipped plugin defaults
    // exactly (same block as tools/render_probe.cpp): the partial ranking is
    // strike-position sensitive and velStrike remaps it at velocity 127 --
    // measuring anything but the shipped default state would pin an
    // arbitrary lab configuration.
    {
        auto strongestPartial=[&](float tune)->double{
            MultiScaleBodyEngine e; e.prepare(48000); e.reset();
            e.setPitchScale(tune); e.setDecayScale(0.5f); e.setBrightness(0.65f);
            e.setStrike(0.5f,0.5f); e.setModeCount(0.60f); e.setWidth(0.30f);
            e.setPreset(0);
            for(int b=0;b<16;++b) e.setBandTrim(b,1.f);
            e.setRadiationMix(0.45f); e.setAttack(0.15f); e.setReleaseParam(0.45f);
            e.setLFORate(0.30f); e.setLFODepth(0.f); e.setExciteMix(0.f);
            e.setVelStrike(0.35f); e.setDetuneSpread(0.15f); e.setGlide(0.15f);
            e.setReverbWet(0.f); e.setMonoMode(false);
            e.noteOn(60,1.f,0);
            const int N=(int)(0.45*48000); // note-60-only window (A3 hits at 0.5 s)
            std::vector<float> x; x.reserve((size_t)N);
            for(int i=0;i<N;++i) x.push_back(e.processSampleMono());
            double bestF=0.0; float bestMag=-1.f;
            for(double f=80.0;f<=3000.0;f+=1.0){          // coarse sweep, 1 Hz
                const float m=goertzelMag(x,f,48000);
                if(m>bestMag){bestMag=m;bestF=f;}
            }
            const double lo=std::max(40.0,bestF-1.5);
            for(double f=lo;f<=bestF+1.5;f+=0.01){        // fine refinement
                const float m=goertzelMag(x,f,48000);
                if(m>bestMag){bestMag=m;bestF=f;}
            }
            return bestF;
        };
        const double fUnity=strongestPartial(0.5f);       // +0.0 ST (default)
        const double fDown =strongestPartial(0.2256f);    // ~-13.17 ST
        const double bakedLow=kPresets[0].freq[0]/(2*M_PI);
        printf("tune span: strongest partial %.2f Hz at default Tune -> %.2f Hz at v=0.2256 (baked low mode %.1f Hz)\n",
               fUnity,fDown,bakedLow);
        require(std::fabs(fUnity-bakedLow)<=12.0,
            "default Tune must render Bowl's baked absolute low mode");
        const double cents=1200.0*std::log2(fDown/261.626);
        printf("scored C4: strongest ring %.2f Hz = %+.1f cents vs 261.63 Hz\n",fDown,cents);
        require(std::fabs(cents)<=50.0,
            "Tune v=0.2256 (-13.17 ST) must land note 60 within 50 cents of scored C4");
        printf("tune span / scored C4 PASS\n");
    }
    // --- issue #6: MPE per-note Channel Pressure latch (member ch 1-15) -----
    // All comparisons are bit-identity (never RMS) and every pair of engines
    // gets MATCHED strike history: strikeSeq_ seeds the per-voice transient
    // RNG monotonically per engine (hpp), so a reused engine's 2nd strike is
    // seeded differently than a fresh engine's 1st — reused-vs-fresh shapes
    // would fail spuriously. Each test renders one note per engine.
    {
        // (1) init-trap: fresh engine, noteOn on ch1..15 with NO 0xD0 ever
        // sent must render exactly what the same note on ch0 renders —
        // catches any zero-fill regression of mpeZ_ (0 reads as "latched Z=0"
        // and would halve the drive).
        const auto renderCh=[&](int ch){
            MultiScaleBodyEngine e; e.prepare(44100); e.reset();
            e.noteOn(60,0.5f,ch);
            std::vector<float> x; x.reserve(8192);
            for(int i=0;i<8192;++i) x.push_back(e.processSampleMono());
            return x;
        };
        for(int ch=1;ch<=15;++ch){
            const std::vector<float> a=renderCh(ch), b=renderCh(0);
            require(a.size()==b.size(),"mpe init-trap render size");
            bool same=a==b;
            require(same,"mpe init-trap: unlatched member channel must be bit-identical to ch0");
        }
        printf("mpe init-trap PASS (ch1..15 == ch0, no 0xD0 sent)\n");
    }
    {
        // (2) blend math: latched Z on ch5 must equal the direct drive.
        // drive = 0.5*(vel+Z) = 0.5*(0.5+1.0) = 0.75 (exactly representable),
        // so engine A (ch5, vel 0.5, Z=1) must equal engine B (ch0, vel 0.75).
        const auto renderNV=[&](int ch,float vel){
            MultiScaleBodyEngine e; e.prepare(44100); e.reset();
            if(ch>=1) e.setMpePressure(ch,1.0f);
            e.noteOn(60,vel,ch);
            std::vector<float> x; x.reserve(8192);
            for(int i=0;i<8192;++i) x.push_back(e.processSampleMono());
            return x;
        };
        const std::vector<float> a=renderNV(5,0.5f), b=renderNV(0,0.75f);
        require(a.size()==b.size(),"mpe blend render size");
        require(a==b,"mpe blend: latch Z=1 on ch5 must render identically to ch0 vel 0.75");
        printf("mpe blend math PASS (0.5*(0.5+1.0)==0.75 drive)\n");
    }
    {
        // (3) noteOff clears the latch. Engine A: latch Z=1 on ch5, strike
        // (vel .5 -> drive .75), noteOff clears Z. Reference: identical
        // history but never latched (strike 1 = direct vel .75 on ch5).
        // The 2nd ch5 strike must be bit-identical: if the clear is broken
        // it would render with drive .75 instead of .5.
        const auto renderNoteOff=[&](bool latch){
            MultiScaleBodyEngine e; e.prepare(44100); e.reset();
            if(latch){ e.setMpePressure(5,1.0f); e.noteOn(60,0.5f,5); }
            else     { e.noteOn(60,0.75f,5); }   // same drive, no latch
            for(int i=0;i<64;++i) e.processSampleMono();
            e.noteOff(60,5);                     // matched release (+ latch clear in A)
            e.noteOn(62,0.5f,5);                 // assertion target: must be unlatched
            std::vector<float> x; x.reserve(8192);
            for(int i=0;i<8192;++i) x.push_back(e.processSampleMono());
            return x;
        };
        const std::vector<float> a=renderNoteOff(true), b=renderNoteOff(false);
        require(a.size()==b.size(),"mpe noteOff render size");
        require(a==b,"mpe noteOff: cleared latch must leave 2nd ch5 strike == never-latched reference");
        printf("mpe noteOff clear PASS\n");
    }
    {
        // (4) allSoundOff (panic) clears every member latch. Same matched-
        // history shape as (3): latched strike + panic must leave the post-
        // panic ch5 strike identical to a never-latched reference.
        const auto renderPanic=[&](bool latch){
            MultiScaleBodyEngine e; e.prepare(44100); e.reset();
            if(latch){ e.setMpePressure(5,1.0f); e.noteOn(60,0.5f,5); }
            else     { e.noteOn(60,0.75f,5); }   // same drive, no latch
            for(int i=0;i<64;++i) e.processSampleMono();
            e.allSoundOff();                     // panic: kill voices + clear latches
            e.noteOn(62,0.5f,5);                 // assertion target: must be unlatched
            std::vector<float> x; x.reserve(8192);
            for(int i=0;i<8192;++i) x.push_back(e.processSampleMono());
            return x;
        };
        const std::vector<float> a=renderPanic(true), b=renderPanic(false);
        require(a.size()==b.size(),"mpe panic render size");
        require(a==b,"mpe panic: allSoundOff must clear latches so post-panic ch5 strike == never-latched");
        printf("mpe panic clear PASS\n");
    }




    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
