#ifndef UI_COMMON_HPP
#define UI_COMMON_HPP
#include "DistrhoPlugin.hpp"
#include "lvgl.h"
#include "DistrhoPluginInfo.h"
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

// === PIGMENTS-ALIGNED palette (cool charcoal chassis + light-blue accent) ===
// Macro names preserved (UIWidgets.hpp reads these at runtime). Values are
// bar-matched: dark charcoal ~#1A1D24 background, white text, light-blue
// ~#3FA9F5 primary accent. Material swatches stay semantic (wood/glass/etc.)
// because they encode the paper-47 body-material identity, not chrome.
#define COL_BG         lv_color_hex(0x1F2229)
#define COL_ACCENT     lv_color_hex(0x2C3038)
#define COL_BTN_HOVER  lv_color_hex(0x3F4450)
#define COL_HIGHLIGHT  lv_color_hex(0x3FA9F5)  // PRIMARY accent (was amber)
#define COL_TEXT       lv_color_hex(0xE6E8EE)
#define COL_TEXT_DIM   lv_color_hex(0xB4B8C2)
#define COL_HAIRLINE   lv_color_hex(0x3A3E48)
#define COL_KNOB_LABEL lv_color_hex(0x8B8F99)
#define COL_KNOB       lv_color_hex(0xF4F4F4)
#define COL_KNOB_LIGHT lv_color_hex(0xDDE2EA)
#define COL_WHITE      lv_color_hex(0xFFFFFF)
#define COL_BLACK      lv_color_hex(0x000000)
#define COL_KNOB_RING_BG    lv_color_hex(0x15181E)  // dark track
#define COL_KNOB_CAP        lv_color_hex(0x383D46)  // lifted cap for dark theme
#define COL_KNOB_INDICATOR  COL_HIGHLIGHT           // light-blue arc
#define COL_PANEL_DARK lv_color_hex(0x1F2229)
#define COL_BORDER     lv_color_hex(0x4A4E58)
#define COL_CHECKED_BG lv_color_hex(0x2E7BC0)       // active button gradient bottom

// === CHASSIS (Pigments-aligned) ============================================
#define PLATE_BG          lv_color_hex(0x1A1D24)  // chassis - dark cool charcoal
#define PLATE_PANEL       lv_color_hex(0x232830)  // panel surface, slightly lifted
#define PLATE_LINE        lv_color_hex(0x3A3E48)  // hairline rules, dividers, rings
#define PLATE_EDGE        lv_color_hex(0x4A4E58)  // exposed edges, strong borders
#define PLATE_WELL        lv_color_hex(0x15181E)  // inset wells (charts, disc, selects)
#define PLATE_PREVIEW_BG  lv_color_hex(0x15181E)  // body-preview box interior
#define PLATE_WELL_HI     lv_color_hex(0x1A1D24)  // well gradient bottom (depth)
#define PLATE_EMPTY       lv_color_hex(0x1A1D24)  // unoccupied preview cells
#define PLATE_TITLE       lv_color_hex(0xF4F4F4)  // title white
#define PLATE_TEXT        lv_color_hex(0xDDE2EA)  // primary spec text
#define PLATE_TEXT_MID    lv_color_hex(0xA8AEBA)  // secondary text
#define PLATE_TEXT_DIM    lv_color_hex(0x7B8090)  // tertiary/label text
#define PLATE_AMBER_DIM   lv_color_hex(0x2E7BC0)  // dimmed blue (value labels)
#define PLATE_AMBER       lv_color_hex(0x5FBCF8)  // bright blue readout
#define PLATE_AMBER_PALE  lv_color_hex(0x9FD4FF)  // pale blue (mallet rim)
#define PLATE_LABEL_ACCENT lv_color_hex(0x5FBCF8) // active group label (BODY)
#define PLATE_MARK        lv_color_hex(0x3A3E48)  // witness marks
#define PLATE_BTN_PRESS    lv_color_hex(0x232830)  // flat-button pressed surface
#define COL_METER_SAFE    lv_color_hex(0x8FAE5A)  // muted green: nominal level
#define COL_METER_HOT     lv_color_hex(0xD9534A)  // red: headroom exceeded
#define KB_WELL           lv_color_hex(0x15181E)
#define KB_BLACK          lv_color_hex(0x14181E)
#define KB_BLACK_HI       lv_color_hex(0x1E2228)
// body-material swatches (paper-47 identity; KEEP semantic, not chrome)
#define MAT_WOOD          lv_color_hex(0x8B6A3A)
#define MAT_GLASS         lv_color_hex(0x7AB8FF)
#define MAT_MEMBRANE      lv_color_hex(0xFF6B6B)
#define MAT_STEEL         lv_color_hex(0xA8B0BE)
// v2 baked bodies (18-preset set)
#define MAT_HANDPAN       lv_color_hex(0x6B5D52)
#define MAT_LOGDRUM       lv_color_hex(0x7C5230)
#define MAT_MARIMBA       lv_color_hex(0x9A4A26)
#define MAT_COWBELL       lv_color_hex(0xA98B45)
#define MAT_KALIMBA       lv_color_hex(0x56748A)
#define MAT_CELESTA       lv_color_hex(0xBFC9D4)
// === ROUND-9: per-section color bands (Pigments-style source identity) ===
// Each of the four knob groups (BODY/RESONATE/EXCITER/SPACE) gets a distinct
// accent color used for the section label rule and arc fill. Maps cleanly to
// the four functional regions of paper-47 modal synthesis: geometry, resonant
// shaping, excitation, and acoustic space.
#define SEC_BODY          lv_color_hex(0x3FA9F5)  // light-blue (primary chassis)
#define SEC_RESONATE      lv_color_hex(0x4FC1A1)  // teal (resonant shaping)
#define SEC_EXCITER       lv_color_hex(0xF0A050)  // warm orange (excitation)
#define SEC_SPACE         lv_color_hex(0x9F7AD3)  // cool purple (space)

