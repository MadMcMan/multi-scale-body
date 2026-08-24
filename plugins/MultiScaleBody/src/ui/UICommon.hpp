#ifndef UI_COMMON_HPP
#define UI_COMMON_HPP
#include "DistrhoPlugin.hpp"
#include "lvgl.h"
#include "DistrhoPluginInfo.h"
#include <string>
START_NAMESPACE_DISTRHO
// Minimal interface for MultiScaleBody — matches cymbals AbstractCymbalsUI subset we actually use
class AbstractMultiScaleBodyUI {
public:
    virtual ~AbstractMultiScaleBodyUI() {}
    virtual float getParamValue(uint32_t index) const = 0;
    virtual void setParamValue(uint32_t index, float value) = 0;
    virtual void editParameter(uint32_t index, bool start) = 0;
    virtual void syncParamWidget(uint32_t index, float value) = 0;
    virtual std::string parameterName(uint32_t index) const = 0;
};
// Alias for compatibility with full cymbals headers if they reference AbstractCymbalsUI
using AbstractCymbalsUI = AbstractMultiScaleBodyUI;

// === Cymbals palette — exact copy === //
#define COL_BG         lv_color_hex(0x0D0C0A)
#define COL_PANEL      lv_color_hex(0x1D2026)
#define COL_ACCENT     lv_color_hex(0x2B3039)
#define COL_HIGHLIGHT  lv_color_hex(0xFFB020)
#define COL_TEXT       lv_color_hex(0xE6E0D6)
#define COL_TEXT_DIM   lv_color_hex(0x99958B)
#define COL_TITLE      lv_color_hex(0xD3C8AE)
#define COL_HAIRLINE   lv_color_hex(0x353B45)
#define COL_KNOB_LABEL lv_color_hex(0x8D877A)
#define COL_SLIDER     COL_HIGHLIGHT
#define COL_KNOB       lv_color_hex(0xF2EDE4)
#define COL_KNOB_LIGHT lv_color_hex(0xE0DAD0)
#define COL_KNOB_BORDER lv_color_hex(0x383F4A)
#define COL_WHITE      lv_color_hex(0xFFFFFF)
#define COL_BLACK      lv_color_hex(0x000000)
#define COL_CHART      COL_HIGHLIGHT
#define COL_CHART_BG   COL_PANEL
#define COL_KNOB_RING_BG    lv_color_hex(0x232830)
#define COL_KNOB_CAP        lv_color_hex(0x2A2E35)
#define COL_KNOB_INDICATOR  COL_HIGHLIGHT
#define COL_PANEL_DARK lv_color_hex(0x131518)
#define COL_SHADOW     lv_color_hex(0x000000)
#define COL_BORDER     lv_color_hex(0x383F4A)
#define COL_CHECKED_BG lv_color_hex(0xC88A1E)
#define COL_OFF        lv_color_hex(0x32373F)
#define COL_MOD        lv_color_hex(0x5DDDD0)
#define COL_MOD_DIM    lv_color_hex(0x1E4A45)

// === STRIKE PLATE palette (MultiScaleBody chassis) === //
#define PLATE_BG          lv_color_hex(0x0A0908)  // true-black warm chassis
#define PLATE_PANEL       lv_color_hex(0x141210)  // machined plate surface
#define PLATE_LINE        lv_color_hex(0x28241C)  // hairline rules, dividers, rings
#define PLATE_EDGE        lv_color_hex(0x342E24)  // exposed edges, strong borders
#define PLATE_WELL        lv_color_hex(0x0D0B09)  // inset wells (charts, disc, selects)
#define PLATE_PREVIEW_BG  lv_color_hex(0x0B0A08)  // body-preview box interior
#define PLATE_WELL_HI     lv_color_hex(0x181410)  // well gradient bottom (depth)
#define PLATE_EMPTY       lv_color_hex(0x0A0E14)  // unoccupied preview cells
#define PLATE_TITLE       lv_color_hex(0xF2EDE4)  // title white
#define PLATE_TEXT        lv_color_hex(0xC9C1B2)  // primary spec text
#define PLATE_TEXT_MID    lv_color_hex(0x8A8272)  // secondary text
#define PLATE_TEXT_DIM    lv_color_hex(0x655F51)  // tertiary/label text
#define PLATE_AMBER       lv_color_hex(0xFFCE6B)  // bright amber readout
#define PLATE_AMBER_PALE  lv_color_hex(0xFFF0C8)  // mallet marker rim
#define PLATE_LABEL_ACCENT lv_color_hex(0xB49A64) // active group label (BODY)
#define PLATE_MARK        lv_color_hex(0x4A4234)  // witness marks
// meter headroom zones (amber COL_HIGHLIGHT is the warn mid-zone)
#define COL_METER_SAFE    lv_color_hex(0x8FAE5A)  // muted green: nominal level
#define COL_METER_HOT     lv_color_hex(0xD9534A)  // red: headroom exceeded
// keyboard well (slightly cool to read as keys, not chassis)
#define KB_WELL           lv_color_hex(0x0F131A)
#define KB_BLACK          lv_color_hex(0x121110)
#define KB_BLACK_HI       lv_color_hex(0x1C1915)
// body-material swatches (preview grid)
#define MAT_WOOD          lv_color_hex(0x8B6A3A)
#define MAT_GLASS         lv_color_hex(0x7AB8FF)
#define MAT_MEMBRANE      lv_color_hex(0xFF6B6B)
#define MAT_STEEL         lv_color_hex(0xA8B0BE)

