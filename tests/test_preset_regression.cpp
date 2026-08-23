#include "PluginMultiScaleBody.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
static void require(bool c,const char* m){ if(!c){ fprintf(stderr,"FAIL: %s\n",m); std::exit(1);} }
int main(){
    using namespace DISTRHO;
    PluginMultiScaleBody plug;
    require(plug.testGetParameterCount()==PluginMultiScaleBody::kParameterCount,"param count");
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
    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