// === LAYOUT TOKENS — chassis arithmetic (base units @ 1440x860, pre-scale) ===


// === LAYOUT TOKENS — chassis arithmetic (base units @ 1440x860, pre-scale) ===
// Single source of truth for the deterministic grid. Every container in PluginUI.cpp
// derives its size from these numbers via scaled(); painters read the same constants
// the builders do, so nothing can assume a stale container size (the old body-preview
// clip bug: 52px box painted with 72px-era math).
//
// Round-2 (piece 1) rebalance — the round-1 critic read "wall of knobs first, disc
// second" and "duplicated M1-M8 strip competes with the 4 dial groups". Rescues the
// disc as the visual hero (smaller + denser markings + last-strike marker) and
// demotes the 8-knob macro rack to a thin 8-dot LED status row at the bottom of
// the nav strip (12px instead of 72px). 60px of vertical room flows into the stage.
//
// Vertical budget, base scale:
//   2*PAD + HEADER_H + NAV_H(24) + MACRO_LED_H(18) + 4*ROW_GAP + STAGE_H + KB_STRIP_H
//   = 16 + 72 + 24 + 18 + 24 + 566 + 128 + 16 = 864  (base_h 860 -> 4px slack)
// Round-6: LED row 12 -> 18 - the M1-M8 micro labels have a ~13px line height
// and were clipped mid-glyph by the nav panel's bottom edge at 12px.
// Horizontal budget (stage inner = 1440-2*16 = 1408):
//   LEFT_W + CENTER_W + RIGHT(grow) + 2*GUTTER = 392 + 480 + 516 + 20 = 1408  (exact)
namespace lay {
    constexpr int BASE_W = 1440, BASE_H = 860;
    constexpr int PAD = 16;                      // chassis inset on all sides
    constexpr int ROW_GAP = 6;                   // vertical gap between regions
    constexpr int HEADER_H = 72;                 // identity bar: brand | preset | master | zoom
    // NAV (h = 24): paper identity row; the macro-LED row lives in the same
    // nav-strip flex column underneath (18px) so it reads as a status
    // annotation, not a control region competing for primary attention.
    constexpr int NAV_H = 24;
    constexpr int MACRO_LED_H = 18;              // 8 status cells: dot + M<n> name (13px label line height)
    constexpr int KB_STRIP_H = 128;              // 8 pad + 22 head + 6 gap + 80 keys + 8 pad (+4 slack)
    constexpr int STAGE_H = BASE_H - 2 * PAD - HEADER_H - NAV_H - MACRO_LED_H - KB_STRIP_H - 4 * ROW_GAP;  // = 566
    // Round-6: brand column widened 240 -> 300 so the fitted title never
    // truncates ("MULTI-SCALE B" clip at 240). Preset browser donates the 60.
    constexpr int KEY_GAP = 2;                   // keybed pitch gap (white keys)
    constexpr int BRAND_W = 300;
    // Round-6: keyboard spans the full footer - 3 octaves (C3-B5), white-key
    // width derived from the strip inner width so the keybed IS the footer.
    constexpr int KEY_OCTAVES = 3;
    constexpr int KEY_WHITE_N = 7 * KEY_OCTAVES;                     // 21
    constexpr int KEY_BLACK_N = 5 * KEY_OCTAVES;                     // 15
    constexpr int KB_INNER_W = BASE_W - 2 * PAD - 16;                // strip pad_all 8 x2
    constexpr int KEY_WHITE_W = (KB_INNER_W - (KEY_WHITE_N - 1) * KEY_GAP) / KEY_WHITE_N;  // 64 @s=1
    constexpr int KEY_BLACK_W = KEY_WHITE_W / 2;                     // 32
    constexpr int GUTTER = 10;                   // horizontal gap between stage columns