// === LAYOUT TOKENS — chassis arithmetic (base units @ 1440x860, pre-scale) ===
// Single source of truth for the deterministic grid. Every container in PluginUI.cpp
// derives its size from these numbers via scaled(); painters read the same constants
// the builders do, so nothing can assume a stale container size (the old body-preview
// clip bug: 52px box painted with 72px-era math). Vertical budget, base scale:
//   2*PAD + HEADER_H + ROW_GAP + STAGE_H + ROW_GAP + KB_STRIP_H = 16+64+10+616+10+128 = 860  (exact)
// Horizontal budget (stage inner = 1440-2*16 = 1408):
//   LEFT_W + CENTER_W + RIGHT(grow) + 2*GUTTER = 392 + 430 + 566 + 20 = 1408                 (exact)
namespace lay {
    constexpr int BASE_W = 1440, BASE_H = 860;
    constexpr int PAD = 16;                      // chassis inset on all sides
    constexpr int ROW_GAP = 10;                  // vertical gap between header/stage/keyboard
    constexpr int HEADER_H = 64;                 // title strip
    constexpr int KB_STRIP_H = 128;              // 8 pad + 22 head + 6 gap + 80 keys + 8 pad (+4 slack)
    constexpr int STAGE_H = BASE_H - 2 * PAD - HEADER_H - KB_STRIP_H - 2 * ROW_GAP;  // = 616
    constexpr int GUTTER = 10;                   // horizontal gap between stage columns

    constexpr int LEFT_W = 392;                  // dial bank: 4 knobs x 92 + 3 x 8 gutters
    constexpr int CENTER_W = 430;                // hero plate
    // right analysis column absorbs what remains: 1408 - 392 - 430 - 20 = 566

    // machined knobs (containers; arc/cap/needle internals live in UIWidgets law)
    constexpr int KNOB_W_N = 92, KNOB_H_N = 116, KNOB_ARC_N = 76;   // primary group (BODY)
    constexpr int KNOB_W_C = 88, KNOB_H_C = 98,  KNOB_ARC_C = 64;   // secondary groups
    constexpr int GRID_GUT_X = 8;                // knob pitch gutter (column math above)
    constexpr int SEC_LABEL_H = 16;              // group caption row
    constexpr int SEC_GAP = 6;                   // caption -> knob grid
    constexpr int COORD_H = 20;                  // strike coordinate readout
    // hero strike plate
    constexpr int CARD_PAD = 12;
    constexpr int HEAD_H = 22;                   // card caption rows
    constexpr int DISC_D = CENTER_W - 2 * CARD_PAD;                                  // 406, width-exact
    constexpr int SPEC_STRIP_H = 34;             // body spec cells (BODY/MATERIAL/MODES/F0)

    // body preset preview — builder and painter share these (clip-bug killer):
    //   inner = BOX - 2*PAD = 48; cell = (inner - 3*GAP)/4 = 10; grid = 4*10+3*2 = 46 <= 48
    constexpr int PREVIEW_BOX = 56, PREVIEW_PAD = 4, PREVIEW_GAP = 2;
    constexpr int PRESET_ROW_H = 56;             // preview box + dropdown/info column

    // analysis tower (right column, fills STAGE_H exactly)
    constexpr int SPECTRUM_CARD_H = 370;         // 2*12 pad + 22 head + 8 gap + 316 chart
    constexpr int CHART_H = 316;
    constexpr int SCOPE_CARD_H = 236;            // 2*12 pad + 22 head + 8 + 14 meter + 8 + 160 scope
    constexpr int METER_H = 14;
    constexpr int SCOPE_H = 160;

    // shared control chrome
    constexpr int RADIUS = 6, RADIUS_SM = 4;
    constexpr int BTN_H = 22;                    // small square buttons (ARP/octave/randomize)
    constexpr int BTN_W_OCT = 28, ARP_W = 46, OCTLBL_W = 70, RND_W = 96;
    constexpr int FS_W = 104, FS_H = 30;
    constexpr int DOT = 8;                       // LFO pulse dot
    // piano keys: 7 white x 56 + 6 x 2 gap = 404 wide
    constexpr int KEY_W = 56, KEY_H = 80, KEY_BLACK_W = 28, KEY_BLACK_H = 48, KEY_GAP = 2;
}

 extern float gUIScale;
inline lv_coord_t scaled(lv_coord_t base) { return (lv_coord_t)(base * gUIScale + 0.5f); }
inline const lv_font_t* getScaledFont() {
    if (gUIScale >= 1.5f) return &lv_font_montserrat_20;
    if (gUIScale >= 1.2f) return &lv_font_montserrat_16;
    return &lv_font_montserrat_14;
}
inline const lv_font_t* getScaledSmallFont() {
    if (gUIScale >= 1.5f) return &lv_font_montserrat_16;
    if (gUIScale >= 1.2f) return &lv_font_montserrat_14;
    return &lv_font_montserrat_12;
}
inline const lv_font_t* getScaledMicroFont() {
    if (gUIScale >= 1.2f) return &lv_font_montserrat_14;
    return &lv_font_montserrat_10;
}
inline const lv_font_t* getDisplayFont() {
    return &lv_font_montserrat_24;  // display role: title only, largest baked size
}
END_NAMESPACE_DISTRHO
#endif
