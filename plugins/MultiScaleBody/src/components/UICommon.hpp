#ifndef UI_COMMON_HPP
#define UI_COMMON_HPP
// ============================================================================
//  components/UICommon.hpp  —  the abstract UI interface (component contract)
// ----------------------------------------------------------------------------
//  PROVENANCE: forked from cymbals-ui (include/cymbals-ui/UICommon.hpp) and
//  adapted. First-party: no cymbals-ui dependency.
//
//  This is ONLY the contract the components bind to. Every design token
//  (colors, layout, scale, typography) lives in styles/Palette.hpp — keep the
//  two concerns separate. See UIWidgets.hpp header and AGENTS.md ("Components")
//  for the ownership rule.
// ============================================================================
#include "DistrhoPlugin.hpp"
#include <cstdint>
#include <string>
START_NAMESPACE_DISTRHO
// Minimal UI interface consumed by UIWidgets' arc callbacks
class AbstractMultiScaleBodyUI {
public:
    virtual ~AbstractMultiScaleBodyUI() {}
    virtual float getParamValue(uint32_t index) const = 0;
    virtual void setParamValue(uint32_t index, float value) = 0;
    virtual void editParameter(uint32_t index, bool start) = 0;
    virtual void syncParamWidget(uint32_t index, float value) = 0;
    virtual std::string parameterName(uint32_t index) const = 0;
};
END_NAMESPACE_DISTRHO
#endif