    constexpr int LEFT_W = 392;                  // dial bank: 4 knobs x 92 + 3 x 8 gutters
    constexpr int CENTER_W = 480;                // hero plate (widened 430 -> 480; 280 disc + body info breathe)
    // right analysis column: 1408 - 392 - 480 - 20 = 516 (was 566)

    // machined knobs (containers; arc/cap/needle internals live in UIWidgets law)
    constexpr int KNOB_W_N = 92, KNOB_H_N = 116, KNOB_ARC_N = 76;   // primary group (BODY)
    constexpr int KNOB_W_C = 88, KNOB_H_C = 98,  KNOB_ARC_C = 64;   // secondary groups
    constexpr int GRID_GUT_X = 8;                // knob pitch gutter (column math above)
    constexpr int SEC_LABEL_H = 16;              // group caption row
    constexpr int SEC_GAP = 6;                   // caption -> knob grid
    constexpr int COORD_H = 20;                  // strike coordinate readout
    // hero strike plate - round-2 shrink: 280px (was 406) so it stops dominating
    // the stage as a dark empty area; the freed width flows to the spectrum panel
    // and the freed height inside the center column lets body-info + spec-strip
    // spread out beside the disc instead of below it.
    constexpr int CARD_PAD = 12;
    constexpr int HEAD_H = 22;                   // card caption rows
    constexpr int DISC_D = 280;                  // round-2: shrunk from 406
    constexpr int SPEC_STRIP_H = 32;             // body spec cells (BODY/MATERIAL/MODES/F0)

    // body preset preview - builder and painter share these (clip-bug killer):
    //   inner = BOX - 2*PAD = 48; cell = (inner - 3*GAP)/4 = 10; grid = 4*10+3*2 = 46 <= 48
    constexpr int PREVIEW_BOX = 52, PREVIEW_PAD = 4, PREVIEW_GAP = 2;
    constexpr int PRESET_ROW_H = 52;             // preview box + dropdown/info column

    // analysis tower (right column, fills STAGE_H exactly @566):
    // spectrum 360 + gap 6 + scope 200 = 566. Scope grew +4 with the round-6
    // LED-row fix (stage 568 -> 566 would otherwise leave a 4px charcoal seam).
    constexpr int SPECTRUM_CARD_H = 360;         // 2*10 + 22 head + 8 + 280 chart + 8 + 12 band ticks
    constexpr int CHART_H = 280;
    constexpr int TICKS_H = 12;                  // B1..B16 micro-label strip under the spectrum
    constexpr int SCOPE_CARD_H = 200;            // 2*10 + 22 head + 8 + 14 meter + 8 + 138 scope
    constexpr int METER_H = 12;
    constexpr int SCOPE_H = 138;

