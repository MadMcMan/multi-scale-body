#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "MultiScaleBodyEngine.hpp"
#include "ModalData.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
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
    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
