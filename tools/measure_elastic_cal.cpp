// Measure the per-preset Elastic-vs-Classic RMS ratio so the elastic gain
// calibration (GAIN_CAL in tools/elastic_bake.py) equalizes loudness after the
// 0.18 frequency-scale audit fix. The engine path is exactly linear in gain
// (strike injection = force*gain[i]*strikeEff[i]; the resonator state is linear
// in force), so ONE multiplicative constant per preset equalizes RMS at any
// strike point / mode count / decay. Settings mirror the original calibration:
// modeCount=1, strike .37/.63, decay .5, contact OFF, 44.1k, 24000 frames.
//
// Build: g++ -std=c++17 -O1 -I plugins/MultiScaleBody/src \
//          tools/measure_elastic_cal.cpp \
//          plugins/MultiScaleBody/src/MultiScaleBodyEngine.cpp -o build/measure_elastic_cal.exe
#include "MultiScaleBodyEngine.hpp"
#include <cmath>
#include <cstdio>
using namespace modal;

static double rmsFor(int preset, int modelMode){
    MultiScaleBodyEngine e;
    e.prepare(44100);
    e.setModelMode(modelMode);
    e.setPreset(preset);
    e.setModeCount(1.0f);
    e.setPitchScale(0.5f);
    e.setDecayScale(0.5f);
    e.setContactNoise(0.f);
    e.setReverbWet(0.f);
    e.setExciteMix(0.f);
    e.setStrike(0.37f,0.63f);
    e.reset();
    e.noteOn(60,1.0f,0);
    double sum=0.0;
    const int N=24000;
    for(int i=0;i<N;++i){ const float s=e.processSampleMono(); sum+=(double)s*s; }
    return std::sqrt(sum/(double)N);
}

int main(){
    for(int p=0;p<kNumPresets;++p){
        const double rc=rmsFor(p,0);
        const double re=rmsFor(p,1);
        const double cal=(re>1e-12)?(rc/re):0.0;
        std::printf("%2d %-12s classic=%.6g elastic=%.6g cal=%.4f\n",
                    p, kPresets[p].name, rc, re, cal);
    }
    return 0;
}