    // shared control chrome
    constexpr int RADIUS = 6, RADIUS_SM = 4;
    constexpr int BTN_H = 22;                    // small square buttons (ARP/octave/randomize)
    constexpr int BTN_W_OCT = 28, ARP_W = 46, OCTLBL_W = 70, RND_W = 96;
    constexpr int DOT = 8;                       // LFO pulse dot
    // header zoom stepper: [ - ] 100% [ + ]  (replaces the old FULLSCREEN button)
    constexpr int ZOOM_BTN = 24, ZOOM_LBL_W = 46, ZOOM_LBL_H = 30;
    // zoom ladder in % of the 1440x860 base plate; +/- walks the ladder,
    // window keeps the base aspect EXACTLY at every step (w,h scale together)
    inline constexpr int ZOOM_STEPS[] = {50, 75, 100, 125, 150, 200};
    inline constexpr int ZOOM_STEP_COUNT = (int)(sizeof(ZOOM_STEPS) / sizeof(ZOOM_STEPS[0]));
    constexpr int KEY_H = 80, KEY_BLACK_H = 48;  // keybed heights (widths derived above)
    constexpr int DROPDOWN_ROW_H = 15, DROPDOWN_MAX_ROWS = 8;
    // unified drop-shadow direction (one global light, top-left => shadow falls down)
    constexpr int CARD_SHADOW = 10, SHADOW_OFF_Y = 3;
    // spectrum peak-hold caps drawn over the bars (piece-3: thinner/taller tick)
    constexpr int PEAK_CAP_W = 1, PEAK_CAP_H = 6;
    // === ROUND-2: macro strip demoted to LED row ===========================
    // 8 small status dots in a single thin row; each lights amber when its
    // kMacroParams[m] is non-default. inner = 1408 - 7*8 = 1352; cell = 169.
    constexpr int MACRO_LED_GAP = 8;
    constexpr int MACRO_LED_CELL_W = (BASE_W - 2*PAD - 7*MACRO_LED_GAP) / 8;  // 169 @s=1
    constexpr int MACRO_LED_DOT = 4;             // dot diameter (subtle, not a knob)
    // === ROUND-2: disc guide rings =========================================
    // two concentric amber hairlines (center/rim zones) and a small filled
    // last-strike marker that persists for ~0.5s after each hit
    constexpr int DISC_RING_SOFT = 50;           // inner-zone ring radius (relative 0..100, % of DISC_D)
    constexpr int DISC_RING_HARD = 84;           // outer-zone ring radius
    constexpr int DISC_STRIKE_MARKER = 6;        // last-strike marker dot diameter
    constexpr int DISC_STRIKE_HOLD_MS = 500;     // last-strike marker lifetime
    // === ROUND-6: MODE MAP (per-mode strike-gain map, replaces the round-5
    // 16-bar MODE ACTIVITY panel that duplicated the MODE SPECTRUM) =======
    // One thin bar per MODE (up to 128, mode order = ascending baked
    // frequency); height = strike-position gain from the SAME bilinear
    // sound-map the idle spectrum preview uses - engine-truthful modal data,
    // a different question than the live 16-band spectrum above it.
    // Column budget @s=1 (discCol, 566 tall, 6px flex gap): 22 head + 6 +
    // 280 disc + 6 + 22 head + 6 + 224 card = 566 EXACT.
    // Bars: 128 x 1px with 1px gaps = 256 = card inner (280 - 2*12 pad).
    constexpr int MAP_CARD_H = 224;           // full-height card (budget above)
    constexpr int MAP_BARS_H = 186;           // bar strip height (bars bottom-aligned)
    constexpr int MAP_BAR_W = 1;              // per-mode bar width (128-slot comb)
    constexpr int MAP_PEAK_H = 12;            // PEAK/M<n> readout row under the bars
    // top-bar identity cluster: brand mark | preset dropdown | master knob | zoom
    constexpr int NAV_CHIP_W = 86, NAV_CHIP_GAP = 6;
    // === R5: DAMPING panel (per-band tail-time map, fills infoCol dead band) ===
    // Paper-47 physics: each baked preset has 128 modes with per-mode decay
    // RATES; mean decay across a frequency band is the body's damping at
    // that scale - the multi-scale identity the plugin is named for. The
    // panel aggregates 128 modes into 16 log bands, draws a horizontal
    // bar per band whose length tracks 1/<decay>, and reacts live to the
    // DECAY knob (the engine's decayScale_ mirror).
    // infoCol budget: 22 head + 6 + 20 coord + 6 + 1 div + 6 + 52 infoRow +
    // 6 + 1 div + 6 + 34 specStrip + 6 + 400 card + 5*6 inter-gaps = 566
    // (exact match STAGE_H). Card internals: 2*4 pad + 22 head + 4 gap +
    // 16*22 rows = 392; inner 400 - 8 pad = 392. See PluginUI.cpp builder.
    constexpr int DAMP_CARD_H = 400;             // full infoCol-height fill
    constexpr int DAMP_ROW_H = 22;              // per-band row (16 fit cleanly)
    constexpr int DAMP_LABEL_W = 22;            // "B1".."B16" column
    constexpr int DAMP_VAL_W = 40;              // tail-time value column
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
// piece-4: title bumped +2px (24 -> 26). Letter-space stays +2 for the
// wide-set headline feel that makes the brand read as a primary identity.
inline const lv_font_t* getDisplayFont26() { return &lv_font_montserrat_26; }
END_NAMESPACE_DISTRHO
#endif
