#ifndef MSB_UI_CONTROL_CONTRACT_HPP
#define MSB_UI_CONTROL_CONTRACT_HPP
// ============================================================================
//  tests/UiControlContract.hpp — the Gate C numbers, declared once.
//
//  Both gate tests read this header so a parameter added to the plugin cannot
//  silently land with no GUI control:
//
//    * test_multiscalebody_dsp  checks the PLUGIN side: kParameterCount, the
//      metering-output block boundaries, and that every non-metering index is a
//      real input. It links only the engine, so it uses the static constexpr
//      accessors and never instantiates the plugin class.
//
//    * test_multiscalebody_ui   checks the UI side: it walks the real LVGL tree
//      and asserts the control census matches these counts exactly.
//
//  If the two ever disagree the plugin has either an orphaned control or an
//  unreachable parameter, and BOTH tests fail.
// ============================================================================
#include <cstdint>

namespace msb {

// The metering block appended to the parameter list: kParamOutLevel plus the
// 16 per-band outputs. These are DSP -> UI only: the host reads them, the UI
// renders them, and no control may be bound to one.
static constexpr uint32_t kMeteringParams = 1 + 16;

// Control families, counted the way the UI actually binds them.
static constexpr uint32_t kArcKnobs      = 33;  // machined lv_arc, user_data = param
static constexpr uint32_t kTaggedSelects = 5;   // dropdown / toggle, tagLearnable
static constexpr uint32_t kBandSurface   = 32;  // spectrum scrub: 16 gain + 16 decay
static constexpr uint32_t kXyPad         = 2;   // strike disc: X and Y
static constexpr uint32_t kSwitches      = 2;   // ARP on/off, SCRUB gain/decay

static constexpr uint32_t kTotalControls =
    kArcKnobs + kTaggedSelects + kBandSurface + kXyPad + kSwitches;

} // namespace msb
#endif