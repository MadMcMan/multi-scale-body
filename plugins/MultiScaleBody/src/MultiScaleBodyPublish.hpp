#pragma once
// ============================================================================
// The SHIPPING block -> UI publisher for MultiScaleBody.
//
// It lives here, header-inline and named, instead of as the anonymous tail of
// PluginMultiScaleBody::run(), for one reason: the render harness calls THIS
// function. The two lines it replaces
//
//     vizLevel_ = std::clamp(blockPeak*1.4f, 0.f, 1.f);
//     engine_.analyseBands(vizBins_);
//
// were unreachable from tests/test_multiscalebody_ui.cpp, which drives the real
// editor through DPF's UIExporter but never constructs the plugin. So
// MultiScaleBodyUI::fGotLiveViz stayed false for the whole run, every data panel
// in uiIdle() took the `else` branch and drew the baked preset preview, and
// resonanceDrawCb() returned at its first guard. A run() that stopped calling
// analyseBands() entirely - or a UI that inverted the live test - would have
// left that suite green.
//
// Audio-thread contract, unchanged from the block this replaces: one clamp,
// one pass over the active voices' modal state, no allocation, no locks.
// ============================================================================

#include "MultiScaleBodyEngine.hpp"
#include <algorithm>
#include <cstdint>

namespace modal {

// The one and only block -> UI publication path.
//
// blockPeak is the caller's per-block peak, exactly as run() computed it.
// levelOut and binsOut receive the values run() stored in its members; in the
// harness they are the locals the test then hands to the editor through the
// same parameterChanged() transport DPF's host poll uses.
inline void publishViz(MultiScaleBodyEngine& engine, float blockPeak,
                       float& levelOut, float* binsOut /*[16]*/) {
    levelOut = std::clamp(blockPeak * 1.4f, 0.f, 1.f);
    engine.analyseBands(binsOut);
}

}  // namespace modal