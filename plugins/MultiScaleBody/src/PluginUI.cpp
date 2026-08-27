#include "DistrhoPluginInfo.h"
#include <unordered_map>  // pulled in before DistrhoUI.hpp so std:: is fully populated when DPF's
                          // `namespace std { ... }` opens inside DISTRHO (otherwise <unordered_map>
                          // ends up parsed under DISTRHO::std, which has no iterator_traits, forward, pair)
#include "DistrhoUI.hpp"
#include "LVGL.hpp"
#include "lvgl.h"
#include "PluginMultiScaleBody.hpp"
#include "ModalData.hpp"
#include "ui/UIStyles.hpp"
#include "ui/UIWidgets.hpp"
#include "ui/UICommon.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <cmath>
START_NAMESPACE_DISTRHO
float gUIScale = 1.0f;
// Scale from the ACTUAL LVGL surface, not DPF's size bookkeeping: the layout must
// always match what will be drawn. If getSize() ever disagrees with the real window
// (bridged UIs, hosts that clamp resize requests), a stale scale lays content out
// past the visible area - clipped right/bottom edges, seemingly "empty" regions.
static float currentSurfaceScale(){
    lv_display_t* d=lv_display_get_default();
    const float w = d ? (float)lv_display_get_horizontal_resolution(d) : (float)DISTRHO_UI_DEFAULT_WIDTH;
    const float h = d ? (float)lv_display_get_vertical_resolution(d) : (float)DISTRHO_UI_DEFAULT_HEIGHT;
    const float nsW=w/(float)DISTRHO_UI_DEFAULT_WIDTH;
    const float nsH=h/(float)DISTRHO_UI_DEFAULT_HEIGHT;
    return std::clamp(nsW<nsH?nsW:nsH,0.5f,2.5f);
}
class MultiScaleBodyUI;
static float peakOf(const float* bins){
    float m=0.f; for(int b=0;b<16;++b) m=std::max(m,bins[b]); return m;
}
// === STRIKE PLATE ripple machinery (expanding rings from the hit point) ===
static void rippleSizeCb(void* var,int32_t v){ lv_obj_t* r=(lv_obj_t*)var; if(r) lv_obj_set_size(r,v,v); }
static void rippleOpaCb(void* var,int32_t v){ lv_obj_t* r=(lv_obj_t*)var; if(r) lv_obj_set_style_border_opa(r,(lv_opa_t)v,0); }
static void rippleDelCb(lv_anim_t* a){ lv_obj_t* r=(lv_obj_t*)a->var; if(r) lv_obj_del(r); }
// mallet glow pulse: quick zoom pop + shadow bloom on the strike marker
static void pulseZoomCb(void* var,int32_t v){ lv_obj_t* r=(lv_obj_t*)var; if(r) lv_obj_set_style_transform_zoom(r,v,0); }
static void pulseGlowCb(void* var,int32_t v){ lv_obj_t* r=(lv_obj_t*)var; if(r) lv_obj_set_style_shadow_opa(r,(lv_opa_t)v,0); }

// ============================================================================
// DETERMINISTIC GEOMETRY REMAKE
//
// The previous layout died twice from LV_SIZE_CONTENT + flex_grow collapse:
// a content-sized ancestor settles at a stale fixed point and clips everything
// below it, and painters that assume container geometry drift out of sync with
// builders (the body-preview grid was painted with 72px-era math in a 52px box).
//
// Rules enforced in this file:
//   1. Every container gets an EXPLICIT scaled size taken from lay:: tokens
//      (UICommon.hpp). No LV_SIZE_CONTENT anywhere above knobs/preview/keyboard.
//   2. flex_grow only on children whose parent has an explicit size
//      (header title cell, preset info column, analysis column width).
//   3. Painters derive geometry from the same lay:: constants as builders.
//   4. Scrolling is disabled on every container; the whole plate provably fits
//      s = min(w/1440, h/860) because every region sums exactly to the base
//      budget (see the arithmetic comments in UICommon.hpp / buildUI below).
// Direction: MACHINED PLATE / INDUSTRIAL AMBER / TACTILE - warm true-black
// chassis, one amber accent that only ever means value or signal, hero = the
// playable body disc.
// ============================================================================
class MultiScaleBodyUI : public UI, public AbstractMultiScaleBodyUI {
public:
    // auditioning a body - the BODY group's four + four secondaries that change
    // the perceived voice most. Shared between buildUI() and parameterChanged().
    static const uint32_t kMacroParams[8];
    MultiScaleBodyUI(): UI(DISTRHO_UI_DEFAULT_WIDTH, DISTRHO_UI_DEFAULT_HEIGHT),
        fLVGL(nullptr){
        // widget maps + param cache MUST be initialized before buildUI():
        // buildUI() reads paramCache (strike dot pos, preset selection) and writes widgets[]
        clearWidgetRefs();
        paramCache[PluginMultiScaleBody::kParamPitch]=0.5f;
        paramCache[PluginMultiScaleBody::kParamDecay]=0.5f;
        paramCache[PluginMultiScaleBody::kParamBrightness]=0.65f;
        paramCache[PluginMultiScaleBody::kParamStrikeX]=0.5f;
        paramCache[PluginMultiScaleBody::kParamStrikeY]=0.5f;
        paramCache[PluginMultiScaleBody::kParamModeCount]=0.60f;
        paramCache[PluginMultiScaleBody::kParamWidth]=0.30f;
        paramCache[PluginMultiScaleBody::kParamPreset]=0.0f;
        paramCache[PluginMultiScaleBody::kParamRadiation]=0.45f;
        paramCache[PluginMultiScaleBody::kParamAttack]=0.15f;
        paramCache[PluginMultiScaleBody::kParamRelease]=0.45f;
        paramCache[PluginMultiScaleBody::kParamLFORate]=0.30f;
        paramCache[PluginMultiScaleBody::kParamLFODepth]=0.0f;
        paramCache[PluginMultiScaleBody::kParamExciteMix]=0.f;
        paramCache[PluginMultiScaleBody::kParamVelStrike]=0.35f;
        paramCache[PluginMultiScaleBody::kParamDetune]=0.15f;
        paramCache[PluginMultiScaleBody::kParamGlide]=0.15f;
        paramCache[PluginMultiScaleBody::kParamWet]=0.f;
        paramCache[PluginMultiScaleBody::kParamMono]=0.f;
        setSize(DISTRHO_UI_DEFAULT_WIDTH, DISTRHO_UI_DEFAULT_HEIGHT);
        fLVGL = new DGL_NAMESPACE::LVGLTopLevelWidget(getWindow());
        styles.init();
        // buildUI() is deferred to the first uiIdle(): a tree built inside the
        // constructor (before the first LVGL refresh) settles into a 0x0 layout
        // fixed point that never re-runs; trees built while the refresh loop is
        // alive lay out correctly (same path as the rebuildForScale rescale).
    }
    ~MultiScaleBodyUI() override { if(fSpectrumTimer){ lv_timer_del(fSpectrumTimer); fSpectrumTimer=nullptr; } delete fLVGL; }
    std::string parameterName(uint32_t i) const override {
        using P=PluginMultiScaleBody;
        switch(i){
            case P::kParamPitch: return "Tune";        case P::kParamDecay: return "Decay";
            case P::kParamBrightness: return "Bright"; case P::kParamModeCount: return "Modes";
            case P::kParamWidth: return "Width";       case P::kParamRadiation: return "Radiation";
            case P::kParamAttack: return "Attack";     case P::kParamRelease: return "Release";
            case P::kParamLFORate: return "LFO Rate";  case P::kParamLFODepth: return "LFO Depth";
            case P::kParamExciteMix: return "Exciter"; case P::kParamVelStrike: return "Vel Strike";
            case P::kParamDetune: return "Imperfect";  case P::kParamGlide: return "Glide";
            case P::kParamWet: return "Reverb";        case P::kParamMono: return "Mono";
            default: return {}; // bands and metering outputs have no knob title
        }
    }
    float getParamValue(uint32_t i) const override { if(i<PluginMultiScaleBody::kParameterCount) return paramCache[i]; return 0.f; }
    void setParamValue(uint32_t i,float v) override {
        if(i>=PluginMultiScaleBody::kParameterCount) return;
        paramCache[i]=v; setParameterValue(i,v); syncParamWidget(i,v);
        if(i==PluginMultiScaleBody::kParamStrikeX || i==PluginMultiScaleBody::kParamStrikeY) updateStrikeMarker();
        if(i==PluginMultiScaleBody::kParamPreset){ syncPresetDropdown(v); if(bodySubLabel) updateBodyInfo(); updateBodyPreview(); }
    }
    void editParameter(uint32_t i,bool s) override { if(i<PluginMultiScaleBody::kParameterCount) UI::editParameter(i,s); }
    // duplicate parameter widgets - macros + master arc replicate the same
    // param as a knob in the dial bank; the host->UI sync must update every
    // visible instance, not just the last-created one in widgets[]
    static constexpr uint32_t kMaxExtraWidgets=8;   // 8 macros (master shares the dial-bank Wet)
    struct ExtraRef{ uint32_t pi; lv_obj_t* arc; };
    ExtraRef extraWidgets[kMaxExtraWidgets];
    int extraWidgetCount=0;
    void syncParamWidget(uint32_t i,float v) override {
        if(i>=PluginMultiScaleBody::kParameterCount) return;
        if(widgets[i]) UIWidgets::syncFromParam(widgets[i],v);
        for(int e=0;e<extraWidgetCount;++e) if(extraWidgets[e].pi==i && extraWidgets[e].arc)
            UIWidgets::syncFromParam(extraWidgets[e].arc,v);
    }
    void regExtraWidget(uint32_t pi, lv_obj_t* arc){
        if(extraWidgetCount>=kMaxExtraWidgets) return;
        extraWidgets[extraWidgetCount++]={pi,arc};
    }
    void parameterChanged(uint32_t i,float v) override {
        // metering outputs arrive here every audio block - the bridge-safe DSP->UI link
        if(i>=PluginMultiScaleBody::kParamOutBand0 && i<PluginMultiScaleBody::kParameterCount){
            fVizBins[i-PluginMultiScaleBody::kParamOutBand0]=v;
            fGotLiveViz=true;
            return;
        }
        if(i<PluginMultiScaleBody::kParameterCount){ paramCache[i]=v; syncParamWidget(i,v);
            // round-2: LED row mirrors the dial-bank knob for the same param
            for(int m=0;m<8;++m) if(i==kMacroParams[m]) updateMacroLed(m);
            if(i==PluginMultiScaleBody::kParamStrikeX || i==PluginMultiScaleBody::kParamStrikeY) updateStrikeMarker();
            if(i==PluginMultiScaleBody::kParamPreset){ syncPresetDropdown(v); if(bodySubLabel) updateBodyInfo(); updateBodyPreview(); }
        }
    }
    void stateChanged(const char* key,const char* value) override {
        if(key && std::strcmp(key,"arpon")==0){
            arpOnLocal = value && value[0]=='1';
            if(arpBtn){ if(arpOnLocal) lv_obj_add_state(arpBtn,LV_STATE_CHECKED); else lv_obj_clear_state(arpBtn,LV_STATE_CHECKED); }
        }
    }
    void uiIdle() override {
        if(!fUIBuilt){
            buildUI();
            if(fUIBuilt && !fSpectrumTimer){
                fSpectrumTimer = lv_timer_create([](lv_timer_t* t){
                    auto ui=(MultiScaleBodyUI*)lv_timer_get_user_data(t); if(ui) ui->updateSpectrumDisplay();
                },33,this);
            }
        }
        // hosts may open the window at a non-default size without ever sending
        // uiReshape - keep the scale in sync from the real LVGL surface
        const float ns=currentSurfaceScale();
        if(std::abs(ns-::DISTRHO::gUIScale)>=0.01f) rebuildForScale(ns);
        UI::uiIdle();
    }
    void uiReshape(uint w,uint h) override {
        UI::uiReshape(w,h);
        rebuildForScale(currentSurfaceScale());
    }
    // Mouse wheel: DPF delivers it to every top-level widget; the LVGL bridge
    // also queues it as an ENCODER diff (which can only move group focus, never
    // scroll). While the preset list is open we scroll the LIST here and consume
    // the event; otherwise we decline it so nothing is swallowed.
    bool onScroll(const Widget::ScrollEvent& ev) override {
        if(presetDropdown && lv_dropdown_is_open(presetDropdown)){
            lv_obj_t* list=lv_dropdown_get_list(presetDropdown);
            if(list){
                const float dy=ev.delta.getY();
                if(dy>0.f||dy<0.f)
                    lv_obj_scroll_by(list,0,(lv_coord_t)((dy>0.f?1:-1)*scaled(lay::DROPDOWN_ROW_H)),LV_ANIM_OFF);
                return true;
            }
        }
        return false;
    }
    void rebuildForScale(float ns){
        if(std::abs(ns-::DISTRHO::gUIScale)<0.01f) return;
        ::DISTRHO::gUIScale=ns;
        if(!fUIBuilt) return;
        styles.reset(); styles.init();
        if(kbHeldNote>=0 && kbHeldNote<=127){ sendNote(0,(uint8_t)kbHeldNote,0); kbHeldNote=-1; }
        for(int o=0;o<12;++o){ int n=kbBaseNote+o; if(n>=0&&n<=127) sendNote(0,(uint8_t)n,0); }
        lv_obj_t* root=lv_screen_active(); if(root) lv_obj_clean(root);
        fUIBuilt=false;
        clearWidgetRefs();
        fPrevEnergy=0.f; fLevelEnv=0.f; fMeterEnv=0.f; fMeterPeak=0.f; fPeakAge=0; gScopeMax=0.05f; fRippleCooldown=0; fStrikeHeld=false;
        fMarkerPlaced=false;
        buildUI();
        if(fUIBuilt && !fSpectrumTimer){
            fSpectrumTimer = lv_timer_create([](lv_timer_t* t){
                auto ui=(MultiScaleBodyUI*)lv_timer_get_user_data(t); if(ui) ui->updateSpectrumDisplay();
            },33,this);
        }
    }
private:
    // single owner of every lv_obj_t* member default; ctor and rebuildForScale share it
    void clearWidgetRefs(){
        for(uint32_t i=0;i<PluginMultiScaleBody::kParameterCount;++i){ widgets[i]=nullptr; paramCache[i]=0.5f; }
        extraWidgetCount=0; for(uint32_t i=0;i<kMaxExtraWidgets;++i) extraWidgets[i]={0,nullptr};
        strikeDisc=strikeDot=strikeCoordLabel=presetDropdown=bodySubLabel=nullptr;
        // piece-6: preset browser prev/next arrows
        presetPrevBtn=presetNextBtn=nullptr;
        bodyPreview=lfoDot=strikeLastMark=nullptr;
        hdrBodyVal=hdrMatVal=hdrModeVal=hdrF0Val=nullptr;
        fSpectrumChart=fScopeChart=fLevelBar=fLevelPeak=zoneWarnMark=zoneHotMark=nullptr;
        fScopeSeries=nullptr; arpBtn=nullptr;
        kbContainer=kbOctLabel=zoomMinus=zoomPlus=zoomValLbl=nullptr;
        for(int i=0;i<7;++i) kbWhite[i]=nullptr;
        for(int i=0;i<5;++i) kbBlack[i]=nullptr;
        for(int i=0;i<8;++i) macroLedDots[i]=nullptr;
    }

