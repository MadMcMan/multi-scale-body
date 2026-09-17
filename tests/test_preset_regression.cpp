#include "PluginMultiScaleBody.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
static void require(bool c,const char* m){ if(!c){ fprintf(stderr,"FAIL: %s\n",m); std::exit(1);} }
// expose protected state plumbing for the wave-2 state tests
struct TestPlug : DISTRHO::PluginMultiScaleBody {
    using DISTRHO::PluginMultiScaleBody::setState;
    using DISTRHO::PluginMultiScaleBody::getState;
};
int main(){
    using namespace DISTRHO;
    PluginMultiScaleBody plug;
    require(plug.testGetParameterCount()==PluginMultiScaleBody::kParameterCount,"param count");
    // wave-2 defaults are identity: bow/damper/inharm off, band decays 0.5 (-> 1.0)
    require(std::fabs(plug.testGetParameterValue(PluginMultiScaleBody::kParamBow))<1e-6f,"bow default 0");
    require(std::fabs(plug.testGetParameterValue(PluginMultiScaleBody::kParamDamper))<1e-6f,"damper default 0");
    require(std::fabs(plug.testGetParameterValue(PluginMultiScaleBody::kParamInharm))<1e-6f,"inharm default 0");
    require(std::fabs(plug.testGetParameterValue(PluginMultiScaleBody::kParamSlideMode))<1e-6f,"slide default 0 (classic)");
    require(plug.testEngine().getBandDecayTrim(0)==1.f,"bandDecay default trim 1.0");
    // wave-3 defaults are identity: physical model off, eco off, budget mid
    require(std::fabs(plug.testGetParameterValue(PluginMultiScaleBody::kParamSupport))<1e-6f,"support default 0");
    require(std::fabs(plug.testGetParameterValue(PluginMultiScaleBody::kParamHoldDamp))<1e-6f,"holddamp default 0");
    require(std::fabs(plug.testGetParameterValue(PluginMultiScaleBody::kParamResMorph))<1e-6f,"resmorph default 0");
    require(std::fabs(plug.testGetParameterValue(PluginMultiScaleBody::kParamMorphTarget))<1e-6f,"morphtarget default 0");
    require(std::fabs(plug.testGetParameterValue(PluginMultiScaleBody::kParamMorphAmt))<1e-6f,"morphamt default 0");
    require(std::fabs(plug.testGetParameterValue(PluginMultiScaleBody::kParamMaterial))<1e-6f,"material default 0");
    require(std::fabs(plug.testGetParameterValue(PluginMultiScaleBody::kParamRayleighA))<1e-6f,"rayleighA default 0");
    require(std::fabs(plug.testGetParameterValue(PluginMultiScaleBody::kParamRayleighB))<1e-6f,"rayleighB default 0");
    require(std::fabs(plug.testGetParameterValue(PluginMultiScaleBody::kParamEcoMode))<1e-6f,"ecomode default 0");
    require(std::fabs(plug.testGetParameterValue(PluginMultiScaleBody::kParamEcoBudget)-0.5f)<1e-6f,"ecobudget default 0.5");
    plug.testSetParameterValue(PluginMultiScaleBody::kParamPreset, 0.0f);
    float v=plug.testGetParameterValue(PluginMultiScaleBody::kParamPreset);
    require(std::abs(v-0.0f)<0.01f,"preset round-trip 0.0");
    plug.testSetParameterValue(PluginMultiScaleBody::kParamPreset, 1.0f);
    v=plug.testGetParameterValue(PluginMultiScaleBody::kParamPreset);
    require(std::abs(v-1.0f)<0.01f,"preset round-trip 1.0");
    plug.testSampleRate2(44100);
    plug.testActivate();
    float bufL[512]={}, bufR[512]={};
    float* out[2]={bufL,bufR};
    MidiEvent ev{}; ev.frame=0; ev.size=3; ev.data[0]=0x90; ev.data[1]=60; ev.data[2]=100;
    plug.testRun2(nullptr,out,512,&ev,1);
    float sum=0; for(int i=0;i<512;++i) sum+=std::abs(bufL[i]);
    require(sum>0.01f,"non-silent after noteOn");
    printf("non-silent sum %.3f\n",sum);
    for(int b=0;b<20;++b){ plug.testRun2(nullptr,out,512,nullptr,0); }
    // --- CC123 all-notes-off: every sounding voice must enter Release ---
    {
        MidiEvent cc123{}; cc123.frame=0; cc123.size=3; cc123.data[0]=0xB0; cc123.data[1]=123; cc123.data[2]=0;
        plug.testRun2(nullptr,out,512,&cc123,1);
        bool anyRelease=false;
        for(int i=0;i<modal::kVoiceCount;++i)
            if(plug.testEngine().voice(i).envState==modal::Voice::Release) anyRelease=true;
        require(anyRelease,"CC123 all-notes-off releases voices");
    }
    // --- CC120 all-sound-off: immediate digital silence ---
    {
        MidiEvent cc120{}; cc120.frame=0; cc120.size=3; cc120.data[0]=0xB0; cc120.data[1]=120; cc120.data[2]=0;
        plug.testRun2(nullptr,out,512,&cc120,1);
        float resid=0;
        for(int b=0;b<8;++b){
            for(int i=0;i<512;++i){ bufL[i]=0.f; bufR[i]=0.f; }
            plug.testRun2(nullptr,out,512,nullptr,0);
            for(int i=0;i<512;++i) resid+=std::abs(bufL[i])+std::abs(bufR[i]);
        }
        require(resid<1e-4f,"CC120 all-sound-off silences output");
        printf("cc120 residual %.2e\n",resid);
    }
    // --- idea 15: MIDI learn binds a CC, learned CC drives the param ----------
    {
        TestPlug p;
        p.testSampleRate2(44100); p.testActivate();
        p.setState("learn","1");   // arm: next ch0 CC binds to Decay
        MidiEvent cc{}; cc.frame=0; cc.size=3; cc.data[0]=0xB0; cc.data[1]=74; cc.data[2]=100;
        p.testRun2(nullptr,out,512,&cc,1);   // consumed by the binding
        std::string ccmap=p.getState("ccmap").buffer();
        require(ccmap.find("1=74")!=std::string::npos,"learn: CC74 bound to Decay in ccmap");
        const float before=p.testGetParameterValue(PluginMultiScaleBody::kParamDecay);
        p.testRun2(nullptr,out,512,&cc,1);   // now a normal learned write
        const float after=p.testGetParameterValue(PluginMultiScaleBody::kParamDecay);
        require(std::fabs(after-before)>0.01f,"learn: learned CC drives the param");
        require(after>0.75f,"learn: CC74=100/127 lands Decay near 0.79");
        printf("midi learn PASS (ccmap %s)\n",ccmap.c_str());
    }
    {
        // persistence: a saved ccmap parses and drives on load
        TestPlug q;
        q.testSampleRate2(44100); q.testActivate();
        q.setState("ccmap","2=74;");   // Brightness (=2, not 3: StrikeX occupies 3)
        MidiEvent cc{}; cc.frame=0; cc.size=3; cc.data[0]=0xB0; cc.data[1]=74; cc.data[2]=63;
        q.testRun2(nullptr,out,512,&cc,1);
        require(std::fabs(q.testGetParameterValue(PluginMultiScaleBody::kParamBrightness)-63.f/127.f)<1e-4f,
                "ccmap load: CC74 drives Brightness");
        printf("ccmap persistence PASS\n");
    }
    // --- idea 13: .scl text parses into a working note table ---------------
    {
        TestPlug p;
        p.testSampleRate2(44100); p.testActivate();
        // 7-EDO: first degree 171.4c (implicit tonic -> note 60 = 1.0),
        // last line 1200c = the octave boundary.
        const char* scl7="seven-edo\n7\n171.428571\n342.857143\n514.285714\n685.714286\n857.142857\n1028.571429\n1200.0\n";
        p.setState("scale",scl7);
        MidiEvent on{}; on.frame=0; on.size=3; on.data[0]=0x90; on.data[1]=67; on.data[2]=100;
        p.testRun2(nullptr,out,512,&on,1);
        // 7-EDO note 67 == octave above C4: mode-0 freq doubles vs 12-EDO C4
        TestPlug q;
        q.testSampleRate2(44100); q.testActivate();
        MidiEvent on2{}; on2.frame=0; on2.size=3; on2.data[0]=0x90; on2.data[1]=72; on2.data[2]=100;
        q.testRun2(nullptr,out,512,&on2,1);
        const double f67=std::acos(std::clamp((double)p.testEngine().voice(0).cosTheta[0],-1.0,1.0))*44100.0;
        const double f72=std::acos(std::clamp((double)q.testEngine().voice(0).cosTheta[0],-1.0,1.0))*44100.0;
        require(std::fabs(f67-f72)<1.0,"scl: 7-edo note 67 == octave (degree 0 + 1 octave)");
        // clearing the scale restores 12-EDO math
        p.setState("scale","");
        p.testRun2(nullptr,out,512,&on2,1);
        const double f67c=std::acos(std::clamp((double)p.testEngine().voice(0).cosTheta[0],-1.0,1.0))*44100.0;
        require(std::fabs(f67c-f72)<1.0,"scl clear: back to equal temperament");
        printf("scl parse + clear PASS\n");
    }
    // --- 12-TET .scl reproduces the classic table exactly -------------------
    {
        TestPlug p;
        p.testSampleRate2(44100); p.testActivate();
        char scl[256]="12-tet\n12\n100.0\n200.0\n300.0\n400.0\n500.0\n600.0\n700.0\n800.0\n900.0\n1000.0\n1100.0\n1200.0\n";
        p.setState("scale",scl);
        MidiEvent on{}; on.frame=0; on.size=3; on.data[0]=0x90; on.data[1]=60; on.data[2]=100;
        p.testRun2(nullptr,out,512,&on,1);
        TestPlug q2;
        q2.testSampleRate2(44100); q2.testActivate();
        q2.testRun2(nullptr,out,512,&on,1);
        require(p.testEngine().voice(0).cosTheta[0]==q2.testEngine().voice(0).cosTheta[0],
                "12-TET scl == classic path (bit-identical mode-0 coeff)");
        printf("12-TET scl identity PASS\n");
    }
    // --- wave-5: new param defaults are identity -------------------------
    {
        PluginMultiScaleBody p;
        require(std::fabs(p.testGetParameterValue(PluginMultiScaleBody::kParamSupX)-0.5f)<1e-6f,"supx default 0.5");
        require(std::fabs(p.testGetParameterValue(PluginMultiScaleBody::kParamSupY)-0.5f)<1e-6f,"supy default 0.5");
        require(std::fabs(p.testGetParameterValue(PluginMultiScaleBody::kParamStrikeW)-0.5f)<1e-6f,"strikew default 0.5");
        require(std::fabs(p.testGetParameterValue(PluginMultiScaleBody::kParamScrape))<1e-6f,"scrape default 0");
        require(std::fabs(p.testEngine().getSupX()-0.5f)<1e-6f,"engine supx default");
        require(std::fabs(p.testEngine().getStrikeW()-0.5f)<1e-6f,"engine strikew default");
        require(std::fabs(p.testEngine().getScrape())<1e-6f,"engine scrape default");
        printf("wave-5 defaults PASS\n");
    }
    // --- wave-5: material select snaps the Rayleigh knobs ----------------
    // DEFAULT leaves knobs alone; STEEL (index 2) snaps to (0.55, 0.13) and
    // the engine follows on the same dispatch.
    {
        PluginMultiScaleBody p;
        p.testSampleRate2(44100); p.testActivate();
        p.testSetParameterValue(PluginMultiScaleBody::kParamRayleighA, 0.7f);
        p.testSetParameterValue(PluginMultiScaleBody::kParamRayleighB, 0.6f);
        p.testSetParameterValue(PluginMultiScaleBody::kParamMaterial, 0.f); // DEFAULT
        require(std::fabs(p.testGetParameterValue(PluginMultiScaleBody::kParamRayleighA)-0.7f)<1e-6f,
                "material DEFAULT leaves Rayl A alone");
        require(std::fabs(p.testGetParameterValue(PluginMultiScaleBody::kParamRayleighB)-0.6f)<1e-6f,
                "material DEFAULT leaves Rayl B alone");
        p.testSetParameterValue(PluginMultiScaleBody::kParamMaterial, 0.2f); // STEEL
        const float eA=modal::MultiScaleBodyEngine::kMatRay[2][0];
        const float eB=modal::MultiScaleBodyEngine::kMatRay[2][1];
        require(std::fabs(p.testGetParameterValue(PluginMultiScaleBody::kParamRayleighA)-eA)<1e-6f,
                "material STEEL snaps Rayl A");
        require(std::fabs(p.testGetParameterValue(PluginMultiScaleBody::kParamRayleighB)-eB)<1e-6f,
                "material STEEL snaps Rayl B");
        require(std::fabs(p.testEngine().getRayleighA()-eA)<1e-6f,"engine follows Rayl A snap");
        require(std::fabs(p.testEngine().getRayleighB()-eB)<1e-6f,"engine follows Rayl B snap");
        printf("material rayleigh snap PASS (A=%.2f B=%.2f)\n",eA,eB);
    }
    // --- routing guard: every input param stores distinctly ----------------
    // Regression fence for "changing a knob broke the routing": if a knob is
    // ever wired to the wrong param, or two knobs share an index, this fails.
    // For every input param, write a unique probe and require it round-trips
    // exactly; near-equal neighbours (0 vs 1e-9) are exercised so a knob that
    // routes to a *nearby* param (not a distinct one) is caught too.
    {
        PluginMultiScaleBody p;
        p.testSampleRate2(44100); p.testActivate();
        const uint32_t ni=PluginMultiScaleBody::kNumInputParams;
        for(uint32_t i=0;i<PluginMultiScaleBody::kParameterCount;++i){
            if(!PluginMultiScaleBody::isInputParameter(i)) continue;
            // enum-snapped params (Preset/MorphTarget/Material/SlideMode/Eco/
            // Mono) round-trip to the nearest STEP by design — handled below.
            if(i==PluginMultiScaleBody::kParamPreset
               || i==PluginMultiScaleBody::kParamMorphTarget
               || i==PluginMultiScaleBody::kParamMaterial
               || i==PluginMultiScaleBody::kParamSlideMode
               || i==PluginMultiScaleBody::kParamEcoMode
               || i==PluginMultiScaleBody::kParamModelMode
               || i==PluginMultiScaleBody::kParamMono) continue;
            const float probe=0.01f*((float)(i%97)+1.f);   // unique, non-default
            p.testSetParameterValue(i, probe);
            float got=p.testGetParameterValue(i);
            if(std::fabs(got-probe)>1e-6f){
                printf("FAIL: input %u probe %.5f stored %.5f\n",i,probe,got);
                require(false,"routing: param stores its own probe");
            }
        }
        // snapped params must be idempotent: y=snap(x) then set(y) reads y.
        const uint32_t snapped[6]={PluginMultiScaleBody::kParamPreset,
            PluginMultiScaleBody::kParamMorphTarget,PluginMultiScaleBody::kParamMaterial,
            PluginMultiScaleBody::kParamSlideMode,PluginMultiScaleBody::kParamEcoMode,
            PluginMultiScaleBody::kParamMono};
        for(uint32_t pi : snapped){
            p.testSetParameterValue(pi,0.12345f);
            const float y=p.testGetParameterValue(pi);
            p.testSetParameterValue(pi,y);
            require(std::fabs(p.testGetParameterValue(pi)-y)<1e-6f,
                    "routing: snapped param is idempotent");
        }
        printf("routing: all %u input params route + discrete snap PASS\n",ni);
    }
    // --- routing guard: output params are write-protected -------------------
    // kParamOutLevel..kParamOutBand15 are DSP metering only; a knob wired to
    // one (a routing mistake) must be a silent, state-free no-op — it would
    // otherwise read as "moved a knob, nothing changes" on that output.
    {
        PluginMultiScaleBody p;
        p.testSampleRate2(44100); p.testActivate();
        int nsink=0;
        for(uint32_t i=PluginMultiScaleBody::kParamOutLevel;i<=PluginMultiScaleBody::kParamOutBand15;++i){
            const float probe=0.31415f;
            p.testSetParameterValue(i, probe);
            const float got=p.testGetParameterValue(i);
            if(i==PluginMultiScaleBody::kParamOutLevel){ if(std::fabs(got-0.f)<1e-6f) ++nsink; }
            else if(i>=PluginMultiScaleBody::kParamOutBand0 && got==0.f) ++nsink;
        }
        require(nsink==17,"routing: all output writes rejected");
        require(p.testGetParameterValue(PluginMultiScaleBody::kParamVolume)==1.f,
                "output write left an input untouched");
        printf("routing: output params write-protected PASS\n");
    }
    // Appended input must survive patch/MIDI routes across the output block.
    {
        using P=PluginMultiScaleBody;
        auto render=[](int route){
            TestPlug p;
            p.testSampleRate2(44100);
            p.testSetParameterValue(P::kParamWet,0.f);
            if(route==0) p.testSetParameterValue(P::kParamContactNoise,0.f);
            if(route==1){
                TestPlug saved;
                saved.testSetParameterValue(P::kParamWet,0.f);
                saved.testSetParameterValue(P::kParamContactNoise,0.f);
                std::string patch=saved.getState("patch").buffer();
                p.setState("patch",patch.c_str());
            }
            if(route==2){
                TestPlug saved;
                const auto index=std::to_string(P::kParamContactNoise);
                saved.setState("learn",index.c_str());
                float l[64]{},r[64]{}; float* dst[]={l,r};
                MidiEvent cc{}; cc.size=3; cc.data[0]=0xB0; cc.data[1]=74; cc.data[2]=0;
                saved.testRun2(nullptr,dst,64,&cc,1);
                std::string map=saved.getState("ccmap").buffer();
                p.setState("ccmap",map.c_str());
                p.testRun2(nullptr,dst,64,&cc,1);
            }
            else{
                float l[64]{},r[64]{}; float* dst[]={l,r};
                p.testRun2(nullptr,dst,64);
            }
            p.testActivate();
            float l[64]{},r[64]{}; float* dst[]={l,r};
            for(int i=0;i<128;++i) p.testRun2(nullptr,dst,64);
            std::vector<float> result;
            MidiEvent on{}; on.size=3; on.data[0]=0x90; on.data[1]=60; on.data[2]=20;
            for(int block=0;block<64;++block){
                p.testRun2(nullptr,dst,64,block?nullptr:&on,block?0:1);
                result.insert(result.end(),l,l+64);
                result.insert(result.end(),r,r+64);
            }
            return result;
        };
        const auto direct=render(0),patch=render(1),midi=render(2),normal=render(3);
        require(direct==patch,"Contact patch recall preserves rendered sound");
        require(direct==midi,"Contact learned CC recall preserves rendered sound");
        double delta=0; for(size_t i=0;i<direct.size();++i) delta+=std::fabs(direct[i]-normal[i]);
        require(delta>1e-4,"Contact mute removes audible transient");
        printf("Contact patch/MIDI audio parity PASS (delta %.6f)\n",delta);
    }
    // --- routing guard: MIDI-learn 'learn' state round-trips the index ------
    // Regression for a real defect: getState("learn") returned "1" (a bool),
    // so a host that persisted/reloaded state while MIDI-learn was armed
    // restored learnPending_=1 (Decay) and the next CC bound the WRONG knob.
    {
        TestPlug p;
        p.testSampleRate2(44100); p.testActivate();
        p.setState("learn","17");
        const char* s=p.getState("learn");
        require(s && std::string(s)=="17", "learn state round-trips the index, not '1'");
        p.setState("learn","999");   // out of range
        require(std::string(p.getState("learn")).empty(),"learn out-of-range is dropped");
        p.setState("learn","");
        require(std::string(p.getState("learn")).empty(),"learn cleared is empty");
        printf("routing: learn state index round-trip PASS\n");
    }
    // --- elastic FEM mode: dispatch, state recall, legacy-patch default ------
    {
        PluginMultiScaleBody p;
        require(std::fabs(p.testGetParameterValue(PluginMultiScaleBody::kParamModelMode))<1e-6f,
                "modelMode default 0 (Classic)");
        require(p.testEngine().getModelMode()==0,"engine modelMode default 0");
        printf("elastic: modelMode default 0 PASS\n");
    }
    {
        // plugin dispatch: setting kParamModelMode=1 selects Elastic output
        // (distinct from Classic), =0 restores Classic byte-exact on a
        // fresh-engine basis; patch recall carries the mode; a legacy patch
        // (no model field) always restores Classic so old sounds are unchanged.
        auto render=[&](float m){
            PluginMultiScaleBody p; p.testSampleRate2(48000); p.testActivate();
            p.testSetParameterValue(PluginMultiScaleBody::kParamPreset, 6.f/17.f);
            p.testSetParameterValue(PluginMultiScaleBody::kParamContactNoise, 0.f);
            if(m>0.f) p.testSetParameterValue(PluginMultiScaleBody::kParamModelMode, 1.f);
            float L[256],R[256]; float* out[]={L,R};
            MidiEvent on{}; on.size=3; on.data[0]=0x90; on.data[1]=48; on.data[2]=100;
            std::vector<float> x; double en=0; float pk=0;
            for(int b=0;b<94;++b){
                p.testRun2(nullptr,out,256,b?nullptr:&on,b?0:1);
                for(int i=0;i<256;++i){ x.push_back(L[i]); x.push_back(R[i]);
                    if(!std::isfinite(L[i])||!std::isfinite(R[i])) std::exit(2);
                    en+=double(L[i])*L[i]+double(R[i])*R[i]; pk=std::max(pk,std::max(std::fabs(L[i]),std::fabs(R[i]))); }
            }
            require(en>1e-10&&pk<=0.951f,"elastic plugin render finite + bounded");
            return x;
        };
        const std::vector<float> c=render(0.f), e=render(1.f), c2=render(0.f);
        require(c!=e,"elastic: plugin Elastic output distinct from Classic");
        require(c==c2,"elastic: plugin Classic byte-exact across dispatch");
        printf("elastic dispatch PASS (classic==classic byte-exact, elastic distinct)\n");
    }
    {
        // patch recall: a saved patch with model=1 restores Elastic; a legacy
        // patch (no model field) forces Classic so old sounds are untouched.
        TestPlug p; p.testSampleRate2(44100); p.testActivate();
        p.testSetParameterValue(PluginMultiScaleBody::kParamModelMode, 1.f);
        require(p.testEngine().getModelMode()==1,"elastic engine follows param");
        std::string patch=p.getState("patch").buffer();
        TestPlug q; q.testSampleRate2(44100); q.testActivate();
        q.setState("patch",patch.c_str());
        require(q.testEngine().getModelMode()==1,"elastic patch recall keeps mode 1");
        TestPlug r; r.testSampleRate2(44100); r.testActivate();
        r.setState("patch","0=0;");   // legacy patch: no model field
        require(r.testEngine().getModelMode()==0,"elastic legacy patch forces Classic");
        printf("elastic state recall + legacy default PASS\n");
    }
    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
