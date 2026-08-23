#pragma once
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cmath>
namespace modal {
class OutputLP {
public:
    void prepare(double sr) {
        if(sr < 1000) sr = 44100;
        sampleRate_ = sr;
        // ~20 ms exponential glide on the coefficient itself: automation of
        // cutoff (Brightness / LFO) can never zipper or click
        smoothCoef_ = 1.0 - std::exp(-1.0 / (0.02 * sampleRate_));
        setCutoffHz(cutoffHz_);
        reset();
    }
    void setCutoffHz(double hz) {
        cutoffHz_ = hz;
        if(sampleRate_ < 1000) return;
        double x = 2.0 * M_PI * hz / sampleRate_;
        double target = std::exp(-x);
        if (target < 0) target=0;
        if (target > 0.999999) target=0.999999;
        alphaTarget_ = target;
        if (!smoothInit_) { alpha_ = target; smoothInit_ = true; }
    }
    void reset() { y_ = 0.0f; }
    inline float process(float x) {
        alpha_ += (alphaTarget_ - alpha_) * smoothCoef_;
        y_ = (1.0f - (float)alpha_) * x + (float)alpha_ * y_;
        return y_;
    }
private:
    double sampleRate_ = 44100.0;
    double cutoffHz_ = 18000.0;
    double alphaTarget_ = 0.5;
    double alpha_ = 0.5;
    double smoothCoef_ = 0.00113; // re-derived in prepare()
    bool smoothInit_ = false;
    float y_ = 0.0f;
};
}