    // ---- shared builder/painter geometry -----------------------------------
    // Body-preview grid math lives HERE ONLY. The builder sizes the box from
    // lay::PREVIEW_*, this function derives cell/gap/inset from the SAME
    // constants, so the painting can never overflow the container it lives in.
    static void previewGeometry(int& cell,int& gap,int& off){
        gap = scaled(lay::PREVIEW_GAP);
        const int inner = scaled(lay::PREVIEW_BOX) - 2*scaled(lay::PREVIEW_PAD);
        cell = (inner - 3*gap) / 4;                 // (48 - 3*2)/4 = 10 at s=1
        const int grid = 4*cell + 3*gap;            // 46 <= inner 48
        off = std::max(0, (inner - grid) / 2);      // center the slack
    }
    void updateBodyInfo(){
        if(!bodySubLabel) return;
        int mx = modal::kNumPresets - 1;
        int idx = (int)std::round(paramCache[PluginMultiScaleBody::kParamPreset]*(float)mx);
        idx = std::clamp(idx, 0, mx);
        const auto& pr = modal::kPresets[idx];
        char buf[160];
        const char* mat = "Alloy";
        std::string n = pr.name;
        if(n=="WoodBlock" || n=="Squirrel") mat="Pine - Wood";
        else if(n=="Membrane") mat="Membrane";
        else if(n=="Glass") mat="Crystal";
        else if(n=="Bar" || n=="Chime" || n=="Plate" || n=="Gong" || n=="Handpan" || n=="Kalimba" || n=="Celesta") mat="Steel";
        else if(n=="Bell" || n=="Shell" || n=="Cowbell") mat="Bronze";
        else if(n=="Bowl" || n=="Blade") mat="Aluminium";
        else if(n=="LogDrum") mat="Mahogany";
        else if(n=="Marimba") mat="Rosewood";
        snprintf(buf,sizeof(buf),"%s  -  %s  -  %d modes  -  %.0f Hz", pr.name, mat, pr.n, pr.freq[0]/(2*3.14159f));
        lv_label_set_text(bodySubLabel, buf);
        if(hdrBodyVal){ char b[32]; snprintf(b,sizeof(b),"%s",pr.name); lv_label_set_text(hdrBodyVal,b); }
        if(hdrMatVal){ lv_label_set_text(hdrMatVal,mat); }
        if(hdrModeVal){ char b[16]; snprintf(b,sizeof(b),"%d",pr.n); lv_label_set_text(hdrModeVal,b); }
        if(hdrF0Val){ char b[16]; snprintf(b,sizeof(b),"%.0f HZ",pr.freq[0]/(2*3.14159f)); lv_label_set_text(hdrF0Val,b); }
    }
    void updateBodyPreview(){
        if(!bodyPreview) return;
        lv_obj_clean(bodyPreview);
        int mx = modal::kNumPresets - 1;
        int idx = (int)std::round(paramCache[PluginMultiScaleBody::kParamPreset]*(float)mx);
        idx = std::clamp(idx,0,mx);
        std::string name = modal::kPresets[idx].name;
        // geometry from the SHARED helper - same numbers the builder used
        int cell=10,gap=2,off=0; previewGeometry(cell,gap,off);
        for(int y=0;y<4;++y){
            for(int x=0;x<4;++x){
                float occ=0;
                if(name=="Bowl"){ bool inner = x>=1 && x<=2 && y>=1 && y<=2; occ = inner?0.15f:1.f; }
                else if(name=="Plate"){ occ = (y==3)?1.f:(y==2?0.5f:0.f); }
                else if(name=="Squirrel"){ occ = (x>=1&&x<=2&&y>=1&&y<=2)?1.f:0.f; if(x==2&&y==3) occ=0.7f; if(x==0&&y==2) occ=0.5f; }
                else if(name=="Blade"){ occ = (x>=1&&x<=2&&y>=1&&y<=2)?1.f:0.f; if((x==0||x==3)&&y==2) occ=0.9f; }
                else if(name=="Shell"){ float dx=x-1.5f, dy=y-1.5f; float r=std::sqrt(dx*dx+dy*dy); if(r>1.2f&&r<1.9f) occ=1.f; else if(r>1.f&&r<2.1f) occ=0.5f; }
                else if(name=="Bar"){ occ=(y==1)?1.f:0.f; }
                else if(name=="Membrane"){ float dx=x-1.5f, dy=y-1.5f; if(dx*dx+dy*dy < 3.2f) occ=1.f; }
                else if(name=="Bell"){ float dx=x-1.5f, dy=y-1.5f; float r=std::sqrt(dx*dx+dy*dy); if(r>1.f&&r<1.9f) occ=1.f; else if(r>0.9f&&r<2.05f) occ=0.45f; occ = std::max(occ, (y==0?0.6f:0.f)); }
                else if(name=="Gong"){ float dx=x-1.5f, dy=y-1.5f; float rad=std::sqrt(dx*dx+dy*dy); if(rad<1.9f) occ=1.f; else if(rad<2.08f) occ=0.35f; if(rad<0.88f) occ=1.f; }
                // v2 baked bodies: declarative 4x4 occupancy matrices (row-major [y][x])
                else {
                    static const float kHandpan[16]={0.f,.35f,.35f,0.f, .35f,.85f,.85f,.35f, .35f,.85f,1.f,.35f, 0.f,.35f,.35f,0.f};
                    static const float kLogDrum[16]={0.f,.15f,.15f,0.f, .55f,.95f,1.f,.95f, .55f,.95f,1.f,.95f, 0.f,.15f,.15f,0.f};
                    static const float kMarimba[16]={0.f,.25f,.25f,0.f, .40f,.80f,1.f,.80f, .40f,.80f,1.f,.80f, 0.f,.25f,.25f,0.f};
                    static const float kCowbell[16]={.10f,.20f,.20f,.10f, .90f,.35f,.35f,.90f, .90f,.35f,.35f,.90f, .10f,.20f,.20f,.10f};
                    static const float kKalimba[16]={0.f,.50f,1.f,.50f, 0.f,.55f,1.f,.55f, 0.f,.45f,.85f,.45f, 0.f,.35f,.65f,.35f};
                    static const float kCelesta[16]={0.f,.20f,.20f,0.f, .35f,.75f,1.f,.75f, .35f,.75f,1.f,.75f, 0.f,.20f,.20f,0.f};
                    const float* t=nullptr;
                    if(name=="Handpan") t=kHandpan;
                    else if(name=="LogDrum") t=kLogDrum;
                    else if(name=="Marimba") t=kMarimba;
                    else if(name=="Cowbell") t=kCowbell;
                    else if(name=="Kalimba") t=kKalimba;
                    else if(name=="Celesta") t=kCelesta;
                    if(t) occ=t[y*4+x];
                }
                occ = std::clamp(occ,0.f,1.f);
                lv_obj_t* cellObj=lv_obj_create(bodyPreview);
                lv_obj_set_size(cellObj,cell,cell);
                lv_obj_set_pos(cellObj, off + x*(cell+gap), off + y*(cell+gap));
                lv_obj_set_style_radius(cellObj,scaled(3),0);
                lv_obj_set_style_border_width(cellObj,1,0);
                lv_obj_set_style_border_color(cellObj,COL_HAIRLINE,0);
                lv_obj_set_style_pad_all(cellObj,0,0);
                lv_obj_clear_flag(cellObj,LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(cellObj,LV_OBJ_FLAG_CLICKABLE);
                if(occ<0.02f){ lv_obj_set_style_bg_color(cellObj,PLATE_EMPTY,0); lv_obj_set_style_bg_opa(cellObj,LV_OPA_60,0); }
                else {
                    // piece-5: body preview grid - one-accent discipline. The
                    // material swatch color already exists per material (Wood,
                    // Glass, Steel, etc). For the Bell/Gong/Shell fall-through
                    // we previously defaulted to COL_HIGHLIGHT (full amber);
                    // demote that to PLATE_TEXT_MID so the only full-amber mark
                    // on the chassis is the indicator arc + mallet.
                    lv_color_t base = PLATE_TEXT_MID;
                    if(name=="WoodBlock"||name=="Squirrel") base = MAT_WOOD;
                    else if(name=="Glass") base = MAT_GLASS;
                    else if(name=="Membrane") base = MAT_MEMBRANE;
                    else if(name=="Plate"||name=="Bar"||name=="Chime") base = MAT_STEEL;
                    else if(name=="Handpan") base = MAT_HANDPAN;
                    else if(name=="LogDrum") base = MAT_LOGDRUM;
                    else if(name=="Marimba") base = MAT_MARIMBA;
                    else if(name=="Cowbell") base = MAT_COWBELL;
                    else if(name=="Kalimba") base = MAT_KALIMBA;
                    else if(name=="Celesta") base = MAT_CELESTA;
                    lv_obj_set_style_bg_color(cellObj,base,0);
                    lv_obj_set_style_bg_opa(cellObj, (lv_opa_t)(LV_OPA_30 + occ*0.7f*255),0);
                    lv_obj_set_style_shadow_width(cellObj, occ>0.9f?scaled(4):0,0);
                    lv_obj_set_style_shadow_color(cellObj,base,0);
                    lv_obj_set_style_shadow_opa(cellObj, occ>0.6f?LV_OPA_30:LV_OPA_0,0);
                }
            }
        }
    }
    void syncPresetDropdown(float v){
        if(!presetDropdown) return;
        int mx = modal::kNumPresets - 1;
        int idx = (int)std::round(v*(float)mx); idx=std::clamp(idx,0,mx);
        lv_dropdown_set_selected(presetDropdown, idx);
    }
    // ---- round-2 helpers (LED row + disc info) ----------------------------
    // An LED is "lit" when its macro param is non-default (anything but 0.5
    // for normalized; except Wet=0 / Mono=0 which are themselves the default).
    // We compare against a small epsilon to absorb host round-trip quantization.
    void updateMacroLed(int m){
        if(m<0||m>=8||!macroLedDots[m]) return;
        const float v=paramCache[kMacroParams[m]];
        // 0.0 is the true default for: Wet (0%), Mono (off).
        // For everything else 0.5 is the true default (per PluginUI ctor).
        const float defv=(kMacroParams[m]==PluginMultiScaleBody::kParamWet
                       ||kMacroParams[m]==PluginMultiScaleBody::kParamMono) ? 0.f : 0.5f;
        const bool lit=std::fabs(v-defv)>0.01f;
        lv_obj_set_style_bg_color(macroLedDots[m], lit?COL_HIGHLIGHT:PLATE_MARK, 0);
    }
    // last-strike marker: a small filled amber dot at the click position that
    // persists for ~0.5s after the hit and then fades. Visual feedback the
    // disc actually registered the strike (the dynamic mallet marker
    // also pops on hit, but the persistent mark is the "audit trail").
    void placeLastStrike(int px,int py){
        if(!strikeDisc) return;
        if(!strikeLastMark){
            strikeLastMark=makeBox(strikeDisc,scaled(lay::DISC_STRIKE_MARKER),scaled(lay::DISC_STRIKE_MARKER));
            lv_obj_set_style_radius(strikeLastMark,LV_RADIUS_CIRCLE,0);
            lv_obj_set_style_bg_color(strikeLastMark,COL_HIGHLIGHT,0);
            lv_obj_set_style_bg_opa(strikeLastMark,LV_OPA_COVER,0);
            lv_obj_set_style_border_color(strikeLastMark,PLATE_AMBER_PALE,0);
            lv_obj_set_style_border_width(strikeLastMark,1,0);
            lv_obj_set_style_shadow_width(strikeLastMark,scaled(8),0);
            lv_obj_set_style_shadow_color(strikeLastMark,COL_HIGHLIGHT,0);
            lv_obj_set_style_shadow_opa(strikeLastMark,LV_OPA_60,0);
            lv_obj_clear_flag(strikeLastMark,LV_OBJ_FLAG_CLICKABLE);
        }
        const int m=(int)lv_obj_get_width(strikeLastMark);
        lv_obj_set_pos(strikeLastMark, px-m/2, py-m/2);
        lv_obj_set_style_bg_opa(strikeLastMark,LV_OPA_COVER,0);
        fLastStrikeAgeMs=0;
    }
    void updateStrikeMarker(){
        if(!strikeDisc || !strikeDot) return;
        lv_coord_t pw = lv_obj_get_width(strikeDisc);
        lv_coord_t ph = lv_obj_get_height(strikeDisc);
        int dotS = scaled(lay::DOT+4);              // mallet marker 12px base
        if(pw<=0||ph<=0) return;                    // no geometry yet; timer retries
        int x = (int)(paramCache[PluginMultiScaleBody::kParamStrikeX] * (pw - dotS));
        int y = (int)((1.f - paramCache[PluginMultiScaleBody::kParamStrikeY]) * (ph - dotS));
        lv_obj_set_pos(strikeDot, x, y);
        if(strikeCoordLabel){
            char buf[32];
            snprintf(buf,sizeof(buf),"X %.2f  -  Y %.2f", paramCache[PluginMultiScaleBody::kParamStrikeX], paramCache[PluginMultiScaleBody::kParamStrikeY]);
            lv_label_set_text(strikeCoordLabel, buf);
        }
    }
    void spawnRipple(){
        if(!strikeDisc || !strikeDot) return;
        lv_coord_t dw = lv_obj_get_width(strikeDisc);
        lv_coord_t dh = lv_obj_get_height(strikeDisc);
        if(dw<=0||dh<=0) return;
        int cx = lv_obj_get_x(strikeDot) + lv_obj_get_width(strikeDot)/2;
        int cy = lv_obj_get_y(strikeDot) + lv_obj_get_height(strikeDot)/2;
        for(int k=0;k<3;++k){
            lv_obj_t* ring=lv_obj_create(strikeDisc);
            lv_obj_remove_style_all(ring);
            int d0=scaled(lay::DOT+6);
            lv_obj_set_size(ring,d0,d0);
            lv_obj_set_pos(ring,cx-d0/2,cy-d0/2);
            lv_obj_set_style_radius(ring,LV_RADIUS_CIRCLE,0);
            lv_obj_set_style_bg_opa(ring,LV_OPA_TRANSP,0);
            lv_obj_set_style_border_color(ring,COL_HIGHLIGHT,0);
            lv_obj_set_style_border_width(ring,k==0?2:1,0);
            lv_obj_set_style_border_opa(ring,(lv_opa_t)(220-k*60),0);
            lv_obj_clear_flag(ring,LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(ring,LV_OBJ_FLAG_SCROLLABLE);
            int endD=(int)((float)(dw>dh?dw:dh)*0.92f);
            lv_anim_t aS; lv_anim_init(&aS);
            lv_anim_set_var(&aS,ring);
            lv_anim_set_exec_cb(&aS,(lv_anim_exec_xcb_t)rippleSizeCb);
            lv_anim_set_values(&aS,d0,endD);
            lv_anim_set_time(&aS,700); lv_anim_set_delay(&aS,k*140);
            lv_anim_set_path_cb(&aS,lv_anim_path_ease_out);
            lv_anim_start(&aS);
            lv_anim_t aO; lv_anim_init(&aO);
            lv_anim_set_var(&aO,ring);
            lv_anim_set_exec_cb(&aO,(lv_anim_exec_xcb_t)rippleOpaCb);
            lv_anim_set_values(&aO,(lv_opa_t)(220-k*60),LV_OPA_0);
            lv_anim_set_time(&aO,760+k*120); lv_anim_set_delay(&aO,k*140);
            lv_anim_set_path_cb(&aO,lv_anim_path_linear);
            lv_anim_set_ready_cb(&aO,rippleDelCb);
            lv_anim_start(&aO);
        }
    }
    // mallet marker pop on hit: zoom 256(=1.0) -> ~1.4x -> rest, shadow blooms
    void spawnMalletPulse(){
        if(!strikeDot) return;
        lv_anim_t aZ; lv_anim_init(&aZ);
        lv_anim_set_var(&aZ,strikeDot);
        lv_anim_set_exec_cb(&aZ,(lv_anim_exec_xcb_t)pulseZoomCb);
        lv_anim_set_values(&aZ,256,358);
        lv_anim_set_time(&aZ,90); lv_anim_set_path_cb(&aZ,lv_anim_path_ease_out);
        lv_anim_start(&aZ);
        lv_anim_t aZ2; lv_anim_init(&aZ2);
        lv_anim_set_var(&aZ2,strikeDot);
        lv_anim_set_exec_cb(&aZ2,(lv_anim_exec_xcb_t)pulseZoomCb);
        lv_anim_set_values(&aZ2,358,256);
        lv_anim_set_time(&aZ2,220); lv_anim_set_delay(&aZ2,95); lv_anim_set_path_cb(&aZ2,lv_anim_path_ease_in_out);
        lv_anim_start(&aZ2);
        lv_anim_t aG; lv_anim_init(&aG);
        lv_anim_set_var(&aG,strikeDot);
        lv_anim_set_exec_cb(&aG,(lv_anim_exec_xcb_t)pulseGlowCb);
        lv_anim_set_values(&aG,LV_OPA_60,LV_OPA_COVER);
        lv_anim_set_time(&aG,90); lv_anim_set_path_cb(&aG,lv_anim_path_ease_out);
        lv_anim_start(&aG);
        lv_anim_t aG2; lv_anim_init(&aG2);
        lv_anim_set_var(&aG2,strikeDot);
        lv_anim_set_exec_cb(&aG2,(lv_anim_exec_xcb_t)pulseGlowCb);
        lv_anim_set_values(&aG2,LV_OPA_COVER,LV_OPA_60);
        lv_anim_set_time(&aG2,260); lv_anim_set_delay(&aG2,95); lv_anim_set_path_cb(&aG2,lv_anim_path_ease_in_out);
        lv_anim_start(&aG2);
    }
    // contextual value formatting (design guidelines: units where they exist).
    // Mirrors the engine's own denormalization so readouts match the audio:
    //   Glide v*600 ms - LFO 0.05*240^v Hz - Modes 8+v*120 - Reverb % - Tune +-24 ST
    void formatParamValue(int pi,float v,char* buf,size_t cap) const {
        using P=PluginMultiScaleBody;
        switch(pi){
            case P::kParamWet:      snprintf(buf,cap,"%d %%",(int)std::lround(v*100.f)); break;
            case P::kParamGlide:    snprintf(buf,cap,"%d MS",(int)std::lround(v*600.f)); break;
            case P::kParamLFORate: {
                float hz=0.05f*std::pow(240.f,v);
                if(hz>=10.f) snprintf(buf,cap,"%.0f HZ",hz);
                else         snprintf(buf,cap,"%.2f HZ",hz);
                break; }
            case P::kParamModeCount: snprintf(buf,cap,"%d",8+(int)(v*120.f)); break;
            case P::kParamPitch:     snprintf(buf,cap,"%+.1f ST",(v-0.5f)*48.f); break;
            default:                 snprintf(buf,cap,"%.2f",v); break;
        }
    }
    // registered AFTER UIWidgets' own arc handlers (insertion order), so the
    // formatted text wins over their raw %.2f writes on every drag frame
    static void valueFormatCb(lv_event_t* e){
        auto code=lv_event_get_code(e);
        if(code!=LV_EVENT_VALUE_CHANGED && code!=LV_EVENT_PRESSING) return;
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        lv_obj_t* arc=(lv_obj_t*)lv_event_get_target(e);
        if(!ui||!arc) return;
        int pi=(int)(intptr_t)lv_obj_get_user_data(arc);
        lv_obj_t* cont=lv_obj_get_parent(arc);
        if(!cont) return;
        lv_obj_t* lbl=lv_obj_get_child(cont,lv_obj_get_child_count(cont)-1);
        if(!lbl||!lv_obj_check_type(lbl,&lv_label_class)) return;
        char buf[24];
        ui->formatParamValue(pi,lv_arc_get_value(arc)/1000.f,buf,sizeof(buf));
        lv_label_set_text(lbl,buf);
    }
    // spectrum peak-hold caps: falling-hold markers painted over the bars in
    // DRAW_POST_END. Peak state lives in fSpecPeaks[]/fSpecHoldAge[]; y-mapping
    // mirrors the chart's own value->y mapping for range 0..1000.
    static void spectrumPeakDrawCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        lv_obj_t* chart=(lv_obj_t*)lv_event_get_target(e);
        if(!ui||!chart||chart!=ui->fSpectrumChart) return;
        lv_chart_series_t* s=lv_chart_get_series_next(chart,nullptr);
        if(!s) return;
        lv_area_t cc; lv_obj_get_coords(chart,&cc);
        const lv_coord_t bl=lv_obj_get_style_border_width(chart,LV_PART_MAIN);
        const lv_coord_t pl=bl+lv_obj_get_style_pad_left(chart,LV_PART_MAIN);
        const lv_coord_t pt=bl+lv_obj_get_style_pad_top(chart,LV_PART_MAIN);
        const lv_coord_t pb=bl+lv_obj_get_style_pad_bottom(chart,LV_PART_MAIN);
        const lv_coord_t h=cc.y2-pt-pb-cc.y1+1;
        const lv_coord_t contentBottom=cc.y2-pb;
        lv_layer_t* layer=lv_event_get_layer(e);
        for(int b=0;b<16;++b){
            if(ui->fSpecPeaks[b]<=0.003f) continue;
            lv_point_t p; lv_chart_get_point_pos_by_id(chart,s,b,&p);
            lv_draw_rect_dsc_t dsc; lv_draw_rect_dsc_init(&dsc);
            dsc.bg_color=PLATE_AMBER_PALE; dsc.bg_opa=LV_OPA_80;
            dsc.radius=1; dsc.border_width=0; dsc.shadow_width=0;
            lv_coord_t w=scaled(lay::PEAK_CAP_W), hh=scaled(lay::PEAK_CAP_H);
            lv_area_t a;
            a.x1=(lv_coord_t)(cc.x1+pl+p.x-w/2); a.x2=a.x1+w-1;
            a.y1=(lv_coord_t)(contentBottom-h*ui->fSpecPeaks[b]-hh/2);
            a.y2=a.y1+hh-1;
            lv_draw_rect(layer,&dsc,&a);
        }
    }
    // meter zone marks sit at fixed fractions of the measured bar width;
    // called after the tree has settled (positions are meaningless before)
    void layoutMeterMarks(){
        if(!fLevelBar || !zoneWarnMark || !zoneHotMark) return;
        const lv_coord_t bw=lv_obj_get_width(fLevelBar);
        if(bw<=4) return;
        lv_obj_set_x(zoneWarnMark,(lv_coord_t)(bw*60/100));
        lv_obj_set_x(zoneHotMark,(lv_coord_t)(bw*85/100));
    }
    static void dropdownCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        lv_obj_t* dd=(lv_obj_t*)lv_event_get_target(e);
        int sel=lv_dropdown_get_selected(dd);
        int mx = modal::kNumPresets - 1;
        float v = mx ? (float)sel/(float)mx : 0.f;
        if(ui){ ui->editParameter(PluginMultiScaleBody::kParamPreset,true); ui->setParamValue(PluginMultiScaleBody::kParamPreset, v); ui->editParameter(PluginMultiScaleBody::kParamPreset,false); }
    }
    // piece-6: preset browser prev/next arrow click. Cycles the selected
    // preset by +/-1 (with wrap) and writes through setParamValue so the
    // existing dropdown sync + body info + body preview paths all fire.
    static void presetArrowCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        if(!ui||!ui->presetDropdown) return;
        int dir=(int)(intptr_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e));
        if(dir==0) return;
        int mx = modal::kNumPresets - 1;
        int cur=lv_dropdown_get_selected(ui->presetDropdown);
        int nxt=std::clamp(cur+dir, 0, mx);
        if(nxt==cur && ((dir<0 && cur==0) || (dir>0 && cur==mx))){
            // wrap at the end
            nxt = (dir<0) ? mx : 0;
        }
        float v = mx ? (float)nxt/(float)mx : 0.f;
        ui->editParameter(PluginMultiScaleBody::kParamPreset,true);
        ui->setParamValue(PluginMultiScaleBody::kParamPreset, v);
        ui->editParameter(PluginMultiScaleBody::kParamPreset,false);
    }
    static void padPressCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        if(!ui||!ui->strikeDisc) return;
        lv_indev_t* indev=lv_indev_get_act(); if(!indev) return;
        lv_point_t p; lv_indev_get_point(indev,&p);
        lv_area_t coords; lv_obj_get_coords(ui->strikeDisc,&coords);
        lv_coord_t pw = coords.x2 - coords.x1 +1;
        lv_coord_t ph = coords.y2 - coords.y1 +1;
        float fx = std::clamp((float)(p.x - coords.x1)/(float)pw, 0.f,1.f);
        float fy = std::clamp(1.f - (float)(p.y - coords.y1)/(float)ph, 0.f,1.f);
        auto code = lv_event_get_code(e);
        if(code==LV_EVENT_PRESSED || code==LV_EVENT_PRESSING){
            if(code==LV_EVENT_PRESSED){
                ui->editParameter(PluginMultiScaleBody::kParamStrikeX,true);
                ui->editParameter(PluginMultiScaleBody::kParamStrikeY,true);
                ui->spawnMalletPulse();   // visual hit confirmation
                // round-2 audit trail: a small amber dot persists at the
                // strike point for ~0.5s, fading out via the spectrum timer
                ui->placeLastStrike(p.x - coords.x1, p.y - coords.y1);
                // physical hit: strike position first, then trigger the body
                ui->sendNote(0,(uint8_t)ui->fStrikeNote,100);
                ui->fStrikeHeld=true;
            }
            ui->setParamValue(PluginMultiScaleBody::kParamStrikeX, fx);
            ui->setParamValue(PluginMultiScaleBody::kParamStrikeY, fy);
        } else if(code==LV_EVENT_RELEASED || code==LV_EVENT_PRESS_LOST){
            if(ui->fStrikeHeld){ ui->sendNote(0,(uint8_t)ui->fStrikeNote,0); ui->fStrikeHeld=false; }
            ui->editParameter(PluginMultiScaleBody::kParamStrikeX,false);
            ui->editParameter(PluginMultiScaleBody::kParamStrikeY,false);
        }
    }
    static void spectrumBandCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        if(!ui || !ui->fSpectrumChart) return;
        lv_indev_t* indev=lv_indev_get_act(); if(!indev) return;
        lv_point_t p; lv_indev_get_point(indev,&p);
        lv_area_t coords; lv_obj_get_coords(ui->fSpectrumChart,&coords);
        lv_coord_t w = coords.x2 - coords.x1 + 1;
        lv_coord_t h = coords.y2 - coords.y1 + 1;
        float fx = std::clamp((float)(p.x - coords.x1)/(float)w, 0.f, 1.f);
        float fy = std::clamp(1.f - (float)(p.y - coords.y1)/(float)h, 0.f, 1.f);
        int band = std::clamp((int)(fx*16.f),0,15);
        float level = std::clamp(fy,0.f,1.f);
        auto code = lv_event_get_code(e);
        if(code==LV_EVENT_PRESSED || code==LV_EVENT_PRESSING){
            if(band!=ui->fScrubBand){
                // drag crossed into another band: close the old edit bracket,
                // open the new one (RELEASED closes by member, not by position)
                if(ui->fScrubBand>=0) ui->editParameter(PluginMultiScaleBody::kParamBand0+ui->fScrubBand,false);
                ui->fScrubBand=band;
                ui->fScrubLevel=level;
                ui->editParameter(PluginMultiScaleBody::kParamBand0+band,true);
                ui->setParamValue(PluginMultiScaleBody::kParamBand0+band, level);
            } else if(level!=ui->fScrubLevel){   // same band: write only on change
                ui->fScrubLevel=level;
                ui->setParamValue(PluginMultiScaleBody::kParamBand0+band, level);
            }
        } else if(code==LV_EVENT_RELEASED || code==LV_EVENT_PRESS_LOST){
            if(ui->fScrubBand>=0){
                ui->editParameter(PluginMultiScaleBody::kParamBand0+ui->fScrubBand,false);
                ui->fScrubBand=-1;
            }
        }
    }
    void updateKeyboardNotes(){
        if(!kbContainer) return;
        static const int whiteOff[7]={0,2,4,5,7,9,11};
        static const int blackOff[5]={1,3,6,8,10};
        static const char* whiteName[7]={"C","D","E","F","G","A","B"};
        for(int i=0;i<7;++i){
            if(!kbWhite[i]) continue;
            int note = kbBaseNote + whiteOff[i];
            note = std::clamp(note,0,127);
            lv_obj_set_user_data(kbWhite[i], (void*)(intptr_t)note);
            lv_obj_t* child = lv_obj_get_child(kbWhite[i],0);
            if(child && lv_obj_check_type(child,&lv_label_class)){
                int oct = note/12 -1;
                char buf[8]; snprintf(buf,sizeof(buf),"%s%d",whiteName[i],oct);
                lv_label_set_text(child,buf);
            }
        }
        for(int i=0;i<5;++i){
            if(!kbBlack[i]) continue;
            int note = kbBaseNote + blackOff[i];
            note = std::clamp(note,0,127);
            lv_obj_set_user_data(kbBlack[i], (void*)(intptr_t)note);
        }
        if(kbOctLabel){
            int oct = kbBaseNote/12 -1;
            char buf[16]; snprintf(buf,sizeof(buf),"C%d - B%d",oct,oct);
            lv_label_set_text(kbOctLabel,buf);
        }
    }
    static bool isBlackMidiNote(int n){ int pc=n%12; return pc==1||pc==3||pc==6||pc==8||pc==10; }
    static void keyEventCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        lv_obj_t* key=(lv_obj_t*)lv_event_get_target(e);
        if(!ui||!key) return;
        int note=(int)(intptr_t)lv_obj_get_user_data(key);
        if(note<0||note>127) return;
        auto code=lv_event_get_code(e);
        bool isBlack=isBlackMidiNote(note);
        if(code==LV_EVENT_PRESSED){
            ui->sendNote(0,(uint8_t)note,100); ui->kbHeldNote=note;
            lv_obj_set_style_bg_color(key,COL_HIGHLIGHT,0); lv_obj_set_style_bg_opa(key,LV_OPA_COVER,0);
            if(!isBlack) lv_obj_set_style_text_color(key,COL_BG,0);
        } else if(code==LV_EVENT_RELEASED || code==LV_EVENT_PRESS_LOST || code==LV_EVENT_LEAVE){
            ui->sendNote(0,(uint8_t)note,0); if(ui->kbHeldNote==note) ui->kbHeldNote=-1;
            if(isBlack){ lv_obj_set_style_bg_color(key,KB_BLACK,0); lv_obj_set_style_bg_opa(key,LV_OPA_COVER,0); }
            else { lv_obj_set_style_bg_color(key,COL_KNOB,0); lv_obj_set_style_bg_opa(key,LV_OPA_COVER,0); lv_obj_set_style_text_color(key,COL_PANEL_DARK,0); }
        }
    }
    static void octaveBtnCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        lv_obj_t* btn=(lv_obj_t*)lv_event_get_target(e);
        if(!ui||!btn) return;
        int dir=(int)(intptr_t)lv_obj_get_user_data(btn);
        int next=ui->kbBaseNote+dir; next=std::clamp(next,24,96); next=(next/12)*12;
        if(next!=ui->kbBaseNote){ ui->kbBaseNote=next; ui->updateKeyboardNotes(); }
    }
    // arpeggiator master switch - persisted via shared "arpon" state
    static void arpBtnCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        lv_obj_t* btn=(lv_obj_t*)lv_event_get_target(e);
        if(!ui||!btn) return;
        bool on=lv_obj_has_state(btn,LV_STATE_CHECKED);
        ui->arpOnLocal=on;
        ui->setState("arpon",on?"1":"0");
    }
    // ---- header zoom control (replaces the old FULLSCREEN path) -----------
    // Window size = base plate * zoom%, so the 1440:860 aspect holds EXACTLY
    // at every step. Content rescale is owned by rebuildForScale(), driven by
    // the real LVGL surface - no double scaling, and repeated toggles are
    // drift-free because sizes are recomputed from BASE_W/BASE_H, never from
    // the current window size.
    void applyZoomStep(int dir){
        const int idx=std::clamp(fZoomIdx+dir,0,lay::ZOOM_STEP_COUNT-1);
        if(idx==fZoomIdx) return;
        fZoomIdx=idx;
        const float z=(float)lay::ZOOM_STEPS[fZoomIdx]/100.f;
        setSize((uint)std::lround(lay::BASE_W*z),(uint)std::lround(lay::BASE_H*z));
        refreshZoomWidgets();
    }
    void refreshZoomWidgets(){
        if(zoomValLbl){ char b[16]; snprintf(b,sizeof(b),"%d%%",lay::ZOOM_STEPS[fZoomIdx]); lv_label_set_text(zoomValLbl,b); }
        if(zoomMinus){ if(fZoomIdx==0) lv_obj_add_state(zoomMinus,LV_STATE_DISABLED); else lv_obj_clear_state(zoomMinus,LV_STATE_DISABLED); }
        if(zoomPlus){ if(fZoomIdx==lay::ZOOM_STEP_COUNT-1) lv_obj_add_state(zoomPlus,LV_STATE_DISABLED); else lv_obj_clear_state(zoomPlus,LV_STATE_DISABLED); }
    }
    static void zoomBtnCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        lv_obj_t* btn=(lv_obj_t*)lv_event_get_target(e);
        if(ui&&btn) ui->applyZoomStep((int)(intptr_t)lv_obj_get_user_data(btn));
    }
    static void rndBtnCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e); if(!ui) return;
        auto rnd=[](float lo,float hi){ return lo + (hi-lo)*((float)std::rand()/RAND_MAX); };
        // every write is automation-bracketed so hosts capture it as a gesture
        auto set=[&](uint32_t p,float v){
            ui->editParameter(p,true); ui->setParamValue(p,v); ui->editParameter(p,false);
        };
        set(PluginMultiScaleBody::kParamDecay, rnd(0.25f,0.85f));
        set(PluginMultiScaleBody::kParamBrightness, rnd(0.35f,0.95f));
        set(PluginMultiScaleBody::kParamRadiation, rnd(0.1f,0.9f));
        set(PluginMultiScaleBody::kParamDetune, rnd(0.f,0.5f));
        set(PluginMultiScaleBody::kParamWet, rnd(0.f,0.6f));
        set(PluginMultiScaleBody::kParamVelStrike, rnd(0.2f,0.8f));
        for(int b=0;b<16;++b)
            set(PluginMultiScaleBody::kParamBand0+b, std::clamp(rnd(0.3f,0.75f),0.f,1.f));
        int mx=modal::kNumPresets-1;
        set(PluginMultiScaleBody::kParamPreset,(float)(std::rand()%(mx+1))/(float)mx);
    }

    // ---- small builder helpers (all sizes flow through scaled()) ----------
    static lv_obj_t* makeBox(lv_obj_t* parent,lv_coord_t w,lv_coord_t h){
        lv_obj_t* c=lv_obj_create(parent);
        lv_obj_set_size(c,w,h);
        lv_obj_set_style_bg_opa(c,LV_OPA_TRANSP,0);
        lv_obj_set_style_border_width(c,0,0);
        lv_obj_set_style_pad_all(c,0,0);
        lv_obj_clear_flag(c,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(c,LV_SCROLLBAR_MODE_OFF);
        return c;
    }
    static lv_obj_t* makeCol(lv_obj_t* parent,lv_coord_t w,lv_coord_t h,lv_coord_t rowGap,lv_flex_align_t mainPlace=LV_FLEX_ALIGN_START){
        lv_obj_t* c=makeBox(parent,w,h);
        lv_obj_set_layout(c,LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(c,LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(c,mainPlace,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_START);
        if(rowGap) lv_obj_set_style_pad_row(c,rowGap,0);
        return c;
    }
    static lv_obj_t* makeRow(lv_obj_t* parent,lv_coord_t w,lv_coord_t h,lv_coord_t colGap,lv_flex_align_t mainPlace=LV_FLEX_ALIGN_START){
        lv_obj_t* c=makeBox(parent,w,h);
        lv_obj_set_layout(c,LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(c,LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(c,mainPlace,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
        if(colGap) lv_obj_set_style_pad_column(c,colGap,0);
        return c;
    }
    // machined plate card: panel surface, hairline border, explicit size
    lv_obj_t* makeCard(lv_obj_t* parent,lv_coord_t w,lv_coord_t h,lv_coord_t rowGap,lv_flex_align_t mainPlace=LV_FLEX_ALIGN_START){
        lv_obj_t* c=makeCol(parent,w,h,rowGap,mainPlace);
        lv_obj_set_style_bg_color(c,PLATE_PANEL,0);
        lv_obj_set_style_bg_opa(c,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(c,PLATE_LINE,0);
        lv_obj_set_style_border_width(c,1,0);
        lv_obj_set_style_radius(c,scaled(lay::RADIUS),0);
        lv_obj_set_style_pad_all(c,scaled(lay::CARD_PAD),0);
        // unified chassis drop: one global light, shadow falls straight down
        lv_obj_set_style_shadow_width(c,scaled(lay::CARD_SHADOW),0);
        lv_obj_set_style_shadow_color(c,COL_BLACK,0);
        lv_obj_set_style_shadow_opa(c,LV_OPA_30,0);
        lv_obj_set_style_shadow_offset_y(c,scaled(lay::SHADOW_OFF_Y),0);
        return c;
    }
    lv_obj_t* addLabel(lv_obj_t* parent,const char* txt,const lv_font_t* font,lv_color_t color,int letterSpace){
        lv_obj_t* l=lv_label_create(parent);
        lv_label_set_text(l,txt);
        lv_obj_set_style_text_font(l,font,0);
        lv_obj_set_style_text_color(l,color,0);
        lv_obj_set_style_text_letter_space(l,(lv_coord_t)letterSpace,0);
        return l;
    }
    lv_obj_t* addDivider(lv_obj_t* parent,lv_coord_t h){
        lv_obj_t* d=makeBox(parent,1,h);
        lv_obj_set_style_bg_color(d,PLATE_LINE,0);
        lv_obj_set_style_bg_opa(d,LV_OPA_COVER,0);
        lv_obj_clear_flag(d,LV_OBJ_FLAG_CLICKABLE);
        return d;
    }
    // engineering spec cell: caption over value (documentation, not marketing)
    lv_obj_t* addSpecCell(lv_obj_t* parent,const char* lab,lv_obj_t** valOut,lv_color_t valCol,int wBase){
        lv_obj_t* cell=makeCol(parent,scaled(wBase),lv_pct(100),scaled(2),LV_FLEX_ALIGN_CENTER);
        // piece-4: spec caption letter-space +2 (was 2 -> 4) so the engineering
        // cells read as a uniform wide-set strip under the body info.
        addLabel(cell,lab,getScaledMicroFont(),PLATE_TEXT_DIM,4);
        lv_obj_t* v=addLabel(cell,"-",getScaledFont(),valCol,0);
        *valOut=v;
        return cell;
    }
    lv_obj_t* addButton(lv_obj_t* parent,lv_coord_t wBase,lv_coord_t hBase,const char* txt,lv_color_t txtCol,bool flatShadow=true){
        lv_obj_t* b=lv_btn_create(parent);
        lv_obj_set_size(b,scaled(wBase),scaled(hBase));
        lv_obj_set_style_bg_color(b,PLATE_WELL,0);
        lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(b,PLATE_EDGE,0);
        if(flatShadow) lv_obj_set_style_shadow_width(b,0,0);
        // feedback ladder: rest PLATE_WELL -> hover lift -> pressed sink
        lv_obj_set_style_bg_color(b,PLATE_WELL_HI,LV_STATE_HOVERED);
        lv_obj_set_style_bg_color(b,PLATE_BTN_PRESS,LV_STATE_PRESSED);
        lv_obj_set_style_translate_y(b,1,LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b,1,0);
        lv_obj_set_style_pad_all(b,0,0);
        lv_obj_t* l=addLabel(b,txt,getScaledMicroFont(),txtCol,1);
        lv_obj_center(l);
        return b;
    }
    // piece-6: preset browser prev/next mini arrow. 1.2em amber chevron in a
    // flat well button. Returns a clickable btn whose user_data carries +1/-1
    // (consumed by presetArrowCb). The dir arg is also passed back as user_data
    // for handler dispatch.
    lv_obj_t* addPresetArrowBtn(lv_obj_t* parent,int dir){
        lv_obj_t* b=lv_btn_create(parent);
        // 24x24 mini button, same height as the dropdown's 22px well
        const int w=24, h=24;
        lv_obj_set_size(b,scaled(w),scaled(h));
        lv_obj_set_style_bg_color(b,PLATE_WELL,0);
        lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(b,PLATE_EDGE,0);
        lv_obj_set_style_border_width(b,1,0);
        lv_obj_set_style_radius(b,scaled(lay::RADIUS_SM),0);
        lv_obj_set_style_shadow_width(b,0,0);
        // hover/pressed feedback ladder
        lv_obj_set_style_bg_color(b,PLATE_WELL_HI,LV_STATE_HOVERED);
        lv_obj_set_style_bg_color(b,PLATE_BTN_PRESS,LV_STATE_PRESSED);
        lv_obj_set_style_translate_y(b,1,LV_STATE_PRESSED);
        lv_obj_set_style_pad_all(b,0,0);
        // 1.2em amber chevron - U+2039 / U+203A SINGLE LEFT/RIGHT-POINTING
        // ANGLE QUOTATION MARK. Dim-amber to keep the one-accent discipline
        // (only the indicator arc + mallet use full amber).
        const char* sym=(dir<0)?"\u2039":"\u203A";
        lv_obj_t* l=addLabel(b,sym,getScaledSmallFont(),PLATE_AMBER_DIM,0);
        lv_obj_set_style_text_letter_space(l,0,0);
        lv_obj_center(l);
        lv_obj_set_user_data(b,(void*)(intptr_t)dir);
        return b;
    }

    // ---- keyboard strip ----------------------------------------------------
    void createKeyboard(lv_obj_t* root){
        // full-width anchor strip pinned under the stage; explicit height
        // (lay::KB_STRIP_H = 8 pad + 22 head + 6 gap + 80 keys + 8 pad + 4 slack)
        kbContainer=makeCol(root,lv_pct(100),scaled(lay::KB_STRIP_H),scaled(6),LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(kbContainer,KB_WELL,0);
        lv_obj_set_style_bg_opa(kbContainer,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(kbContainer,COL_HAIRLINE,0);
        lv_obj_set_style_border_width(kbContainer,1,0);
        lv_obj_set_style_radius(kbContainer,scaled(10),0);
        lv_obj_set_style_pad_all(kbContainer,scaled(8),0);
        // head row: caption left, ARP/octave cluster right (both explicit heights)
        lv_obj_t* kbHead=makeRow(kbContainer,lv_pct(100),scaled(lay::HEAD_H),0,LV_FLEX_ALIGN_SPACE_BETWEEN);
        lv_obj_t* kbTitle=addLabel(kbHead,"KEYBOARD  -  1 OCTAVE  -  CLICK TO AUDITION",getScaledMicroFont(),COL_HIGHLIGHT,2);
        lv_obj_set_style_text_opa(kbTitle,LV_OPA_80,0);
        // cluster width: ARP 46 + 6 + oct 28 + 6 + label 70 + 6 + oct 28 = 190
        lv_obj_t* octRow=makeRow(kbHead,scaled(190),scaled(lay::BTN_H),scaled(6));
        arpBtn=lv_btn_create(octRow);
        lv_obj_set_size(arpBtn,scaled(lay::ARP_W),scaled(lay::BTN_H));
        styles.applyToggleButton(arpBtn,arpOnLocal);
        lv_obj_set_style_radius(arpBtn,scaled(lay::RADIUS_SM),0);
        lv_obj_set_style_pad_all(arpBtn,0,0);
        lv_obj_add_event_cb(arpBtn,arpBtnCb,LV_EVENT_VALUE_CHANGED,this);
        lv_obj_t* albl=lv_label_create(arpBtn); lv_label_set_text(albl,"ARP"); lv_obj_center(albl);
        lv_obj_t* octDown=lv_btn_create(octRow); lv_obj_set_size(octDown,scaled(lay::BTN_W_OCT),scaled(lay::BTN_H)); lv_obj_add_style(octDown,&styles.btnMain,0); lv_obj_add_style(octDown,&styles.btnHovered,LV_STATE_HOVERED); lv_obj_add_style(octDown,&styles.btnPressed,LV_STATE_PRESSED); lv_obj_set_style_radius(octDown,scaled(lay::RADIUS_SM),0); lv_obj_set_style_pad_all(octDown,0,0);
        lv_obj_set_user_data(octDown,(void*)(intptr_t)-12); lv_obj_add_event_cb(octDown,octaveBtnCb,LV_EVENT_CLICKED,this);
        lv_obj_t* dl=lv_label_create(octDown); lv_label_set_text(dl,"<"); lv_obj_center(dl); lv_obj_set_style_text_color(dl,COL_TEXT,0);
        kbOctLabel=lv_label_create(octRow); lv_label_set_text(kbOctLabel,"C4 - B4");
        lv_obj_set_width(kbOctLabel,scaled(lay::OCTLBL_W));
        lv_obj_set_style_text_align(kbOctLabel,LV_TEXT_ALIGN_CENTER,0);
        lv_obj_set_style_text_color(kbOctLabel,COL_TEXT_DIM,0);
        lv_obj_set_style_text_font(kbOctLabel,getScaledMicroFont(),0);
        lv_obj_t* octUp=lv_btn_create(octRow); lv_obj_set_size(octUp,scaled(lay::BTN_W_OCT),scaled(lay::BTN_H)); lv_obj_add_style(octUp,&styles.btnMain,0); lv_obj_add_style(octUp,&styles.btnHovered,LV_STATE_HOVERED); lv_obj_add_style(octUp,&styles.btnPressed,LV_STATE_PRESSED); lv_obj_set_style_radius(octUp,scaled(lay::RADIUS_SM),0); lv_obj_set_style_pad_all(octUp,0,0);
        lv_obj_set_user_data(octUp,(void*)(intptr_t)12); lv_obj_add_event_cb(octUp,octaveBtnCb,LV_EVENT_CLICKED,this);
        lv_obj_t* ul=lv_label_create(octUp); lv_label_set_text(ul,">"); lv_obj_center(ul); lv_obj_set_style_text_color(ul,COL_TEXT,0);
        // keys row centers the fixed 404-wide keybed in whatever width remains
        lv_obj_t* kbRow=makeRow(kbContainer,lv_pct(100),scaled(lay::KEY_H),0,LV_FLEX_ALIGN_CENTER);
        // keybed arithmetic: 7 white x 56 + 6 x 2 gap = 404 wide, 80 tall
        const int whiteW=scaled(lay::KEY_W), whiteH=scaled(lay::KEY_H);
        const int blackW=scaled(lay::KEY_BLACK_W), blackH=scaled(lay::KEY_BLACK_H);
        const int gap=scaled(lay::KEY_GAP);
        const int keysW=7*whiteW+6*gap;
        lv_obj_t* keysBox=makeBox(kbRow,keysW,whiteH);
        lv_obj_set_layout(keysBox,LV_LAYOUT_NONE);
        static const char* whiteName[7]={"C","D","E","F","G","A","B"}; static const int whiteOff[7]={0,2,4,5,7,9,11}; static const int blackOff[5]={1,3,6,8,10};
        int bX[5]; int stepW=whiteW+gap; bX[0]=stepW*1-blackW/2; bX[1]=stepW*2-blackW/2; bX[2]=stepW*4-blackW/2; bX[3]=stepW*5-blackW/2; bX[4]=stepW*6-blackW/2;
        for(int i=0;i<7;++i){
            lv_obj_t* w=lv_btn_create(keysBox); lv_obj_set_size(w,whiteW,whiteH); lv_obj_set_pos(w,i*stepW,0);
            lv_obj_set_style_bg_color(w,COL_KNOB,0); lv_obj_set_style_bg_color(w,COL_KNOB_LIGHT,LV_STATE_HOVERED); lv_obj_set_style_bg_opa(w,LV_OPA_COVER,0); lv_obj_set_style_border_color(w,COL_HAIRLINE,0); lv_obj_set_style_border_width(w,1,0); lv_obj_set_style_radius(w,scaled(5),0); lv_obj_set_style_pad_all(w,0,0);
            int note=kbBaseNote+whiteOff[i]; lv_obj_set_user_data(w,(void*)(intptr_t)note);
            lv_obj_add_event_cb(w,keyEventCb,LV_EVENT_PRESSED,this); lv_obj_add_event_cb(w,keyEventCb,LV_EVENT_RELEASED,this); lv_obj_add_event_cb(w,keyEventCb,LV_EVENT_PRESS_LOST,this); lv_obj_add_event_cb(w,keyEventCb,LV_EVENT_LEAVE,this);
            kbWhite[i]=w; lv_obj_t* lbl=lv_label_create(w); int oct=note/12-1; char buf[8]; snprintf(buf,sizeof(buf),"%s%d",whiteName[i],oct); lv_label_set_text(lbl,buf);
            lv_obj_add_style(lbl,&styles.labelSmall,0); lv_obj_set_style_text_color(lbl,COL_PANEL_DARK,0); lv_obj_set_style_text_font(lbl,getScaledMicroFont(),0); lv_obj_align(lbl,LV_ALIGN_BOTTOM_MID,0,-scaled(4)); lv_obj_clear_flag(lbl,LV_OBJ_FLAG_CLICKABLE);
        }
        for(int i=0;i<5;++i){
            lv_obj_t* b=lv_btn_create(keysBox); lv_obj_set_size(b,blackW,blackH); lv_obj_set_pos(b,bX[i],0);
            lv_obj_set_style_bg_color(b,KB_BLACK,0); lv_obj_set_style_bg_color(b,KB_BLACK_HI,LV_STATE_HOVERED); lv_obj_set_style_bg_grad_color(b,KB_BLACK_HI,0); lv_obj_set_style_bg_grad_dir(b,LV_GRAD_DIR_VER,0); lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0);
            lv_obj_set_style_border_color(b,COL_HAIRLINE,0); lv_obj_set_style_border_width(b,1,0); lv_obj_set_style_radius(b,scaled(lay::RADIUS_SM),0); lv_obj_set_style_pad_all(b,0,0);
            lv_obj_set_style_shadow_width(b,scaled(4),0); lv_obj_set_style_shadow_color(b,COL_BLACK,0); lv_obj_set_style_shadow_opa(b,LV_OPA_30,0);
            int note=kbBaseNote+blackOff[i]; lv_obj_set_user_data(b,(void*)(intptr_t)note);
            lv_obj_add_event_cb(b,keyEventCb,LV_EVENT_PRESSED,this); lv_obj_add_event_cb(b,keyEventCb,LV_EVENT_RELEASED,this); lv_obj_add_event_cb(b,keyEventCb,LV_EVENT_PRESS_LOST,this); lv_obj_add_event_cb(b,keyEventCb,LV_EVENT_LEAVE,this);
            kbBlack[i]=b;
        }
        updateKeyboardNotes();
    }

    // ==== BUILD =============================================================
    // Layout hierarchy (matches Serum 2 main-view grammar, paper-faithful):
    //   ROOT  (PLATE_BG)
    //   +-- TOP-BAR  (identity: brand mark | preset browser | master knob | zoom)
    //   +-- NAV-STRIP  (section chips + paper identity, single horizontal rule)
    //   +-- MACRO-RACK  (8 quick-access slots: the dial bank's most-touched params)
    //   +-- STAGE
    //   |   +-- LEFT  (4 dial groups: BODY/RESONATE/EXCITER/SPACE)
    //   |   +-- CENTER  (hero strike disc + preset row + spec strip)
    //   |   +-- RIGHT  (spectrum card + scope card)
    //   +-- KEYBOARD  (octave + keys + ARP)
    // vertical budget @s=1: 16+72+6+28+6+72+6+504+6+128+16 = 860 (exact).
    void buildUI(){
        lv_obj_t* root=lv_screen_active();
        if(!root){ lv_display_t* d=lv_display_get_default(); if(d) root=lv_display_get_screen_active(d); }
        if(!root) return;
        fUIBuilt=true; fMarkerPlaced=false;

        // --- ROOT COLUMN -----------------------------------------------------
        // SPACE_BETWEEN absorbs sub-threshold resize drift into the row gaps
        // instead of clipping the keyboard bottom before the rescale rebuild fires.
        lv_obj_set_style_bg_color(root,PLATE_BG,0); lv_obj_set_style_bg_opa(root,LV_OPA_COVER,0);
        lv_obj_set_layout(root,LV_LAYOUT_FLEX); lv_obj_set_flex_flow(root,LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(root,LV_FLEX_ALIGN_SPACE_BETWEEN,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(root,scaled(lay::PAD),0);
        lv_obj_set_scrollbar_mode(root,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(root,LV_OBJ_FLAG_SCROLLABLE);

        // --- TOP-BAR (h = 72): brand | preset browser | master | zoom --------
        // Same role Serum 2 fills with SERUM 2 / preset / MASTER / MENU.
        // Horizontal split:  brand(220) | preset(0,grow) | master(140) | zoom(116).
        lv_obj_t* topbar=makeRow(root,lv_pct(100),scaled(lay::HEADER_H),scaled(10),LV_FLEX_ALIGN_START);
        lv_obj_set_style_bg_color(topbar,PLATE_PANEL,0); lv_obj_set_style_bg_opa(topbar,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(topbar,PLATE_LINE,0); lv_obj_set_style_border_width(topbar,1,0);
        lv_obj_set_style_radius(topbar,scaled(lay::RADIUS),0);
        lv_obj_set_style_pad_hor(topbar,scaled(16),0); lv_obj_set_style_pad_ver(topbar,scaled(10),0);
        // piece-4: title bumped +2px (24 -> 26) via the new getDisplayFont26().
        // Letter-space +2 (was 4) keeps the headline tight; the authors line
        // drops to 50% opacity + 1px smaller so it recedes and lets the title own the bar.
        // brand mark - bold, monospaced-feel, two-line: product | class
        lv_obj_t* brandCol=makeCol(topbar,scaled(240),lv_pct(100),scaled(2));
        lv_obj_t* titleLbl=addLabel(brandCol,"MULTI-SCALE BODY",gUIScale>=1.2f?getDisplayFont26():getDisplayFont(),PLATE_TITLE,2);
        lv_obj_set_style_text_letter_space(titleLbl,4,0);
        lv_obj_t* authorsLbl=addLabel(brandCol,"MODAL SYNTH   -   DAFX-09 / 47",getScaledMicroFont(),PLATE_TEXT_DIM,1);
        lv_obj_set_style_text_opa(authorsLbl,LV_OPA_50,0);
        lv_obj_set_style_text_letter_space(authorsLbl,1,0);
        // preset browser - move from the center to the top bar so the hero gets air
        lv_obj_t* presetBar=makeCol(topbar,0,lv_pct(100),scaled(3));
        lv_obj_set_flex_grow(presetBar,1);
        lv_obj_set_style_pad_hor(presetBar,scaled(10),0);
        lv_obj_set_flex_align(presetBar,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START);
        addLabel(presetBar,"PRESET   -   BAKED MODAL PRESETS",getScaledMicroFont(),PLATE_TEXT_DIM,2);
        // piece-6: preset browser now has prev/next mini arrows around the
        // dropdown. The dropdown itself carries a small caret (LV_PART_INDICATOR)
        // styled as a chevron. Layout: [<] [dropdown  -  caret  -  NAME] [>].
        // Wrap the dropdown in a row so the arrows flank it on both sides at
        // the same height. The arrows are simple text buttons using Unicode
        // chevrons (U+2039 / U+203A) in the amber dim palette.
        lv_obj_t* ddRow=makeRow(presetBar,lv_pct(100),0,scaled(6),LV_FLEX_ALIGN_CENTER);
        lv_obj_set_flex_grow(ddRow,1);
        // prev arrow
        presetPrevBtn=addPresetArrowBtn(ddRow,-1);
        // dropdown moved to the top bar (was the center card's preset row)
        presetDropdown=lv_dropdown_create(ddRow);
        {
            std::string opts; for(int i=0;i<modal::kNumPresets;++i){ if(i) opts+="\n"; opts+=modal::kPresets[i].name; }
            lv_dropdown_set_options(presetDropdown,opts.c_str());
        }
        int mxp=modal::kNumPresets-1;
        int selp=(int)std::round(paramCache[PluginMultiScaleBody::kParamPreset]*(float)mxp);
        lv_dropdown_set_selected(presetDropdown,std::clamp(selp,0,mxp));
        lv_obj_set_flex_grow(presetDropdown,1);
        lv_obj_add_style(presetDropdown,&styles.compactSelectMain,0);
        lv_obj_set_style_bg_color(presetDropdown,PLATE_WELL,0);
        lv_obj_set_style_border_color(presetDropdown,PLATE_EDGE,0);
        lv_obj_set_style_radius(presetDropdown,scaled(lay::RADIUS_SM),0);
        // piece-6: dropdown caret - style the indicator (the built-in arrow on
        // the right edge) as a 1.2em amber chevron. The default is a generic
        // downward triangle; we replace it with the U+25BE BLACK DOWN-POINTING
        // SMALL TRIANGLE rendered in PLATE_AMBER so the dropdown clearly reads
        // as a browser selector, not a generic field.
        {
            lv_obj_t* list=lv_dropdown_get_list(presetDropdown);
            if(list){
                lv_obj_add_style(list,&styles.compactSelectListMain,0);
                lv_obj_set_style_max_height(list,scaled(lay::DROPDOWN_MAX_ROWS*lay::DROPDOWN_ROW_H),0);
            }
        }
        // next arrow
        presetNextBtn=addPresetArrowBtn(ddRow,+1);
        // piece-6: dropdown group/keyboard handling. Dropdown must NOT be in
        // the group (wheel = encoder; group focus defocuses + closes).
        lv_group_remove_obj(presetDropdown);
        lv_obj_add_event_cb(presetDropdown,dropdownCb,LV_EVENT_VALUE_CHANGED,this);
        // wire the prev/next arrows: click cycles the preset by 1
        if(presetPrevBtn) lv_obj_add_event_cb(presetPrevBtn,presetArrowCb,LV_EVENT_CLICKED,this);
        if(presetNextBtn) lv_obj_add_event_cb(presetNextBtn,presetArrowCb,LV_EVENT_CLICKED,this);
        // master knob - synthesized for piece-1 (paper's modal energy level),
        // small 56px arc, the "MAIN" knob analogue on the right of the brand
        lv_obj_t* masterCol=makeCol(topbar,scaled(140),lv_pct(100),scaled(2));
        lv_obj_set_flex_align(masterCol,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
        addLabel(masterCol,"MASTER",getScaledMicroFont(),PLATE_LABEL_ACCENT,3);
        {
            ArcVisualSpec mSpec=normalArcSpec();
            lv_obj_t* masterArc=UIWidgets::createArcKnob(masterCol,PluginMultiScaleBody::kParamWet,this,styles,mSpec);
            regExtraWidget(PluginMultiScaleBody::kParamWet, masterArc);
            lv_obj_add_event_cb(masterArc,valueFormatCb,LV_EVENT_ALL,this);
            {
                lv_obj_t* cont=lv_obj_get_parent(masterArc);
                lv_obj_t* lbl=cont?lv_obj_get_child(cont,lv_obj_get_child_count(cont)-1):nullptr;
                if(lbl&&lv_obj_check_type(lbl,&lv_label_class)){
                    char b[24];
                    formatParamValue(PluginMultiScaleBody::kParamWet,paramCache[PluginMultiScaleBody::kParamWet],b,sizeof(b));
                    lv_label_set_text(lbl,b);
                }
            }
        }
        // zoom stepper - the rightmost cluster, vertical divider before it
        addDivider(topbar,scaled(lay::HEADER_H-20));
        const int clusterW=lay::ZOOM_BTN*2+lay::ZOOM_LBL_W+12;
        lv_obj_t* zoomRow=makeRow(topbar,scaled(clusterW),scaled(lay::ZOOM_LBL_H),scaled(4),LV_FLEX_ALIGN_CENTER);
        zoomMinus=addButton(zoomRow,lay::ZOOM_BTN,lay::ZOOM_LBL_H,"-",PLATE_TEXT_MID);
        lv_obj_set_user_data(zoomMinus,(void*)(intptr_t)-1);
        lv_obj_add_event_cb(zoomMinus,zoomBtnCb,LV_EVENT_CLICKED,this);
        zoomValLbl=addLabel(zoomRow,"100%",getScaledSmallFont(),PLATE_AMBER,1);
        lv_obj_set_width(zoomValLbl,scaled(lay::ZOOM_LBL_W));
        lv_obj_set_style_text_align(zoomValLbl,LV_TEXT_ALIGN_CENTER,0);
        zoomPlus=addButton(zoomRow,lay::ZOOM_BTN,lay::ZOOM_LBL_H,"+",PLATE_TEXT_MID);
        lv_obj_set_user_data(zoomPlus,(void*)(intptr_t)1);
        lv_obj_add_event_cb(zoomPlus,zoomBtnCb,LV_EVENT_CLICKED,this);
        // --- NAV-STRIP (h = NAV_H + MACRO_LED_H = 40) -----------------------
        // Round-2: the section chips + paper identity live in a top flex row,
        // and an 8-slot LED status row hangs below (replaces the old 72px
        // MACRO-RACK of duplicate knobs). Net -60px; flows into the stage so
        // the disc can keep its 280px diameter with breathing room. Each LED
        // lights AMBER when its kMacroParams[m] is non-default - the param
        // value still lives in the dial bank and is editable there.
        // outer = makeCol so chips on top, LEDs on bottom share one panel/border
        lv_obj_t* nav=makeCol(root,lv_pct(100),scaled(lay::NAV_H+lay::MACRO_LED_H+6),0);
        lv_obj_set_style_bg_color(nav,PLATE_PANEL,0); lv_obj_set_style_bg_opa(nav,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(nav,PLATE_LINE,0); lv_obj_set_style_border_width(nav,1,0);
        lv_obj_set_style_radius(nav,scaled(lay::RADIUS),0);
        // navTop: chips + paper identity
        lv_obj_t* navTop=makeRow(nav,lv_pct(100),scaled(lay::NAV_H),0,LV_FLEX_ALIGN_SPACE_BETWEEN);
        lv_obj_set_style_pad_hor(navTop,scaled(8),0); lv_obj_set_style_pad_ver(navTop,scaled(3),0);
        // left chip cluster
        lv_obj_t* navChips=makeRow(navTop,0,scaled(lay::NAV_H-6),scaled(lay::NAV_CHIP_GAP),LV_FLEX_ALIGN_CENTER);
        lv_obj_set_flex_grow(navChips,1);
        static const char* navLabels[5]={"BODY","RESONATE","EXCITER","SPACE","MOD"};
        for(int n=0;n<5;++n){
            const bool active=(n==0);   // piece-1: BODY is the visual lead
            lv_obj_t* chip=makeBox(navChips,scaled(lay::NAV_CHIP_W),scaled(lay::NAV_H-6));
            lv_obj_set_layout(chip,LV_LAYOUT_FLEX);
            lv_obj_set_flex_flow(chip,LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(chip,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
            if(active){
                // BODY chip: amber border + slight amber wash so it reads as
                // the active section (mirrors the brighter BODY group label
                // in the dial bank - one visual lead, two echo points)
                lv_obj_set_style_bg_color(chip,COL_HIGHLIGHT,0);
                lv_obj_set_style_bg_opa(chip,LV_OPA_20,0);
                lv_obj_set_style_border_color(chip,COL_HIGHLIGHT,0);
                lv_obj_set_style_border_opa(chip,LV_OPA_80,0);
            } else {
                lv_obj_set_style_bg_color(chip,PLATE_WELL,0);
                lv_obj_set_style_bg_opa(chip,LV_OPA_60,0);
                lv_obj_set_style_border_color(chip,PLATE_LINE,0);
            }
            lv_obj_set_style_border_width(chip,1,0);
            lv_obj_set_style_radius(chip,scaled(lay::RADIUS_SM),0);
            lv_obj_clear_flag(chip,LV_OBJ_FLAG_SCROLLABLE);
            addLabel(chip,navLabels[n],getScaledMicroFont(),active?PLATE_AMBER:PLATE_TEXT_DIM,2);
        }
        // right paper identity - width-capped to guarantee no overflow at 1440
        lv_obj_t* navRight=makeRow(navTop,scaled(180),scaled(lay::NAV_H-6),0,LV_FLEX_ALIGN_END);
        addLabel(navRight,"DAFX-09  /  PAPER 47",getScaledMicroFont(),PLATE_TEXT_DIM,1);
        // navBottom: 8-slot LED macro strip - thin status row, each dot lit
        // when its kMacroParams[m] is non-default. Sync from parameterChanged
        // updates on every value change so the LEDs stay live.
        lv_obj_t* navLedRow=makeRow(nav,lv_pct(100),scaled(lay::MACRO_LED_H),scaled(lay::MACRO_LED_GAP),LV_FLEX_ALIGN_SPACE_BETWEEN);
        lv_obj_set_style_pad_hor(navLedRow,scaled(8),0); lv_obj_set_style_pad_ver(navLedRow,0,0);
        for(int m=0;m<8;++m){
            lv_obj_t* cell=makeRow(navLedRow,scaled(lay::MACRO_LED_CELL_W),scaled(lay::MACRO_LED_H),scaled(4),LV_FLEX_ALIGN_START);
            // M<n> caption (subtle)
            char num[8]; snprintf(num,sizeof(num),"M%d",m+1);
            addLabel(cell,num,getScaledMicroFont(),PLATE_TEXT_DIM,0);
            // the dot itself - dim when default, amber when non-default
            lv_obj_t* dot=makeBox(cell,scaled(lay::MACRO_LED_DOT),scaled(lay::MACRO_LED_DOT));
            lv_obj_set_style_radius(dot,LV_RADIUS_CIRCLE,0);
            lv_obj_set_style_bg_opa(dot,LV_OPA_COVER,0);
            macroLedDots[m]=dot;
            updateMacroLed(m);   // initial state from current paramCache
            // trailing label - the macro's param name in micro font so the LED
            // row reads as "M1 TUNE    [.]" instead of an anonymous strip
            addLabel(cell,parameterName(kMacroParams[m]).c_str(),getScaledMicroFont(),PLATE_TEXT_DIM,0);
        }   // end macros-as-LEDs
        // --- STAGE ROW (h = 504): dial bank | hero plate | analysis tower -----
        lv_obj_t* stage=makeRow(root,lv_pct(100),scaled(lay::STAGE_H),scaled(lay::GUTTER));
        // LEFT - FORGE: four labeled knob clusters, spread over the full column
        // heights: BODY 16+6+116=138, others 16+6+98=120; SPACE_BETWEEN spreads
        // the leftover 118 across three inter-cluster gaps (~39) - deliberate air
        lv_obj_t* left=makeCol(stage,scaled(lay::LEFT_W),scaled(lay::STAGE_H),0,LV_FLEX_ALIGN_SPACE_BETWEEN);
        const uint32_t groupParams[4][4]={
            {PluginMultiScaleBody::kParamPitch,PluginMultiScaleBody::kParamDecay,PluginMultiScaleBody::kParamBrightness,PluginMultiScaleBody::kParamModeCount},
            {PluginMultiScaleBody::kParamWidth,PluginMultiScaleBody::kParamRadiation,PluginMultiScaleBody::kParamDetune,PluginMultiScaleBody::kParamGlide},
            {PluginMultiScaleBody::kParamVelStrike,PluginMultiScaleBody::kParamExciteMix,PluginMultiScaleBody::kParamAttack,PluginMultiScaleBody::kParamRelease},
            {PluginMultiScaleBody::kParamLFORate,PluginMultiScaleBody::kParamLFODepth,PluginMultiScaleBody::kParamWet,PluginMultiScaleBody::kParamMono}
        };
        const char* groupNames[4]={"BODY","RESONATE","EXCITER","SPACE"};
        for(int g=0;g<4;++g){
            const bool primary=(g==0);
            const int kh=primary?lay::KNOB_H_N:lay::KNOB_H_C;
            lv_obj_t* sec=makeCol(left,scaled(lay::LEFT_W),scaled(lay::SEC_LABEL_H+lay::SEC_GAP+kh),scaled(lay::SEC_GAP));
            lv_obj_set_flex_align(sec,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START);
            // piece-4: section captions letter-space +2 (was 3) -> consistent
            // wide-set feel with the title and spec-strip cells. The active
            // BODY row keeps the accent color so it still reads as the lead.
            addLabel(sec,groupNames[g],getScaledMicroFont(),primary?PLATE_LABEL_ACCENT:PLATE_TEXT_DIM,5);
            // knob grid: explicit single-row width so nothing ever wraps
            lv_obj_t* grid=makeRow(sec,scaled(lay::LEFT_W),scaled(kh),scaled(lay::GRID_GUT_X));
            // size hierarchy: BODY runs full machined size, secondary groups compact
            ArcVisualSpec groupSpec=normalArcSpec();
            if(!primary){ groupSpec.containerW=scaled(lay::KNOB_W_C); groupSpec.containerH=scaled(lay::KNOB_H_C); groupSpec.arcSize=scaled(lay::KNOB_ARC_C); }
            for(int k=0;k<4;++k){
                lv_obj_t* arc=UIWidgets::createArcKnob(grid,groupParams[g][k],this,styles,groupSpec);
                // runs AFTER UIWidgets' own handlers (insertion order) so the
                // contextual unit formatting wins over their raw %.2f writes
                lv_obj_add_event_cb(arc,valueFormatCb,LV_EVENT_ALL,this);
                widgets[groupParams[g][k]]=arc;
                // initial paint: the label was born with raw %.2f and no event
                // has fired yet - apply the contextual format once now
                {
                    lv_obj_t* cont=lv_obj_get_parent(arc);
                    lv_obj_t* lbl=cont?lv_obj_get_child(cont,lv_obj_get_child_count(cont)-1):nullptr;
                    if(lbl&&lv_obj_check_type(lbl,&lv_label_class)){
                        char b[24];
                        formatParamValue(groupParams[g][k],paramCache[groupParams[g][k]],b,sizeof(b));
                        lv_label_set_text(lbl,b);
                    }
                }
            }
        }   // end of the four knob groups
        // === CENTER - HERO (round-2 re-anchor) =============================
        // Body-of-the-control panel: disc on the LEFT, body-info / spec-strip
        // cluster on the RIGHT (was a vertical stack below the disc in r1).
        // The disc shrinks to 280px (was 406) so it stops dominating the
        // stage as a dark empty area; the freed column width flows to the
        // spectrum panel (516 instead of 566) - actually, the freed column
        // is the CENTER being wider (480), so the right column stays the
        // same; the disc gets shorter, the body-info gets more vertical air
        // beside it. Net result: the eye lands on BODY (dial bank) first,
        // then the playable disc, then spectrum, then keyboard.
        lv_obj_t* center=makeRow(stage,scaled(lay::CENTER_W),scaled(lay::STAGE_H),scaled(lay::GUTTER));
        // disc column: 280px wide, fills the column height (top-aligned)
        lv_obj_t* discCol=makeCol(center,scaled(lay::DISC_D),scaled(lay::STAGE_H),scaled(6),LV_FLEX_ALIGN_START);
        // info column: takes the remaining width (480-280-10=190px)
        lv_obj_t* infoCol=makeCol(center,0,scaled(lay::STAGE_H),scaled(6),LV_FLEX_ALIGN_START);
        lv_obj_set_flex_grow(infoCol,1);
        // ---- disc col head + disc ----
        lv_obj_t* cHead=makeRow(discCol,scaled(lay::DISC_D),scaled(lay::HEAD_H),0,LV_FLEX_ALIGN_SPACE_BETWEEN);
        addLabel(cHead,"STRIKE THE BODY",getScaledSmallFont(),COL_HIGHLIGHT,2);
        addLabel(cHead,"CLICK",getScaledMicroFont(),PLATE_TEXT_DIM,1);
        // The disc - top view of the resonant body (hero element)
        const lv_coord_t D=scaled(lay::DISC_D);
        strikeDisc=makeBox(discCol,D,D);
        lv_obj_set_layout(strikeDisc,LV_LAYOUT_NONE);
        lv_obj_set_style_radius(strikeDisc,LV_RADIUS_CIRCLE,0);
        lv_obj_set_style_bg_color(strikeDisc,PLATE_WELL,0);
        lv_obj_set_style_bg_grad_color(strikeDisc,PLATE_WELL_HI,0);
        lv_obj_set_style_bg_grad_dir(strikeDisc,LV_GRAD_DIR_VER,0);
        lv_obj_set_style_bg_opa(strikeDisc,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(strikeDisc,PLATE_EDGE,0);
        lv_obj_set_style_border_width(strikeDisc,1,0);
        lv_obj_set_style_border_opa(strikeDisc,70,0);
        lv_obj_set_style_shadow_width(strikeDisc,scaled(18),0);
        lv_obj_set_style_shadow_color(strikeDisc,COL_BLACK,0);
        lv_obj_set_style_shadow_opa(strikeDisc,LV_OPA_50,0);
        lv_obj_set_style_shadow_offset_y(strikeDisc,scaled(lay::SHADOW_OFF_Y),0);
        lv_obj_add_flag(strikeDisc,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(strikeDisc,padPressCb,LV_EVENT_PRESSED,this);
        lv_obj_add_event_cb(strikeDisc,padPressCb,LV_EVENT_PRESSING,this);
        lv_obj_add_event_cb(strikeDisc,padPressCb,LV_EVENT_RELEASED,this);
        lv_obj_add_event_cb(strikeDisc,padPressCb,LV_EVENT_PRESS_LOST,this);
        // inner well: a smaller circle (80% of disc) with reversed gradient -
        // cheap radial depth (machined dish) without LVGL complex gradients
        lv_coord_t wd=(lv_coord_t)(D*0.80f);
        lv_obj_t* well=makeBox(strikeDisc,wd,wd);
        lv_obj_align(well,LV_ALIGN_CENTER,0,scaled(2));
        lv_obj_set_style_radius(well,LV_RADIUS_CIRCLE,0);
        lv_obj_set_style_bg_color(well,PLATE_WELL_HI,0);
        lv_obj_set_style_bg_grad_color(well,PLATE_WELL,0);
        lv_obj_set_style_bg_grad_dir(well,LV_GRAD_DIR_VER,0);
        lv_obj_set_style_bg_opa(well,LV_OPA_80,0);
        lv_obj_set_style_border_color(well,PLATE_LINE,0);
        lv_obj_set_style_border_width(well,1,0);
        lv_obj_set_style_border_opa(well,60,0);
        // decorative overlays must never eat disc clicks: LVGL delivers PRESSED
        // to the topmost clickable object under the point (lv_indev_search_obj),
        // and plain lv_obj children are clickable BY DEFAULT (lv_obj ctor).
        lv_obj_clear_flag(well,LV_OBJ_FLAG_CLICKABLE);
        // === ROUND-2: TWO AMBER GUIDE RINGS (visual hero information) =======
        // r1 only had 3 dim grey concentric rings that read as decoration.
        // r2 keeps 1 dim outer witness + adds 2 amber hairlines (center zone
        // + rim zone) so the disc carries real information: the soft inner
        // ring marks the body's central sweet spot, the hard outer ring
        // marks the rim - the difference is the modal-density shift.
        {
            const int ringDiam[3]={
                (int)(D*0.92f),                                 // outer witness (grey)
                (int)((lay::DISC_RING_SOFT*2*D)/100),          // soft zone (amber)
                (int)((lay::DISC_RING_HARD*2*D)/100),          // hard zone (amber)
            };
            const lv_color_t ringCol[3]={ PLATE_LINE, COL_HIGHLIGHT, COL_HIGHLIGHT };
            const int ringW[3]={ 1, 1, 1 };
            const int ringOpa[3]={ 80, 100, 60 };
            for(int r=0;r<3;++r){
                lv_obj_t* ring=lv_obj_create(strikeDisc);
                lv_obj_set_size(ring,(lv_coord_t)ringDiam[r],(lv_coord_t)ringDiam[r]);
                lv_obj_align(ring,LV_ALIGN_CENTER,0,0);
                lv_obj_set_style_radius(ring,LV_RADIUS_CIRCLE,0);
                lv_obj_set_style_bg_opa(ring,LV_OPA_TRANSP,0);
                lv_obj_set_style_border_color(ring,ringCol[r],0);
                lv_obj_set_style_border_width(ring,ringW[r],0);
                lv_obj_set_style_border_opa(ring,(lv_opa_t)ringOpa[r],0);
                lv_obj_set_style_pad_all(ring,0,0);
                lv_obj_clear_flag(ring,LV_OBJ_FLAG_CLICKABLE); lv_obj_clear_flag(ring,LV_OBJ_FLAG_SCROLLABLE);
            }
        }
        // crosshair (kept - it was already there and is a real "playable
        // surface" affordance)
        lv_obj_t* chH=makeBox(strikeDisc,D,1); lv_obj_set_pos(chH,0,D/2);
        lv_obj_set_style_bg_color(chH,PLATE_LINE,0); lv_obj_set_style_bg_opa(chH,LV_OPA_40,0);
        lv_obj_set_style_radius(chH,0,0);
        lv_obj_clear_flag(chH,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* chV=makeBox(strikeDisc,1,D); lv_obj_set_pos(chV,D/2,0);
        lv_obj_set_style_bg_color(chV,PLATE_LINE,0); lv_obj_set_style_bg_opa(chV,LV_OPA_40,0);
        lv_obj_set_style_radius(chV,0,0);
        lv_obj_clear_flag(chV,LV_OBJ_FLAG_CLICKABLE);
        // 8-way cardinal witness marks (more directions than r1's 4) - small
        // ticks on the rim help read the strike position against the rings
        for(int t=0;t<8;++t){
            const float ang = t * (2.f*(float)M_PI/8.f);
            const int rOuter = D/2 - scaled(2);
            const int cx = D/2 + (int)(std::cos(ang)*rOuter) - 1;
            const int cy = D/2 + (int)(std::sin(ang)*rOuter) - scaled(4);
            lv_obj_t* mk=makeBox(strikeDisc,2,scaled(8));
            lv_obj_set_pos(mk,cx,cy);
            lv_obj_set_style_transform_pivot_x(mk,1,0);
            lv_obj_set_style_transform_pivot_y(mk,scaled(4),0);
            // rotate each tick so it points outward from center
            const int deg = (int)((ang*180.f/(float)M_PI) + 90.f) * 10;
            lv_obj_set_style_transform_angle(mk,deg,0);
            lv_obj_set_style_bg_color(mk,PLATE_MARK,0); lv_obj_set_style_bg_opa(mk,LV_OPA_COVER,0);
            lv_obj_set_style_radius(mk,0,0);
            lv_obj_clear_flag(mk,LV_OBJ_FLAG_CLICKABLE);
        }
        // the mallet marker (dynamic - follows the param)
        strikeDot=makeBox(strikeDisc,scaled(12),scaled(12));
        lv_obj_set_style_bg_color(strikeDot,COL_HIGHLIGHT,0); lv_obj_set_style_bg_opa(strikeDot,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(strikeDot,PLATE_AMBER_PALE,0); lv_obj_set_style_border_width(strikeDot,1,0);
        lv_obj_set_style_radius(strikeDot,LV_RADIUS_CIRCLE,0);
        lv_obj_set_style_shadow_width(strikeDot,scaled(14),0);
        lv_obj_set_style_shadow_color(strikeDot,COL_HIGHLIGHT,0);
        lv_obj_clear_flag(strikeDot,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_shadow_opa(strikeDot,LV_OPA_60,0);
        lv_obj_set_pos(strikeDot,(int)(paramCache[PluginMultiScaleBody::kParamStrikeX]*(D-scaled(12))),
                                (int)((1.f-paramCache[PluginMultiScaleBody::kParamStrikeY])*(D-scaled(12))));
        // === INFO COL (right of the disc) ================================
        // The body-info cluster spread to the right of the disc instead of
        // below - this is the r1->r2 anchor shift that puts the disc in
        // dialog with the body it represents, not stacked over a cramped 3-label
        // caption row. Also hosts the coord readout.
        // section header
        lv_obj_t* infoHead=makeRow(infoCol,lv_pct(100),scaled(lay::HEAD_H),0,LV_FLEX_ALIGN_START);
        addLabel(infoHead,"BODY",getScaledSmallFont(),PLATE_LABEL_ACCENT,2);
        addLabel(infoHead,"   -   PRESET / MATERIAL / MODES",getScaledMicroFont(),PLATE_TEXT_DIM,0);
        // coordinate readout (the live "X 0.50  Y 0.50" line - now prominent)
        lv_obj_t* coordWrap=makeRow(infoCol,lv_pct(100),scaled(lay::COORD_H),0);
        strikeCoordLabel=addLabel(coordWrap,"X 0.50  -  Y 0.50",getScaledSmallFont(),PLATE_AMBER,1);
        // divider hairline
        addDivider(infoCol,scaled(1));
        // body-info row: material jewel (preview) + spec line
        lv_obj_t* infoRow=makeRow(infoCol,lv_pct(100),scaled(lay::PRESET_ROW_H),scaled(8));
        // builder side of the shared preview geometry (painter: previewGeometry())
        bodyPreview=makeBox(infoRow,scaled(lay::PREVIEW_BOX),scaled(lay::PREVIEW_BOX));
        lv_obj_set_style_bg_color(bodyPreview,PLATE_PREVIEW_BG,0); lv_obj_set_style_bg_opa(bodyPreview,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(bodyPreview,PLATE_EDGE,0); lv_obj_set_style_border_width(bodyPreview,1,0);
        lv_obj_set_style_radius(bodyPreview,scaled(8),0);
        lv_obj_set_style_pad_all(bodyPreview,scaled(lay::PREVIEW_PAD),0);
        lv_obj_set_layout(bodyPreview,LV_LAYOUT_NONE);
        lv_obj_t* infoTextCol=makeCol(infoRow,0,scaled(lay::PRESET_ROW_H),scaled(2));
        lv_obj_set_flex_grow(infoTextCol,1);
        lv_obj_set_flex_align(infoTextCol,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START);
        bodySubLabel=lv_label_create(infoTextCol); lv_label_set_text(bodySubLabel,"");
        lv_obj_set_style_text_font(bodySubLabel,getScaledSmallFont(),0);
        lv_obj_set_style_text_color(bodySubLabel,PLATE_TEXT,0);
        lv_label_set_long_mode(bodySubLabel,LV_LABEL_LONG_WRAP);
        lv_obj_set_width(bodySubLabel,lv_pct(100));
        addLabel(infoTextCol,"MATERIAL JEWEL   -   OCCUPANCY MAP",getScaledMicroFont(),PLATE_TEXT_DIM,1);
        // divider hairline
        addDivider(infoCol,scaled(1));
        // spec strip: BODY / MATERIAL / MODES / F0 in a 2x2 grid so it
        // fits the narrow 190px info column without the r1 truncated captions
        lv_obj_t* specStrip=makeCol(infoCol,lv_pct(100),0,scaled(2));
        lv_obj_t* specRow1=makeRow(specStrip,lv_pct(100),scaled(16),0,LV_FLEX_ALIGN_SPACE_BETWEEN);
        addSpecCell(specRow1,"BODY",&hdrBodyVal,PLATE_AMBER,80);
        addSpecCell(specRow1,"MODES",&hdrModeVal,PLATE_TEXT,56);
        lv_obj_t* specRow2=makeRow(specStrip,lv_pct(100),scaled(16),0,LV_FLEX_ALIGN_SPACE_BETWEEN);
        addSpecCell(specRow2,"MAT",&hdrMatVal,PLATE_TEXT,80);
        addSpecCell(specRow2,"F0",&hdrF0Val,PLATE_AMBER,56);


        // RIGHT - ANALYSIS TOWER: spectrum card 370 + gutter 10 + scope card 236 = 616 exact
        lv_obj_t* right=makeCol(stage,0,scaled(lay::STAGE_H),scaled(lay::GUTTER)); // explicit-height parent: grow legal
        lv_obj_set_flex_grow(right,1);   // absorb remaining width (no horizontal overflow)

        // spectrum card: 24 pad + 22 head + 8 + 294 chart + 8 + 14 band ticks = 370
        // (the B1..B16 strip makes the chart read as an analyzer, not a bar chart)
        lv_obj_t* spectrumCard=makeCard(right,lv_pct(100),scaled(lay::SPECTRUM_CARD_H),scaled(8));
        lv_obj_t* specHead=makeRow(spectrumCard,lv_pct(100),scaled(lay::HEAD_H),scaled(8),LV_FLEX_ALIGN_SPACE_BETWEEN);
        addLabel(specHead,"MODE SPECTRUM",getScaledSmallFont(),COL_HIGHLIGHT,2);
        lv_obj_t* rndBtn=addButton(specHead,lay::RND_W,lay::BTN_H,"RANDOMIZE",COL_HIGHLIGHT);
        lv_obj_add_event_cb(rndBtn,rndBtnCb,LV_EVENT_CLICKED,this);
        lv_obj_t* chart=lv_chart_create(spectrumCard);
        lv_obj_set_size(chart,lv_pct(100),scaled(lay::CHART_H));
        lv_chart_set_type(chart,LV_CHART_TYPE_BAR); lv_chart_set_point_count(chart,16); lv_chart_set_range(chart,LV_CHART_AXIS_PRIMARY_Y,0,1000);
        lv_obj_set_style_bg_color(chart,PLATE_WELL,0); lv_obj_set_style_bg_opa(chart,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(chart,PLATE_LINE,0); lv_obj_set_style_border_width(chart,1,0);
        lv_obj_set_style_radius(chart,scaled(lay::RADIUS),0);
        lv_obj_set_style_pad_all(chart,scaled(8),0); lv_obj_set_style_pad_column(chart,scaled(4),0);
        // piece-3: one vertical gridline per band (16) so the analyzer reads as
        // 16 discrete mode bins, not 12 generic column dividers. Opacity stays
        // at OPA_30 below; the per-band peak tick is what carries the highlight.
        lv_chart_set_div_line_count(chart,4,16);
        lv_obj_set_style_line_color(chart,PLATE_LINE,LV_PART_MAIN); lv_obj_set_style_line_width(chart,1,LV_PART_MAIN); lv_obj_set_style_line_opa(chart,LV_OPA_30,LV_PART_MAIN);
        lv_chart_series_t* series=lv_chart_add_series(chart,COL_HIGHLIGHT,LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_series_color(chart,series,COL_HIGHLIGHT); lv_chart_set_update_mode(chart,LV_CHART_UPDATE_MODE_CIRCULAR);
        lv_obj_add_flag(chart,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(chart,spectrumBandCb,LV_EVENT_PRESSED,this);
        lv_obj_add_event_cb(chart,spectrumBandCb,LV_EVENT_RELEASED,this);
        lv_obj_add_event_cb(chart,spectrumBandCb,LV_EVENT_PRESSING,this);   // drag-scrub across bands
        lv_obj_add_event_cb(chart,spectrumBandCb,LV_EVENT_PRESS_LOST,this); // close the open bracket
        // peak-hold caps painted over the bars after the chart finishes drawing
        lv_obj_add_event_cb(chart,spectrumPeakDrawCb,LV_EVENT_DRAW_POST_END,this);
        fSpectrumChart=chart; for(int i=0;i<16;++i) lv_chart_set_value_by_id(chart,series,i,0);

        // piece-3: B1..B16 micro-legend; B1 is the lead (selected band hairline at
        // 100% alpha amber), the rest are dim. The dim labels still anchor the eye
        // to a 16-step analyzer; the bright B1 is what the operator reads as "the band".
        lv_obj_t* tickRow=makeRow(spectrumCard,lv_pct(100),scaled(lay::TICKS_H),0,LV_FLEX_ALIGN_SPACE_BETWEEN);
        for(int b=0;b<16;++b){
            char nm[8]; snprintf(nm,sizeof(nm),"B%d",b+1);
            lv_obj_t* lbl=addLabel(tickRow,nm,getScaledMicroFont(),b==0?PLATE_AMBER:PLATE_TEXT_DIM,b==0?1:0);
            if(b==0) lv_obj_set_style_text_opa(lbl,LV_OPA_COVER,0);
        }
        // scope card: 24 pad + 22 head + 8 + 14 meter + 8 + 160 scope = 236
        lv_obj_t* scopeCard=makeCard(right,lv_pct(100),scaled(lay::SCOPE_CARD_H),scaled(8));
        lv_obj_t* scopeHead=makeRow(scopeCard,lv_pct(100),scaled(lay::HEAD_H),scaled(8));
        lfoDot=makeBox(scopeHead,scaled(lay::DOT),scaled(lay::DOT));
        lv_obj_set_style_radius(lfoDot,LV_RADIUS_CIRCLE,0);
        lv_obj_set_style_bg_color(lfoDot,COL_HIGHLIGHT,0); lv_obj_set_style_bg_opa(lfoDot,LV_OPA_40,0);
        lv_obj_set_style_shadow_width(lfoDot,scaled(8),0); lv_obj_set_style_shadow_color(lfoDot,COL_HIGHLIGHT,0); lv_obj_set_style_shadow_opa(lfoDot,LV_OPA_40,0);
        lv_obj_t* scopeTitleWrap=makeRow(scopeHead,scaled(150),scaled(lay::HEAD_H),scaled(8));
        addLabel(scopeTitleWrap,"DECAY SCOPE",getScaledSmallFont(),PLATE_TEXT,2);
        lv_obj_set_flex_grow(scopeHead,0);
        lv_obj_t* scopeHint=addLabel(scopeHead,"LIVE ENGINE OUTPUT",getScaledMicroFont(),PLATE_TEXT_DIM,1);
        lv_obj_set_flex_grow(scopeHint,0);
        // output level meter - absolute kParamOutLevel, 500 ms peak hold, headroom zones
        lv_obj_t* meterRow=makeBox(scopeCard,lv_pct(100),scaled(lay::METER_H));
        lv_obj_add_style(meterRow,&styles.meterTrack,0);
        lv_obj_set_layout(meterRow,LV_LAYOUT_NONE);
        fLevelBar=lv_bar_create(meterRow);
        lv_obj_set_size(fLevelBar,lv_pct(100),scaled(8));
        lv_obj_align(fLevelBar,LV_ALIGN_CENTER,0,0);
        lv_obj_add_style(fLevelBar,&styles.meterTrack,LV_PART_MAIN);
        lv_bar_set_range(fLevelBar,0,1000);
        lv_bar_set_value(fLevelBar,0,LV_ANIM_OFF);
        lv_obj_set_style_radius(fLevelBar,scaled(2),LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(fLevelBar,COL_METER_SAFE,LV_PART_INDICATOR);
        // zone marks positioned post-layout by layoutMeterMarks() (60% warn / 85% hot)
        zoneWarnMark=makeBox(meterRow,1,scaled(8));
        lv_obj_set_y(zoneWarnMark,scaled(3));
        lv_obj_set_style_bg_color(zoneWarnMark,COL_HIGHLIGHT,0); lv_obj_set_style_bg_opa(zoneWarnMark,LV_OPA_60,0);
        lv_obj_set_style_radius(zoneWarnMark,0,0);
        zoneHotMark=makeBox(meterRow,1,scaled(8));
        lv_obj_set_y(zoneHotMark,scaled(3));
        lv_obj_set_style_bg_color(zoneHotMark,COL_METER_HOT,0); lv_obj_set_style_bg_opa(zoneHotMark,LV_OPA_80,0);
        lv_obj_set_style_radius(zoneHotMark,0,0);
        // peak-hold marker (created here - the old build styled a nullptr)
        fLevelPeak=makeBox(meterRow,2,scaled(10));
        lv_obj_set_pos(fLevelPeak,0,scaled(2));
        lv_obj_set_style_bg_color(fLevelPeak,PLATE_TITLE,0); lv_obj_set_style_bg_opa(fLevelPeak,LV_OPA_COVER,0);
        lv_obj_set_style_radius(fLevelPeak,0,0);

        lv_obj_t* scope=lv_chart_create(scopeCard);
        lv_obj_set_size(scope,lv_pct(100),scaled(lay::SCOPE_H));
        lv_chart_set_type(scope,LV_CHART_TYPE_LINE); lv_chart_set_point_count(scope,128);
        lv_chart_set_range(scope,LV_CHART_AXIS_PRIMARY_Y,0,1000);
        lv_obj_set_style_bg_color(scope,PLATE_WELL,0); lv_obj_set_style_bg_opa(scope,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(scope,PLATE_LINE,0); lv_obj_set_style_border_width(scope,1,0);
        lv_obj_set_style_radius(scope,scaled(lay::RADIUS),0); lv_obj_set_style_pad_all(scope,scaled(6),0);
        lv_chart_set_div_line_count(scope,3,0);
        lv_obj_set_style_line_color(scope,PLATE_LINE,LV_PART_MAIN); lv_obj_set_style_line_width(scope,1,LV_PART_MAIN); lv_obj_set_style_line_opa(scope,LV_OPA_30,LV_PART_MAIN);
        lv_obj_set_style_line_width(scope,scaled(2),LV_PART_ITEMS);
        lv_chart_series_t* ss=lv_chart_add_series(scope,COL_HIGHLIGHT,LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_update_mode(scope,LV_CHART_UPDATE_MODE_CIRCULAR);
        for(int i=0;i<128;++i) lv_chart_set_next_value(scope,ss,0);
        fScopeChart=scope; fScopeSeries=(void*)ss;

        // keyboard strip (full-width row under the stage)
        createKeyboard(root);

        // sync readouts now that all labels exist
        updateBodyInfo();
        updateBodyPreview();

        // settle the layout synchronously: the passive per-frame pass can sit at
        // a stale fixed point after a rescale rebuild; one explicit pass from the
        // root converges the whole tree - THEN position anything that measured
        lv_obj_update_layout(root);
        layoutMeterMarks();
    }

    void updateSpectrumDisplay(){
        if(!fSpectrumChart) return;
        // place the strike marker once the disc has real geometry
        if(!fMarkerPlaced && strikeDisc && lv_obj_get_width(strikeDisc)>0){ fMarkerPlaced=true; updateStrikeMarker(); }
        lv_chart_series_t* s=lv_chart_get_series_next(fSpectrumChart,nullptr); if(!s) return;
        float bins[16]={};
        float totalE=0.f;
        if(fGotLiveViz){
            // live: per-band modal energies published by the DSP via output parameters
            for(int b=0;b<16;++b) bins[b]=fVizBins[b];
            totalE=fVizLevel;
            float bright=paramCache[PluginMultiScaleBody::kParamBrightness];
            for(int b=0;b<16;++b) if(b>10) bins[b]*=(0.6f+0.4f*bright);
        } else {
            // no live data yet (no audio clock): static preview from the preset's sound map
            using namespace modal;
            int mx = kNumPresets - 1;
            int preset = (int)std::round(paramCache[PluginMultiScaleBody::kParamPreset]*(float)mx); preset=std::clamp(preset,0,mx);
            const auto& pr = kPresets[preset];
            int n = std::clamp((int)(8 + paramCache[PluginMultiScaleBody::kParamModeCount]*120.f), 8, pr.n);
            float sx = paramCache[PluginMultiScaleBody::kParamStrikeX];
            float sy = paramCache[PluginMultiScaleBody::kParamStrikeY];
            float fx=sx*15.f, fy=sy*15.f; int x0=(int)fx, y0=(int)fy; x0=std::clamp(x0,0,14); y0=std::clamp(y0,0,14); int x1=x0+1,y1=y0+1; float dx=fx-x0, dy=fy-y0;
            float w00=(1-dx)*(1-dy), w10=dx*(1-dy), w01=(1-dx)*dy, w11=dx*dy;
            for(int m=0;m<n;++m){
                float g = pr.gain[m][y0][x0]*w00 + pr.gain[m][y0][x1]*w10 + pr.gain[m][y1][x0]*w01 + pr.gain[m][y1][x1]*w11;
                int b = (m*16)/n;
                float trim = paramCache[PluginMultiScaleBody::kParamBand0+std::clamp(b,0,15)]*2.f;
                bins[b] += std::abs(g) * trim;
            }
            float mxv=0; for(int b=0;b<16;++b) mxv=std::max(mxv,bins[b]); if(mxv<1e-9f) mxv=1.f;
            for(int b=0;b<16;++b) bins[b]/=mxv;
            totalE=peakOf(bins)*0.35f;
        }
        static float env[16]={};
        for(int b=0;b<16;++b){
            float target=bins[b];
            env[b]=(target>env[b])?env[b]+(target-env[b])*0.6f:env[b]+(target-env[b])*0.18f;
            int v=(int)std::clamp(env[b]*1000.f,0.f,1000.f);
            lv_chart_set_value_by_id(fSpectrumChart,s,b,v);
            // peak-hold: instant attack, ~700 ms hold (21 frames @30fps), then decay
            if(env[b]>fSpecPeaks[b]){ fSpecPeaks[b]=env[b]; fSpecHoldAge[b]=0; }
            else if(++fSpecHoldAge[b]>21) fSpecPeaks[b]=std::max(0.f,std::max(env[b],fSpecPeaks[b]-0.006f));
        }
        lv_chart_refresh(fSpectrumChart);
        if(fRippleCooldown>0) --fRippleCooldown;
        bool onset=(totalE>fPrevEnergy+std::max(0.02f,fPrevEnergy*1.1f)) && totalE>0.04f;
        fPrevEnergy=std::max(totalE,fPrevEnergy*0.90f);
        if(onset && fRippleCooldown==0 && fGotLiveViz){ spawnRipple(); fRippleCooldown=9; }
        // round-2: fade out the persistent last-strike marker after ~0.5s.
        // The timer fires every 33ms; 15 ticks ~= 500ms.
        if(strikeLastMark && fLastStrikeAgeMs>=0){
            fLastStrikeAgeMs += 33;
            if(fLastStrikeAgeMs >= (int)lay::DISC_STRIKE_HOLD_MS){
                lv_opa_t opa=(lv_opa_t)std::max(0, 255 - (fLastStrikeAgeMs - lay::DISC_STRIKE_HOLD_MS)*2);
                lv_obj_set_style_bg_opa(strikeLastMark, opa, 0);
                lv_obj_set_style_border_opa(strikeLastMark, opa, 0);
                lv_obj_set_style_shadow_opa(strikeLastMark, opa/2, 0);
                if(opa==0){ fLastStrikeAgeMs=-1; }
            }
        }
        gScopeMax=std::max(std::max(totalE,gScopeMax*0.995f),0.03f);
        fLevelEnv+=(totalE-fLevelEnv)*(totalE>fLevelEnv?0.55f:0.12f);
        float lvl=std::clamp(fLevelEnv/gScopeMax,0.f,1.f);
        if(fScopeChart && fScopeSeries)
            lv_chart_set_next_value(fScopeChart,(lv_chart_series_t*)fScopeSeries,(int32_t)(lvl*980.f));
        if(strikeDisc)
            lv_obj_set_style_border_opa(strikeDisc,(lv_opa_t)(70+185.f*lvl),0);
        if(lfoDot)
            lv_obj_set_style_bg_opa(lfoDot,(lv_opa_t)(40+215.f*lvl),0);
        // ~500 ms peak hold then decay (15 frames @ 30 fps)
        if(fLevelBar){
            float v=std::clamp(fGotLiveViz?fVizLevel:0.f,0.f,1.f);
            fMeterEnv+=(v-fMeterEnv)*(v>fMeterEnv?0.55f:0.10f);
            if(fMeterEnv>fMeterPeak){ fMeterPeak=fMeterEnv; fPeakAge=0; }
            else if(++fPeakAge>=15) fMeterPeak=std::max(0.f,fMeterPeak-0.015f);
            lv_bar_set_value(fLevelBar,(int)(fMeterEnv*1000.f),LV_ANIM_OFF);
            lv_color_t zone=fMeterEnv>=0.85f?COL_METER_HOT:(fMeterEnv>=0.60f?COL_HIGHLIGHT:COL_METER_SAFE);
            lv_obj_set_style_bg_color(fLevelBar,zone,LV_PART_INDICATOR);
            if(fLevelPeak){
                lv_coord_t bw=lv_obj_get_width(fLevelBar);
                if(bw>4){
                    const int px=(int)(fMeterPeak*(bw-2));
                    if(px!=fLevelPeakX){ fLevelPeakX=px; lv_obj_set_x(fLevelPeak,(lv_coord_t)px); }
                }
            }
        }
        // self-heal: per-frame meter/peak updates can re-dirty the layout and the
        // passive pass may re-settle at a stale fixed point; an explicit pass
        // converges the tree and is a no-op when nothing is dirty
        lv_obj_update_layout(lv_screen_active());
    }
    DGL_NAMESPACE::LVGLTopLevelWidget* fLVGL=nullptr;
    UIStyles styles;
    bool fUIBuilt=false;   // tracks whether buildUI() has populated the tree
    lv_obj_t* widgets[PluginMultiScaleBody::kParameterCount]={};
    float paramCache[PluginMultiScaleBody::kParameterCount]={};

    // master knob value label (re-uses the widget's own label, but the chip in
    // the dial bank's Wet knob is the canonical one - master just inherits it)
    lv_timer_t* fSpectrumTimer=nullptr;
    lv_obj_t* fSpectrumChart=nullptr;
    lv_obj_t* strikeDisc=nullptr;
    lv_obj_t* strikeDot=nullptr;
    lv_obj_t* strikeCoordLabel=nullptr;
    lv_obj_t* presetDropdown=nullptr;
    // piece-6: preset browser prev/next mini arrows flanking the dropdown
    lv_obj_t* presetPrevBtn=nullptr;
    lv_obj_t* presetNextBtn=nullptr;
    lv_obj_t* bodySubLabel=nullptr;
    lv_obj_t* bodyPreview=nullptr;
    lv_obj_t* lfoDot=nullptr;
    lv_obj_t* hdrBodyVal=nullptr;
    lv_obj_t* hdrMatVal=nullptr;
    lv_obj_t* hdrModeVal=nullptr;
    lv_obj_t* hdrF0Val=nullptr;
    int fPeakAge=0;
    int fLevelPeakX=-1;
    lv_obj_t* fScopeChart=nullptr;
    void* fScopeSeries=nullptr;
    lv_obj_t* fLevelBar=nullptr;
    lv_obj_t* fLevelPeak=nullptr;
    lv_obj_t* zoneWarnMark=nullptr;
    lv_obj_t* zoneHotMark=nullptr;
    float fLevelEnv=0.f;
    float fPrevEnergy=0.f;
    float gScopeMax=0.05f;
    // macro-LED row status dots (round-2: was macro-rack value chip labels in r1;
    // r1 also had a 72px row of 8 mini arc-knobs that the critic flagged for
    // competing with the dial groups. r2 collapses the strip to a 12px status
    // row of dots, each lit when its kMacroParams[m] is non-default).
    lv_obj_t* macroLedDots[8]={};

    float fMeterEnv=0.f;
    float fMeterPeak=0.f;
    int fStrikeNote=60;
    bool fStrikeHeld=false;
    bool fMarkerPlaced=false;
    lv_obj_t* strikeLastMark=nullptr;        // round-2: small amber dot that persists ~0.5s post-hit
    int fLastStrikeAgeMs=0;                 // ms since placeLastStrike; -1 = inactive
    int fRippleCooldown=0;
    // written level, so PRESSING writes only on real change (no host spam)
    int fScrubBand=-1;
    float fScrubLevel=-1.f;
    // live metering cache fed by output parameters (bridge-safe DSP->UI link)
    float fVizLevel=0.f;
    float fVizBins[16]={};
    bool fGotLiveViz=false;
    // arpeggiator toggle (mirrors plugin state "arpon")
    lv_obj_t* arpBtn=nullptr;
    bool arpOnLocal=false;
    // keyboard
    lv_obj_t* kbContainer=nullptr;
    lv_obj_t* kbWhite[7];
    lv_obj_t* kbBlack[5];
    lv_obj_t* kbOctLabel=nullptr;
    int kbBaseNote=60;
    int kbHeldNote=-1;
    // header zoom stepper
    lv_obj_t* zoomMinus=nullptr;
    lv_obj_t* zoomPlus=nullptr;
    lv_obj_t* zoomValLbl=nullptr;
    int fZoomIdx=2;   // 100% (index into lay::ZOOM_STEPS)
    // spectrum peak-hold caps (falling-hold markers above the bars)
    float fSpecPeaks[16]={};
    int fSpecHoldAge[16]={};
};
UI* createUI(){ return new MultiScaleBodyUI(); }
const uint32_t MultiScaleBodyUI::kMacroParams[8]={
    PluginMultiScaleBody::kParamPitch,
    PluginMultiScaleBody::kParamDecay,
    PluginMultiScaleBody::kParamBrightness,
    PluginMultiScaleBody::kParamModeCount,
    PluginMultiScaleBody::kParamWidth,
    PluginMultiScaleBody::kParamRadiation,
    PluginMultiScaleBody::kParamVelStrike,
    PluginMultiScaleBody::kParamWet,
};
END_NAMESPACE_DISTRHO
