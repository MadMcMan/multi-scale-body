#include "PluginMultiScaleBody.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
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
    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
