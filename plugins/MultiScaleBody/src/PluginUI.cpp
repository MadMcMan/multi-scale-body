#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
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
#include <functional>
#include <cmath>
#ifdef _WIN32
#include <commdlg.h>   // GetOpenFileNameA (windows.h already pulled by DGL)
#endif
// idea 13: generate the .scl text of an N-EDO scale (name, count, N degree
// lines in cents). The plugin parser handles this exact shape (implicit
// tonic: degree k sits on note 60+k, the last line is the octave boundary),
// so 12-EDO here reproduces the classic ratios exactly.
static std::string edoSclText(int n){
    char head[32]; snprintf(head,sizeof(head),"%d-edo",n);
    std::string s=head; s+="\n";
    char tmp[16]; snprintf(tmp,sizeof(tmp),"%d\n",n); s+=tmp;
    char b[32];
    for(int k=1;k<=n;++k){
        snprintf(b,sizeof(b),"%.6f\n",1200.0*(double)k/(double)n);
        s+=b;
    }
    return s;
}
// Windows .scl file picker: returns false when cancelled or unreadable.
// Reads at most 64 KiB of text (a scale file is a few hundred bytes).
static bool loadSclFileDialog(std::string& outText){
#ifdef _WIN32
    char path[MAX_PATH]={0};
    OPENFILENAMEA ofn{}; ofn.lStructSize=sizeof(ofn);
    ofn.lpstrFilter="Scala scale files (*.scl)\0*.scl\0All files (*.*)\0*.*\0";
    ofn.lpstrFile=path; ofn.nMaxFile=(DWORD)sizeof(path);
    ofn.lpstrTitle="Load a Scala .scl tuning";
    ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_HIDEREADONLY;
    if(GetOpenFileNameA(&ofn)==0) return false;
    FILE* f=std::fopen(path,"rb");
    if(!f) return false;
    std::fseek(f,0,SEEK_END); const long sz=std::ftell(f); std::fseek(f,0,SEEK_SET);
    if(sz<=0||sz>64*1024){ std::fclose(f); return false; }
    outText.assign((size_t)sz,'\0');
    const bool ok=std::fread(&outText[0],1,(size_t)sz,f)==(size_t)sz;
    std::fclose(f);
    return ok;
#else
    (void)outText;
    return false;
#endif
}
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

// === RIGHT-CLICK CATCHER (idea 15: MIDI learn) ==============================
// dpf-widgets' LVGL bridge (indev_mouse_read_cb) feeds ONLY the left mouse
// button to the LVGL pointer indev, so right-click never reaches lv_event
// handlers. DGL itself receives every button (LVGLWidget::onMouse runs the
// BaseWidget dispatch BEFORE storing its button state), so we subclass the
// top-level widget and relay button 2 (kMouseButtonRight) presses by absolute
// window position. The event still passes through to LVGLTopLevelWidget::
// onMouse untouched — the indev ignores non-left buttons, so nothing changes
// on the LVGL side.
class MultiScaleBodyLVGLWidget : public DGL_NAMESPACE::LVGLTopLevelWidget {
public:
    std::function<void(int,int)> onRightClick;   // absolute window x,y
    explicit MultiScaleBodyLVGLWidget(DGL_NAMESPACE::Window& w)
        : LVGLTopLevelWidget(w) {}
protected:
    bool onMouse(const DGL_NAMESPACE::Widget::MouseEvent& ev) override {
        if(ev.button==DGL_NAMESPACE::kMouseButtonRight && ev.press && onRightClick)
            onRightClick((int)ev.absolutePos.getX(),(int)ev.absolutePos.getY());
        return LVGLTopLevelWidget::onMouse(ev);
    }
};

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
//      s = min(w/1440, h/990) because every region sums exactly to the base
//      budget (see the arithmetic comments in UICommon.hpp / buildUI below).
// Direction: MACHINED PLATE / INDUSTRIAL AMBER / TACTILE - warm true-black
// chassis, one amber accent that only ever means value or signal, hero = the
// playable body disc.
// ============================================================================
class MultiScaleBodyUI : public UI, public AbstractMultiScaleBodyUI {
public:
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
        paramCache[PluginMultiScaleBody::kParamVolume]=1.f;
        // wave-2 defaults (everything off/identity; bandDecay blanket 0.5 fits
        // the clearWidgetRefs wipe, but these four need explicit zeros)
        paramCache[PluginMultiScaleBody::kParamBow]=0.f;
        paramCache[PluginMultiScaleBody::kParamDamper]=0.f;
        paramCache[PluginMultiScaleBody::kParamInharm]=0.f;
        paramCache[PluginMultiScaleBody::kParamSlideMode]=0.f;
        // wave-3 defaults: physical model off (EcoBudget blanket 0.5 matches)
        paramCache[PluginMultiScaleBody::kParamSupport]=0.f;
        paramCache[PluginMultiScaleBody::kParamHoldDamp]=0.f;
        paramCache[PluginMultiScaleBody::kParamResMorph]=0.f;
        paramCache[PluginMultiScaleBody::kParamMorphTarget]=0.f;
        paramCache[PluginMultiScaleBody::kParamMorphAmt]=0.f;
        paramCache[PluginMultiScaleBody::kParamMaterial]=0.f;
        paramCache[PluginMultiScaleBody::kParamRayleighA]=0.f;
        paramCache[PluginMultiScaleBody::kParamRayleighB]=0.f;
        paramCache[PluginMultiScaleBody::kParamEcoMode]=0.f;
        setSize(DISTRHO_UI_DEFAULT_WIDTH, DISTRHO_UI_DEFAULT_HEIGHT);
        fLVGL = new MultiScaleBodyLVGLWidget(getWindow());
        // Right-click routing (DPF's LVGL indev only feeds the left button):
        // DGL sees every button, so the subclass forwards right presses by
        // absolute position. Knob hit -> MIDI learn; anywhere else -> cancel.
        ((MultiScaleBodyLVGLWidget*)fLVGL)->onRightClick=[this](int x,int y){
            this->handleRightClick(x,y);
        };
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
            case P::kParamWet: return "Reverb";        case P::kParamMono: return "Mono"; case P::kParamVolume: return "Volume";
            case P::kParamBow: return "Bow";           case P::kParamDamper: return "Damper";
            case P::kParamInharm: return "Inharm";     case P::kParamSlideMode: return "Slide";
            case P::kParamSupport: return "Support";   case P::kParamHoldDamp: return "Hold Damp";
            case P::kParamResMorph: return "Resolution"; case P::kParamMorphAmt: return "Morph";
            case P::kParamMorphTarget: return "Morph Tgt"; case P::kParamMaterial: return "Material";
            case P::kParamRayleighA: return "Rayl A";   case P::kParamRayleighB: return "Rayl B";
            case P::kParamEcoMode: return "Eco";       case P::kParamEcoBudget: return "Eco Budget";
            default: return {}; // bands and metering outputs have no knob title
        }
    }
    float getParamValue(uint32_t i) const override { if(i<PluginMultiScaleBody::kParameterCount) return paramCache[i]; return 0.f; }
    void setParamValue(uint32_t i,float v) override {
        if(i>=PluginMultiScaleBody::kParameterCount) return;
        paramCache[i]=v; setParameterValue(i,v); syncParamWidget(i,v);
        // FIX: UI-originated gestures must invalidate the same derived
        // views the host-echo path does - previously only preset sync ran
        // here, so Decay/strike/Modes/band drags never moved DAMPING,
        // MODE MAP, or the scope preview until a host echo.
        if(i==PluginMultiScaleBody::kParamPreset || i==PluginMultiScaleBody::kParamDecay
           || i==PluginMultiScaleBody::kParamModeCount || i==PluginMultiScaleBody::kParamStrikeX
           || i==PluginMultiScaleBody::kParamStrikeY || i==PluginMultiScaleBody::kParamSupport
           || i==PluginMultiScaleBody::kParamHoldDamp || i==PluginMultiScaleBody::kParamResMorph
           || i==PluginMultiScaleBody::kParamMorphTarget || i==PluginMultiScaleBody::kParamMorphAmt
           || i==PluginMultiScaleBody::kParamMaterial || i==PluginMultiScaleBody::kParamRayleighA
           || i==PluginMultiScaleBody::kParamRayleighB)
            fScopePreviewReady=false;
        if(i==PluginMultiScaleBody::kParamPreset || i==PluginMultiScaleBody::kParamModeCount
           || i==PluginMultiScaleBody::kParamStrikeX || i==PluginMultiScaleBody::kParamStrikeY
           || i==PluginMultiScaleBody::kParamResMorph || i==PluginMultiScaleBody::kParamMorphTarget
           || i==PluginMultiScaleBody::kParamMorphAmt
           || (i>=PluginMultiScaleBody::kParamBand0 && i<=PluginMultiScaleBody::kParamBand15))
            fModeMapDirty=true;
        // wave-3: gain-affecting params also invalidate the disc heatmap
        if(i==PluginMultiScaleBody::kParamPreset || i==PluginMultiScaleBody::kParamModeCount
           || i==PluginMultiScaleBody::kParamResMorph || i==PluginMultiScaleBody::kParamMorphTarget
           || i==PluginMultiScaleBody::kParamMorphAmt
           || (i>=PluginMultiScaleBody::kParamBand0 && i<=PluginMultiScaleBody::kParamBand15))
            fHeatDirty=true;
        if(i==PluginMultiScaleBody::kParamStrikeX || i==PluginMultiScaleBody::kParamStrikeY) updateStrikeMarker();
        if(i==PluginMultiScaleBody::kParamPreset){ syncPresetDropdown(v); if(bodySubLabel) updateBodyInfo(); updateBodyPreview(); }
        // wave-3: support/hold/res/morph/rayleigh reshape the ring like Decay does
        if(i==PluginMultiScaleBody::kParamDecay || i==PluginMultiScaleBody::kParamPreset
           || i==PluginMultiScaleBody::kParamSupport || i==PluginMultiScaleBody::kParamHoldDamp
           || i==PluginMultiScaleBody::kParamResMorph || i==PluginMultiScaleBody::kParamMorphTarget
           || i==PluginMultiScaleBody::kParamMorphAmt || i==PluginMultiScaleBody::kParamRayleighA
           || i==PluginMultiScaleBody::kParamRayleighB
           || i==PluginMultiScaleBody::kParamStrikeX || i==PluginMultiScaleBody::kParamStrikeY
           || (i>=PluginMultiScaleBody::kParamBandDecay0 && i<=PluginMultiScaleBody::kParamBandDecay15)) updateDampingDisplay();
        if(i==PluginMultiScaleBody::kParamVolume && fMasterValLbl){
            char b[24]; formatParamValue(PluginMultiScaleBody::kParamVolume,v,b,sizeof(b));
            lv_label_set_text(fMasterValLbl,b);
        }
        // wave-3/4: programmatic writes (RANDOMIZE, MIDI learn) must refresh
        // the toggle + dropdowns immediately, not just on host echo.
        // (set_selected / add_state never re-fire VALUE_CHANGED: no loops)
        if(i==PluginMultiScaleBody::kParamEcoMode && fEcoBtn){
            if(v>0.5f) lv_obj_add_state(fEcoBtn,LV_STATE_CHECKED);
            else lv_obj_clear_state(fEcoBtn,LV_STATE_CHECKED);
        }
        if(i==PluginMultiScaleBody::kParamMaterial && fMaterialDd)
            lv_dropdown_set_selected(fMaterialDd,std::clamp((int)std::lround(v*10.f),0,10));
        if(i==PluginMultiScaleBody::kParamMorphTarget && fMorphDd)
            lv_dropdown_set_selected(fMorphDd,std::clamp((int)std::lround(v*(float)(modal::kNumPresets-1)),0,modal::kNumPresets-1));
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
        // R3: the idle scope preview depends on these params - recompute lazily
        if(i==PluginMultiScaleBody::kParamPreset || i==PluginMultiScaleBody::kParamDecay
           || i==PluginMultiScaleBody::kParamModeCount || i==PluginMultiScaleBody::kParamStrikeX
           || i==PluginMultiScaleBody::kParamStrikeY || i==PluginMultiScaleBody::kParamSupport
           || i==PluginMultiScaleBody::kParamHoldDamp || i==PluginMultiScaleBody::kParamResMorph
           || i==PluginMultiScaleBody::kParamMorphTarget || i==PluginMultiScaleBody::kParamMorphAmt
           || i==PluginMultiScaleBody::kParamMaterial || i==PluginMultiScaleBody::kParamRayleighA
           || i==PluginMultiScaleBody::kParamRayleighB)
            fScopePreviewReady=false;
        // ROUND-6: the MODE MAP re-derives from these (plus band trims)
        if(i==PluginMultiScaleBody::kParamPreset || i==PluginMultiScaleBody::kParamModeCount
           || i==PluginMultiScaleBody::kParamStrikeX || i==PluginMultiScaleBody::kParamStrikeY
           || i==PluginMultiScaleBody::kParamResMorph || i==PluginMultiScaleBody::kParamMorphTarget
           || i==PluginMultiScaleBody::kParamMorphAmt
           || (i>=PluginMultiScaleBody::kParamBand0 && i<=PluginMultiScaleBody::kParamBand15))
            fModeMapDirty=true;
        // wave-3: gain-affecting params also invalidate the disc heatmap
        if(i==PluginMultiScaleBody::kParamPreset || i==PluginMultiScaleBody::kParamModeCount
           || i==PluginMultiScaleBody::kParamResMorph || i==PluginMultiScaleBody::kParamMorphTarget
           || i==PluginMultiScaleBody::kParamMorphAmt
           || (i>=PluginMultiScaleBody::kParamBand0 && i<=PluginMultiScaleBody::kParamBand15))
            fHeatDirty=true;
        // FIX: kParamOutLevel was falling through to the generic branch so
        // fVizLevel stayed 0 forever (flat scope + dead meter once live).
        // fLiveAge resets on every metering write so the idle preview
        // resumes after ~1.5s of silence instead of flatlining.
        if(i==PluginMultiScaleBody::kParamOutLevel){ fVizLevel=v; fGotLiveViz=true; fLiveAge=0; return; }
        if(i>=PluginMultiScaleBody::kParamOutBand0 && i<PluginMultiScaleBody::kParameterCount){
            fVizBins[i-PluginMultiScaleBody::kParamOutBand0]=v;
            fGotLiveViz=true; fLiveAge=0;
            return;
        }
        // A real (non-metering) parameter just moved. The learn chip is
        // non-blocking, so only the armed target counts as a landed binding.
        if(fLearnParam>=0 && i==(uint32_t)fLearnParam){
            fLearnParam=-1;
            if(fLearnChip) lv_obj_add_flag(fLearnChip,LV_OBJ_FLAG_HIDDEN);
        }
        if(i<PluginMultiScaleBody::kParameterCount){ paramCache[i]=v; syncParamWidget(i,v);
            if(i==PluginMultiScaleBody::kParamStrikeX || i==PluginMultiScaleBody::kParamStrikeY) updateStrikeMarker();
            if(i==PluginMultiScaleBody::kParamPreset){ syncPresetDropdown(v); if(bodySubLabel) updateBodyInfo(); updateBodyPreview(); }
            // R5: Decay knob drives the DAMPING panel's live rescale (and
            // preset changes always re-bake it). Gate by fDampPresetCache/
            // fDampDecayCache inside updateDampingDisplay so other params
            // cost nothing here.
            if(i==PluginMultiScaleBody::kParamDecay || i==PluginMultiScaleBody::kParamPreset
               || i==PluginMultiScaleBody::kParamSupport || i==PluginMultiScaleBody::kParamHoldDamp
               || i==PluginMultiScaleBody::kParamResMorph || i==PluginMultiScaleBody::kParamMorphTarget
               || i==PluginMultiScaleBody::kParamMorphAmt || i==PluginMultiScaleBody::kParamRayleighA
               || i==PluginMultiScaleBody::kParamRayleighB
               || i==PluginMultiScaleBody::kParamStrikeX || i==PluginMultiScaleBody::kParamStrikeY
           || (i>=PluginMultiScaleBody::kParamBandDecay0 && i<=PluginMultiScaleBody::kParamBandDecay15)) updateDampingDisplay();
            if(i==PluginMultiScaleBody::kParamVolume && fMasterValLbl){
                char b[24]; formatParamValue(PluginMultiScaleBody::kParamVolume,v,b,sizeof(b));
                lv_label_set_text(fMasterValLbl,b);
            }
            // wave-3: MODEL panel controls follow host automation too
            // (set_selected / add_state never re-fire VALUE_CHANGED: no loops)
            if(i==PluginMultiScaleBody::kParamEcoMode && fEcoBtn){
                if(v>0.5f) lv_obj_add_state(fEcoBtn,LV_STATE_CHECKED);
                else lv_obj_clear_state(fEcoBtn,LV_STATE_CHECKED);
            }
            if(i==PluginMultiScaleBody::kParamMaterial && fMaterialDd)
                lv_dropdown_set_selected(fMaterialDd,std::clamp((int)std::lround(v*10.f),0,10));
            if(i==PluginMultiScaleBody::kParamMorphTarget && fMorphDd)
                lv_dropdown_set_selected(fMorphDd,std::clamp((int)std::lround(v*(float)(modal::kNumPresets-1)),0,modal::kNumPresets-1));
        }
    }
    void stateChanged(const char* key,const char* value) override {
        if(key && std::strcmp(key,"arpon")==0){
            arpOnLocal = value && value[0]=='1';
            if(arpBtn){ if(arpOnLocal) lv_obj_add_state(arpBtn,LV_STATE_CHECKED); else lv_obj_clear_state(arpBtn,LV_STATE_CHECKED); }
        }
        // host pushed a microtonal scale (patch load): mirror it in the UI
        if(key && std::strcmp(key,"scale")==0){
            scaleTxtCached_ = value ? value : "";
            updateScaleLabel();
            if(fEdoDropdown){
                const int idx=autoEdoOf(scaleTxtCached_);
                if(idx>=0) lv_dropdown_set_selected(fEdoDropdown,idx);
            }
        }
    }
    // label readout for the loaded .scl: first non-comment, non-count line,
    // or the 12-EDO default when no scale is loaded.
    void updateScaleLabel(){
        if(!fScaleLbl) return;
        char buf[48];
        snprintf(buf,sizeof(buf),"SCALE: %s",scaleName(scaleTxtCached_).c_str());
        lv_label_set_text(fScaleLbl,buf);
    }
    static std::string scaleName(const std::string& txt){
        if(txt.empty()) return "12-EDO";
        size_t pos=0;
        while(pos<txt.size()){
            size_t e=txt.find('\n',pos);
            if(e==std::string::npos) e=txt.size();
            std::string line=txt.substr(pos,e-pos);
            pos=e+1;
            size_t s0=line.find_first_not_of(" \t\r");
            if(s0==std::string::npos) continue;
            size_t s1=line.find_last_not_of(" \t\r");
            line=line.substr(s0,s1-s0+1);
            if(line.empty()||line[0]=='!') continue;
            // a pure integer <= 128 is the degree count, not a name
            char* end=nullptr; long c=strtol(line.c_str(),&end,10);
            if(end && *end=='\0' && c>=2 && c<=128) return "(unnamed)";
            return line;
        }
        return "12-EDO";
    }
    void uiIdle() override {
        // First build AND every rescale are owned by rebuildForScale(),
        // including the timer lifecycle. One probe per idle: if no built tree
        // exists yet, build it (rebuildForScale no longer early-returns for
        // an unbuilt tree, even at identical scale); otherwise rebuild only
        // when the surface scale actually moved.
        const float ns=currentSurfaceScale();
        if(!fUIBuilt || std::abs(ns-::DISTRHO::gUIScale)>=0.01f) rebuildForScale(ns);
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
        // Early-return only if a built tree exists at this scale. A bare
        // scale check let a standalone window opening at EXACTLY the base
        // size (scale 1.0, no delta from the default) skip the first build
        // entirely - blank exe screen. The harness never caught it (its
        // surface is 1458x907, scale 1.0125, always differs).
        if(fUIBuilt && std::abs(ns-::DISTRHO::gUIScale)<0.01f) return;
        // Re-entry guard: a rapid resize (e.g. dragging a DAW window corner)
        // fires uiReshape several times before the first rebuild finishes.
        // Without this, the second pass can lv_obj_clean mid-construction
        // and leave the screen blank - the "vanishes on resize" symptom.
        if(fRebuildInFlight) return;
        fRebuildInFlight=true;
        ::DISTRHO::gUIScale=ns;
        if(!fUIBuilt){
            // first build (constructor / first uiIdle): no existing tree
            styles.reset(); styles.init();
            buildUI(nullptr);
        } else {
            // resize rebuild: release held notes BEFORE touching the screen
            if(kbHeldNote>=0 && kbHeldNote<=127){ sendNote(0,(uint8_t)kbHeldNote,0); kbHeldNote=-1; }
            if(fStrikeHeld){ sendNote((uint8_t)fStrikeChannel,(uint8_t)fStrikeNote,0); fStrikeHeld=false; }
            // wave-3 (idea 10): never strand a playback note across a rebuild
            if(fPlayHeld||fRecPlaying) stopPlayback();
            for(int o=0;o<lay::KEY_WHITE_N*2;++o){ int n=kbBaseNote+o; if(n>=0&&n<=127) sendNote(0,(uint8_t)n,0); }
            // Delete the old timer FIRST so the rebuild can't be tickled
            // mid-construction (the tick dereferences fSpectrumChart etc.,
            // which are about to be nulled). Also stops the per-rebuild
            // timer leak (each rebuild used to add another without
            // deleting the last).
            if(fSpectrumTimer){ lv_timer_del(fSpectrumTimer); fSpectrumTimer=nullptr; }
            // Build the new tree into a hidden overlay parent so the
            // existing tree stays visible during construction. Without
            // this, lv_obj_clean blanks the screen for the entire buildUI()
            // duration - the visible flash and vanish on every resize.
            lv_obj_t* screen=lv_screen_active();
            lv_obj_t* stash=screen ? lv_obj_create(screen) : nullptr;
            if(stash){
                lv_obj_set_size(stash,lv_pct(100),lv_pct(100));
                lv_obj_add_flag(stash,LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(stash,(lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE));
                lv_obj_set_style_bg_opa(stash,LV_OPA_TRANSP,0);
                lv_obj_set_style_border_width(stash,0,0);
                lv_obj_set_style_pad_all(stash,0,0);
            }
            // Capture the OLD screen children (old tree + the stash) BEFORE
            // buildUI: new dropdowns created inside the stash parent their
            // list to the REAL screen (LVGL lv_dropdown_constructor), so
            // after the build the screen's child list is old-tree + stash +
            // NEW lists. Snapshotting first keeps the new lists out of the
            // delete set.
            lv_obj_t* old[24]; uint32_t oldN=0;
            if(screen){
                const uint32_t sn=lv_obj_get_child_count(screen);
                for(uint32_t i=0;i<sn && oldN<24;++i){
                    lv_obj_t* c=lv_obj_get_child(screen,i);
                    if(c!=stash) old[oldN++]=c;
                }
            }
            fUIBuilt=false;
            // Snapshot the live param cache BEFORE clearWidgetRefs() wipes it:
            // knob positions must survive zoom/resize rebuilds instead of
            // snapping back to defaults until a host echo arrives (DPF never
            // re-sends params on resize). Restored right after the wipe so
            // buildUI() lays every knob out at its current value.
            float pcSnap[PluginMultiScaleBody::kParameterCount];
            for(uint32_t i=0;i<PluginMultiScaleBody::kParameterCount;++i) pcSnap[i]=paramCache[i];
            clearWidgetRefs();
            for(uint32_t i=0;i<PluginMultiScaleBody::kParameterCount;++i) paramCache[i]=pcSnap[i];
            fPrevEnergy=0.f; fLevelEnv=0.f; fMeterEnv=0.f; fMeterPeak=0.f; fPeakAge=0; gScopeMax=0.05f; fRippleCooldown=0; fStrikeHeld=false;
            fMarkerPlaced=false;
            styles.reset(); styles.init();
            buildUI(stash);
            // The NEW tree is fully built (hidden in stash); old tree still
            // on screen. Delete the old tree now - each captured pointer
            // with a liveness check (deleting the old topbar cascades to its
            // dropdown's destructor, which deletes that dropdown's own list,
            // also a screen child that may be in this array).
            for(uint32_t i=0;i<oldN;++i)
                if(lv_obj_is_valid(old[i])) lv_obj_del(old[i]);
            // Atomic swap: move every new child from the hidden stash onto
            // the real screen, then delete the stash. No blank frame
            // between old tree and new tree.
            if(stash){
                const uint32_t n=lv_obj_get_child_count(stash);
                for(int32_t i=(int32_t)n-1;i>=0;--i){
                    lv_obj_t* c=lv_obj_get_child(stash,(uint32_t)i);
                    lv_obj_set_parent(c,screen);
                }
                lv_obj_del(stash);
            }
        }
        if(fUIBuilt && !fSpectrumTimer){
            fSpectrumTimer = lv_timer_create([](lv_timer_t* t){
                auto ui=(MultiScaleBodyUI*)lv_timer_get_user_data(t); if(ui) ui->updateSpectrumDisplay();
            },33,this);
        }
        fRebuildInFlight=false;
    }
private:
    // single owner of every lv_obj_t* member default; ctor and rebuildForScale share it
    void clearWidgetRefs(){
        for(uint32_t i=0;i<PluginMultiScaleBody::kParameterCount;++i){ widgets[i]=nullptr; paramCache[i]=0.5f; }
        // volume's default is 1.0 (unity), not the blanket 0.5 — a zoom
        // rebuild must not display a phantom -12 dB
        paramCache[PluginMultiScaleBody::kParamVolume]=1.f;
        extraWidgetCount=0; for(uint32_t i=0;i<kMaxExtraWidgets;++i) extraWidgets[i]={0,nullptr};
        strikeDisc=strikeDot=strikeCoordLabel=presetDropdown=bodySubLabel=nullptr;
        // piece-6: preset browser prev/next arrows
        presetPrevBtn=presetNextBtn=nullptr;
        bodyPreview=lfoDot=strikeLastMark=nullptr;
        hdrBodyVal=hdrMatVal=hdrModeVal=hdrF0Val=nullptr;
        fSpectrumChart=fScopeChart=fLevelBar=fLevelPeak=zoneWarnMark=zoneHotMark=nullptr;
        fScopeSeries=nullptr; fScopeAreaSeries=nullptr; arpBtn=nullptr;
        kbContainer=kbOctLabel=zoomMinus=zoomPlus=zoomValLbl=nullptr;
        for(int i=0;i<7;++i) kbWhite[i]=nullptr;
        for(int i=0;i<5;++i) kbBlack[i]=nullptr;
        // R5: damping panel - bars + value labels
        for(int i=0;i<16;++i){ fDampBars[i]=nullptr; fDampVals[i]=nullptr; }
        fDampMax=1.f; fDampPresetCache=-1; fDampDecayCache=-1.f; fDampBandSumCache=-1.f; fDampPhysSumCache=-1.f;
        for(int i=0;i<100;++i) fHeatDots[i]=nullptr; fHeatCount=0; fHeatDirty=true; fModeFocus=-1;
        fScrubMode=0; fScrubParamIdx=-1; fScrubToggle=nullptr;
        fLearnChip=fLearnLbl=nullptr; fLearnParam=-1;
        fEdoDropdown=nullptr; fScaleLbl=nullptr;
        fRecBtn=fPlayBtn=nullptr; fRecOn=false; fRecPlaying=false; fRecN=0; fRecCursor=0; fPlayHeld=false;
        fMaterialDd=fMorphDd=fEcoBtn=nullptr;
        fMasterValLbl=nullptr; fStrikeChannel=0; fNextStrikeChannel=1; fLiveAge=1000; fRebuildInFlight=false;
    }
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
        if(hdrF0Val){ char b[16]; snprintf(b,sizeof(b),"%.0f HZ",uiEffFreqHz(pr,0,idx,std::clamp((int)(8+paramCache[PluginMultiScaleBody::kParamModeCount]*120.f),8,pr.n))); lv_label_set_text(hdrF0Val,b); }
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
    // R5: DAMPING panel updater. Per-band tail-time map. Each band
    // groups modes [b*n/16, (b+1)*n/16); the band's T60 (60dB decay time
    // in abstract units) = 6.9078 / mean(decay[m]) for the modes in that
    // band, then scaled by the engine's decayScale_ mirror so the DECAY
    // knob's effect is visible live (same v->scale curve as engine
    // setDecayScale: 0.1*pow(100, 1-v)). Bar value is t60 / maxT60 * 1000
    // (the lv_bar 0..1000 range). Gated by fDampPresetCache/fDampDecayCache
    // so a move of a non-decay param (e.g. a knob in the left bank) does
    // not re-walk the 128 modes. Bars with no modes (e.g. Bar preset at
    // wave-4 (phys-visual mirror): the engine's effective per-mode resonant
    // frequency in Hz — mirrors bodyFreq() + pitchScale so every readout
    // (F0 cell, MODE MAP peak label) moves when a physical knob moves, not
    // only on preset change. nPreset = preset index, nModes = current mode
    // count semantics (8+v*120 like the engine); Tune/LFO/detune stay the
    // global scalers the engine applies afterwards.
    float uiEffFreqHz(const modal::PresetData& pr,int m,int nPreset,int nModes)const{
        using P=PluginMultiScaleBody;
        float f=pr.freq[m];
        const float rm=paramCache[P::kParamResMorph];
        if(rm>1e-4f) f=f+(pr.fineFreq[m]-f)*rm;
        const float mAmt=paramCache[P::kParamMorphAmt];
        if(mAmt>1e-4f){
            const int mtx=std::clamp((int)std::lround(paramCache[P::kParamMorphTarget]*(float)(modal::kNumPresets-1)),0,modal::kNumPresets-1);
            const auto& pt=modal::kPresets[mtx];
            const int ti=std::min(m,std::max(1,pt.n)-1);
            f=f+(pt.freq[ti]-f)*mAmt;
        }
        const float sup=paramCache[P::kParamSupport];
        if(sup>1e-4f && nModes>1){
            const float q=(float)m/(float)(nModes-1);
            f*=1.f+sup*modal::MultiScaleBodyEngine::kSupFreqTop*q*q;
        }
        const int mat=std::clamp((int)std::lround(paramCache[P::kParamMaterial]*10.f),0,10);
        if(mat>0){
            const auto& bm=modal::MultiScaleBodyEngine::kBodyMat[std::clamp(nPreset,0,modal::kNumPresets-1)];
            const auto& md=modal::MultiScaleBodyEngine::kMaterials[mat];
            if(bm.E>0.0&&bm.rho>0.0) f*=std::sqrt((float)(md.E/md.rho)/(float)(bm.E/bm.rho));
        }
        f*=std::pow(2.f,(paramCache[P::kParamPitch]-0.5f)*4.f); // Tune knob mirror
        return f/(2.f*3.141593f);
    }
    // n=88, bands 8..15) render with 0 fill and "-" label.
    void updateDampingDisplay(){
        if(!fDampBars[0]) return;
        int mx = modal::kNumPresets - 1;
        int idx = (int)std::round(paramCache[PluginMultiScaleBody::kParamPreset]*(float)mx);
        idx = std::clamp(idx,0,mx);
        float decayV = paramCache[PluginMultiScaleBody::kParamDecay];
        // idea 2: per-band decay trims also rescale the display (and gate the
        // recompute). Trim curve mirrors the engine: v 0.5 -> EXACT 1.0.
        float bdSum=0.f, bandTrim[16];
        for(int b=0;b<16;++b){
            bandTrim[b]=std::pow(2.f,(paramCache[PluginMultiScaleBody::kParamBandDecay0+b]-0.5f)*2.f);
            bdSum+=paramCache[PluginMultiScaleBody::kParamBandDecay0+b];
        }
        // wave-3: support/hold/res/morph/rayleigh reshape the ring; they join
        // the gate so the display never lies about the tail.
        float sup=paramCache[PluginMultiScaleBody::kParamSupport];
        float hd=paramCache[PluginMultiScaleBody::kParamHoldDamp];
        float rm=paramCache[PluginMultiScaleBody::kParamResMorph];
        float mAmt=paramCache[PluginMultiScaleBody::kParamMorphAmt];
        float mTgt=paramCache[PluginMultiScaleBody::kParamMorphTarget];
        float rA=paramCache[PluginMultiScaleBody::kParamRayleighA];
        float rB=paramCache[PluginMultiScaleBody::kParamRayleighB];
        float physSum=sup+hd+rm+mAmt+mTgt+rA+rB;
        if(idx==fDampPresetCache && std::abs(decayV-fDampDecayCache)<1e-4f
           && std::abs(bdSum-fDampBandSumCache)<1e-4f
           && std::abs(physSum-fDampPhysSumCache)<1e-4f) return;
        fDampPresetCache=idx; fDampDecayCache=decayV; fDampBandSumCache=bdSum; fDampPhysSumCache=physSum;
        const auto& pr = modal::kPresets[idx];
        const int mtx=std::clamp((int)std::lround(mTgt*(float)mx),0,mx);
        const auto& pt = modal::kPresets[mtx];
        const int tN=std::max(1,pt.n);
        // mirror the engine's setDecayScale curve: larger v = longer tail
        // (smaller rate multiplier). The display tracks 1/(decay*scale).
        const float scale = 0.1f * std::pow(100.f, 1.f - decayV);
        // wave-3 live factors (engine mirrors): support + hold raise the rate,
        // Rayleigh adds the absolute law 0.5*(a + b*w^2) at the band-mid freq.
        const float supMul = 1.f + 0.5f*sup;
        float edge=std::max(std::fabs(paramCache[PluginMultiScaleBody::kParamStrikeX]-0.5f),
                            std::fabs(paramCache[PluginMultiScaleBody::kParamStrikeY]-0.5f))*2.f;
        const float holdMul = 1.f + 2.f*hd*std::max(0.f,1.f-edge);
        const float rayA2 = 5.f*rA*rA, rayB2 = 1.2436e-4f*rB*rB;
        // per-band mean decay, per-band T60
        float bandT60[16]={};
        float maxT60=1e-6f;
        for(int b=0;b<16;++b){
            int m0 = (b*pr.n)/16;
            int m1 = ((b+1)*pr.n)/16;
            if(m0>=m1) continue;   // empty band (e.g. Bar preset bands 8..15)
            double sum=0.0, fsum=0.0; int cnt=0;
            for(int m=m0;m<m1;++m){
                // wave-3: morphed base decay (resolution + body morph, target
                // index clamped to the target's mode count like the engine)
                float dm = pr.decay[m];
                if(rm>1e-4f) dm = dm + (pr.fineDecay[m]-dm)*rm;
                if(mAmt>1e-4f){ const int ti=std::min(m,tN-1); dm = dm + (pt.decay[ti]-dm)*mAmt; }
                float fm = pr.freq[m];
                if(rm>1e-4f) fm = fm + (pr.fineFreq[m]-fm)*rm;
                if(mAmt>1e-4f){ const int ti=std::min(m,tN-1); fm = fm + (pt.freq[ti]-fm)*mAmt; }
                sum += std::max(0.2f, dm); fsum += fm; ++cnt;
            }
            float meanRate = (float)(sum/(double)cnt);
            float meanF = (float)(fsum/(double)cnt);
            // effective rate: DECAY knob x band trim x support x hold, plus
            // the Rayleigh absolute term at the band-mid frequency
            float rate = meanRate * scale * supMul * holdMul + rayA2 + rayB2*meanF*meanF;
            float t60 = 6.9078f / (rate * bandTrim[b]);
            bandT60[b]=t60;
            if(t60>maxT60) maxT60=t60;
        }
        fDampMax=maxT60;
        // paint bars + values
        for(int b=0;b<16;++b){
            int m0=(b*pr.n)/16;
            int m1=((b+1)*pr.n)/16;
            char vbuf[16];
            if(m0>=m1){
                lv_bar_set_value(fDampBars[b],0,LV_ANIM_OFF);
                snprintf(vbuf,sizeof(vbuf),"-");
            } else {
                int v = (int)std::lround(1000.f*bandT60[b]/maxT60);
                lv_bar_set_value(fDampBars[b],v,LV_ANIM_OFF);
                // display seconds with 1- or 2-decimal precision depending
                // on magnitude; cap at 99.9s so 4-digit formats never blow
                // the 40px value cell.
                if(bandT60[b]>=10.f)      snprintf(vbuf,sizeof(vbuf),"%.0f s",bandT60[b]);
                else if(bandT60[b]>=1.f)  snprintf(vbuf,sizeof(vbuf),"%.1f s",bandT60[b]);
                else                      snprintf(vbuf,sizeof(vbuf),"%.2f s",bandT60[b]);
            }
            lv_label_set_text(fDampVals[b],vbuf);
        }
    }
    // wave-3 (idea 5): disc heatmap painter. Dots are tracked in fHeatDots
    // for deletion. Focused mode (>=0): per-cell |gain| of THAT mode, so its
    // vibration nodes read as gaps ("hit the gaps to mute it"); otherwise the
    // aggregate max over active modes (the original heatmap). Gains blend
    // toward the morph target exactly like the engine's noteOn path.
    void paintDiscHeatmap(){
        for(int i=0;i<fHeatCount;++i) if(fHeatDots[i]&&lv_obj_is_valid(fHeatDots[i])) lv_obj_del(fHeatDots[i]);
        fHeatCount=0;
        if(!strikeDisc) return;
        lv_coord_t D=lv_obj_get_width(strikeDisc);
        if(D<=0) return;
        using namespace modal;
        int preset=(int)std::round(paramCache[PluginMultiScaleBody::kParamPreset]*(float)(kNumPresets-1));
        const auto& pr=kPresets[std::clamp(preset,0,kNumPresets-1)];
        float mAmt=paramCache[PluginMultiScaleBody::kParamMorphAmt];
        const int mtx=std::clamp((int)std::lround(paramCache[PluginMultiScaleBody::kParamMorphTarget]*(float)(kNumPresets-1)),0,kNumPresets-1);
        const auto& pt=kPresets[mtx];
        const int tN=std::max(1,pt.n);
        const int focus=std::clamp(fModeFocus,-1,kMaxModes-1);
        const int GS=10;
        const int dotS=3;
        const int margin=(int)(D*0.10f);
        const int inner=D-margin*2;
        const int cell=inner/GS;
        int cellPeak[GS*GS]={0};
        float gMax=1e-9f;
        for(int gy=0;gy<GS;++gy){
            for(int gx=0;gx<GS;++gx){
                float gxN=(gx+0.5f)/(float)GS;
                float gyN=(gy+0.5f)/(float)GS;
                float fx=gxN*14.f, fy=gyN*14.f;
                int x0=std::clamp((int)fx,0,14), y0=std::clamp((int)fy,0,14);
                int x1=x0+1, y1=y0+1; float dx=fx-x0, dy=fy-y0;
                float w00=(1-dx)*(1-dy), w10=dx*(1-dy), w01=(1-dx)*dy, w11=dx*dy;
                float pk=0.f;
                int n=std::clamp((int)(8+paramCache[PluginMultiScaleBody::kParamModeCount]*120.f),8,pr.n);
                if(focus>=0 && focus<n){
                    float g=pr.gain[focus][y0][x0]*w00+pr.gain[focus][y0][x1]*w10
                           +pr.gain[focus][y1][x0]*w01+pr.gain[focus][y1][x1]*w11;
                    if(mAmt>1e-4f){
                        const int ti=std::min(focus,tN-1);
                        float tg=pt.gain[ti][y0][x0]*w00+pt.gain[ti][y0][x1]*w10
                               +pt.gain[ti][y1][x0]*w01+pt.gain[ti][y1][x1]*w11;
                        g=g+(tg-g)*mAmt;
                    }
                    pk=std::fabs(g);
                } else {
                    for(int m=0;m<n;++m){
                        float g=pr.gain[m][y0][x0]*w00+pr.gain[m][y0][x1]*w10
                               +pr.gain[m][y1][x0]*w01+pr.gain[m][y1][x1]*w11;
                        if(mAmt>1e-4f){
                            const int ti=std::min(m,tN-1);
                            float tg=pt.gain[ti][y0][x0]*w00+pt.gain[ti][y0][x1]*w10
                                   +pt.gain[ti][y1][x0]*w01+pt.gain[ti][y1][x1]*w11;
                            g=g+(tg-g)*mAmt;
                        }
                        int band=(m*16)/n;
                        float trim=paramCache[PluginMultiScaleBody::kParamBand0+std::clamp(band,0,15)]*2.f;
                        float v=std::fabs(g)*trim;
                        if(v>pk) pk=v;
                    }
                }
                cellPeak[gy*GS+gx]=(int)(pk*1000.f);
                if(pk>gMax) gMax=pk;
            }
        }
        const bool hl=(focus>=0);
        for(int gy=0;gy<GS;++gy){
            for(int gx=0;gx<GS;++gx){
                float v=(float)cellPeak[gy*GS+gx]/1000.f / gMax;
                if(v<0.02f) continue;
                if(fHeatCount>=100) return;
                lv_obj_t* dot=lv_obj_create(strikeDisc);
                lv_obj_set_size(dot, scaled(dotS), scaled(dotS));
                lv_obj_set_pos(dot, margin + gx*cell + (cell-scaled(dotS))/2,
                                   margin + gy*cell + (cell-scaled(dotS))/2);
                lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_color(dot, hl?PLATE_AMBER_PALE:COL_HIGHLIGHT, 0);
                // R4: raised floor/ceiling (was LV_OPA_10..LV_OPA_70) -
                // the R3 critic read the faint 2px dots as an "empty
                // dotted canvas" at the 1600x1000 staging scale.
                lv_obj_set_style_bg_opa(dot, (lv_opa_t)(LV_OPA_20 + v*(LV_OPA_90-LV_OPA_20)), 0);
                lv_obj_set_style_border_width(dot, 0, 0);
                lv_obj_set_style_pad_all(dot, 0, 0);
                lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
                fHeatDots[fHeatCount++]=dot;
            }
        }
    }
    // wave-3 (idea 5): MODE MAP bar click toggles the node-focus overlay.
    // Click the same bar again (or strike a peak) to clear.
    static void modeBarCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        lv_obj_t* bar=(lv_obj_t*)lv_event_get_target(e);
        if(!ui||!bar) return;
        int m=(int)(intptr_t)lv_obj_get_user_data(bar);
        if(m<0||m>=modal::kMaxModes) return;
        ui->fModeFocus=(ui->fModeFocus==m)?-1:m;
        ui->fHeatDirty=true;
        if(ui->fModePeakLbl){
            char nm[32];
            if(ui->fModeFocus>=0) snprintf(nm,sizeof(nm),"M%d NODE",ui->fModeFocus+1);
            else {
                int mxp=modal::kNumPresets-1;
                int preset=(int)std::round(ui->paramCache[PluginMultiScaleBody::kParamPreset]*(float)mxp);
                const auto& pr=modal::kPresets[std::clamp(preset,0,mxp)];
                int pi=std::clamp(ui->fModePeakIdx,0,modal::kMaxModes-1);
                snprintf(nm,sizeof(nm),"M%d  -  %.0f HZ",ui->fModePeakIdx+1,pr.freq[pi]/(2.f*3.14159f));
            }
            lv_label_set_text(ui->fModePeakLbl,nm);
        }
    }
    void syncPresetDropdown(float v){
        if(!presetDropdown) return;
        int mx = modal::kNumPresets - 1;
        int idx = (int)std::round(v*(float)mx); idx=std::clamp(idx,0,mx);
        lv_dropdown_set_selected(presetDropdown, idx);
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
            case P::kParamVolume: {
                // squared-law gain (engine applies v*v): dB = 40*log10(v)
                if(v>=0.9995f) snprintf(buf,cap,"0.0 dB");
                else if(v<=0.001f) snprintf(buf,cap,"-INF dB");
                else snprintf(buf,cap,"%+.1f dB",20.f*std::log10(v*v));
                break; }
            case P::kParamModeCount: snprintf(buf,cap,"%d",8+(int)(v*120.f)); break;
            case P::kParamPitch:     snprintf(buf,cap,"%+.1f ST",(v-0.5f)*48.f); break;
            case P::kParamBow:       snprintf(buf,cap,"%d %%",(int)std::lround(v*100.f)); break;
            case P::kParamDamper:    snprintf(buf,cap,"%d %%",(int)std::lround(v*100.f)); break;
            case P::kParamInharm:    snprintf(buf,cap,"x%.2f",1.f+v); break;
            case P::kParamSlideMode: {
                static const char* const kSlideNames[3]={"PITCH","MODE","BRIGHT"};
                const int m=std::clamp((int)std::lround(v*2.f),0,2);
                snprintf(buf,cap,"%s",kSlideNames[m]);
                break; }
            case P::kParamSupport:   snprintf(buf,cap,"%d %%",(int)std::lround(v*100.f)); break;
            case P::kParamHoldDamp:  snprintf(buf,cap,"%d %%",(int)std::lround(v*100.f)); break;
            case P::kParamResMorph:  snprintf(buf,cap,"%d %%",(int)std::lround(v*100.f)); break;
            case P::kParamMorphAmt:   snprintf(buf,cap,"%d %%",(int)std::lround(v*100.f)); break;
            case P::kParamMorphTarget: {
                const int t=std::clamp((int)std::lround(v*(float)(modal::kNumPresets-1)),0,modal::kNumPresets-1);
                snprintf(buf,cap,"%s",modal::kPresets[t].name);
                break; }
            case P::kParamMaterial: {
                static const char* const kMatNames[11]={"DEFAULT","ALUMINIUM","STEEL","BRONZE","PINE","ROSEWOOD","MAHOGANY","GLASS","BRASS","TITANIUM","CARBON"};
                const int m=std::clamp((int)std::lround(v*10.f),0,10);
                snprintf(buf,cap,"%s",kMatNames[m]);
                break; }
            case P::kParamRayleighA: snprintf(buf,cap,"%.2f",v); break;
            case P::kParamRayleighB: snprintf(buf,cap,"%.2f",v); break;
            case P::kParamEcoMode:   snprintf(buf,cap,"%s",v>0.5f?"ECO":"OFF"); break;
            case P::kParamEcoBudget: snprintf(buf,cap,"%d",(int)std::lround(64.f+v*896.f)); break;
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
        // Master arc writes its owned beside-arc label (the widget's own
        // built-in labels are hidden) with the formatted dB readout.
        if(pi==PluginMultiScaleBody::kParamVolume && ui->fMasterValLbl){
            char b[24]; ui->formatParamValue(pi,lv_arc_get_value(arc)/1000.f,b,sizeof(b));
            lv_label_set_text(ui->fMasterValLbl,b);
            return;
        }
        auto fIt=gArcVisualBindings.find(arc);
        lv_obj_t* lbl=(fIt!=gArcVisualBindings.end())?fIt->second.valueLabel:nullptr;
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
        // wave-3 (idea 10): capture the drag path while REC is armed
        if(ui->fRecOn && ui->fRecN<512){
            bool dup=false;
            if(ui->fRecN>0){
                float dx=fx-ui->fRecX[ui->fRecN-1], dy=fy-ui->fRecY[ui->fRecN-1];
                dup=(dx*dx+dy*dy)<(0.004f*0.004f);
            }
            if(!dup){ ui->fRecX[ui->fRecN]=fx; ui->fRecY[ui->fRecN]=fy; ++ui->fRecN; }
        }
        auto code = lv_event_get_code(e);
        if(code==LV_EVENT_PRESSED || code==LV_EVENT_PRESSING){
            if(code==LV_EVENT_PRESSED){
                ui->editParameter(PluginMultiScaleBody::kParamStrikeX,true);
                ui->editParameter(PluginMultiScaleBody::kParamStrikeY,true);
                ui->spawnMalletPulse();   // visual hit confirmation
                // round-2 audit trail: a small amber dot persists at the
                // strike point for ~0.5s, fading out via the spectrum timer
                ui->placeLastStrike(p.x - coords.x1, p.y - coords.y1);
                // physical hit: strike position first, then trigger the body.
                // MPE: each strike takes the next member channel (1..15) so
                // the hit is its own MPE note - per-note bend/pressure from
                // the host applies per channel and voices key note+channel.
                // DPF UI can only send notes (no pressure/bend send), so the
                // strike velocity carries the gesture (edge = harder).
                ui->fStrikeChannel=ui->fNextStrikeChannel;
                ui->fNextStrikeChannel=(ui->fNextStrikeChannel%15)+1;
                ui->sendNote((uint8_t)ui->fStrikeChannel,(uint8_t)ui->fStrikeNote,100);
                ui->fStrikeHeld=true;
            }
            ui->setParamValue(PluginMultiScaleBody::kParamStrikeX, fx);
            ui->setParamValue(PluginMultiScaleBody::kParamStrikeY, fy);
        } else if(code==LV_EVENT_RELEASED || code==LV_EVENT_PRESS_LOST){
            if(ui->fStrikeHeld){ ui->sendNote((uint8_t)ui->fStrikeChannel,(uint8_t)ui->fStrikeNote,0); ui->fStrikeHeld=false; }
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
            const int pIdx = ui->fScrubMode
                ? (int)PluginMultiScaleBody::kParamBandDecay0+band
                : (int)PluginMultiScaleBody::kParamBand0+band;
            if(band!=ui->fScrubBand){
                // drag crossed into another band: close the old edit bracket,
                // open the new one (RELEASED closes by member, not by position)
                if(ui->fScrubParamIdx>=0) ui->editParameter((uint32_t)ui->fScrubParamIdx,false);
                ui->fScrubBand=band;
                ui->fScrubParamIdx=pIdx;
                ui->fScrubLevel=level;
                ui->editParameter((uint32_t)pIdx,true);
                ui->setParamValue((uint32_t)pIdx, level);
            } else if(level!=ui->fScrubLevel){   // same band: write only on change
                ui->fScrubLevel=level;
                ui->setParamValue((uint32_t)pIdx, level);
            }
        } else if(code==LV_EVENT_RELEASED || code==LV_EVENT_PRESS_LOST){
            if(ui->fScrubParamIdx>=0){
                ui->editParameter((uint32_t)ui->fScrubParamIdx,false);
                ui->fScrubParamIdx=-1;
                ui->fScrubBand=-1;
            }
        }
    }
    void updateKeyboardNotes(){
        if(!kbContainer) return;
        static const int whiteOff[7]={0,2,4,5,7,9,11};
        static const int blackOff[5]={1,3,6,8,10};
        static const char* whiteName[7]={"C","D","E","F","G","A","B"};
        for(int i=0;i<lay::KEY_WHITE_N;++i){
            if(!kbWhite[i]) continue;
            const int o=i/7, k=i%7;
            int note = kbBaseNote + o*12 + whiteOff[k];
            note = std::clamp(note,0,127);
            lv_obj_set_user_data(kbWhite[i], (void*)(intptr_t)note);
            lv_obj_t* child = lv_obj_get_child(kbWhite[i],0);
            if(child && lv_obj_check_type(child,&lv_label_class)){
                int oct = note/12 -1;
                char buf[8]; snprintf(buf,sizeof(buf),"%s%d",whiteName[k],oct);
                lv_label_set_text(child,buf);
            }
        }
        for(int i=0;i<lay::KEY_BLACK_N;++i){
            if(!kbBlack[i]) continue;
            const int o=i/5, k=i%5;
            int note = kbBaseNote + o*12 + blackOff[k];
            note = std::clamp(note,0,127);
            lv_obj_set_user_data(kbBlack[i], (void*)(intptr_t)note);
        }
        if(kbOctLabel){
            int oct = kbBaseNote/12 -1;
            char buf[16]; snprintf(buf,sizeof(buf),"C%d - B%d",oct,oct+lay::KEY_OCTAVES-1);
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
        int next=ui->kbBaseNote+dir; next=std::clamp(next,24,92); next=(next/12)*12;   // 92 -> floor C6: top key B6 = 119 <= 127
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
    // Window size = base plate * zoom%, so the 1440:990 aspect holds EXACTLY
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
        // wave-2: excitation character (bow/exciter-led presets stay rare so
        // most rolls keep the classic mallet attack; dispatch snaps SlideMode)
        set(PluginMultiScaleBody::kParamBow, (std::rand()%100)<30 ? rnd(0.3f,0.8f) : 0.f);
        set(PluginMultiScaleBody::kParamDamper, rnd(0.f,0.5f));
        set(PluginMultiScaleBody::kParamInharm, rnd(0.f,0.4f));
        { static const float modes[3]={0.f,0.5f,1.f}; set(PluginMultiScaleBody::kParamSlideMode, modes[std::rand()%3]); }
        for(int b=0;b<16;++b)
            set(PluginMultiScaleBody::kParamBandDecay0+b, std::clamp(rnd(0.35f,0.65f),0.f,1.f));
        // wave-3: physical model (morphs/materials biased to off/DEFAULT so
        // rolls stay musical; dispatch snaps MorphTarget/Material to steps)
        set(PluginMultiScaleBody::kParamSupport, rnd(0.f,0.5f));
        set(PluginMultiScaleBody::kParamHoldDamp, rnd(0.f,0.5f));
        set(PluginMultiScaleBody::kParamResMorph, rnd(0.f,1.f));
        set(PluginMultiScaleBody::kParamMorphTarget, (float)(std::rand()%(mx+1))/(float)mx);
        set(PluginMultiScaleBody::kParamMorphAmt, (std::rand()%100)<25 ? rnd(0.2f,0.6f) : 0.f);
        set(PluginMultiScaleBody::kParamMaterial, (std::rand()%100)<25 ? (float)(1+std::rand()%10)/10.f : 0.f);
        set(PluginMultiScaleBody::kParamRayleighA, rnd(0.f,0.3f));
        set(PluginMultiScaleBody::kParamRayleighB, rnd(0.f,0.25f));
        set(PluginMultiScaleBody::kParamEcoMode, (std::rand()%100)<20 ? 1.f : 0.f);
        set(PluginMultiScaleBody::kParamEcoBudget, rnd(0.3f,0.7f));
    }

    // ---- idea 15: MIDI learn (right-click a knob -> next CC binds) ---------
    // Resolution uses the exact LVGL click path (lv_indev_search_obj, the
    // same search the pointer indev runs) and walks up until an lv_arc is
    // found — only knobs carry a param index in user_data; buttons and
    // decorative objects can't be mistaken for them. Right-click anywhere
    // else (or a second right-click) cancels.
    void handleRightClick(int wx,int wy){
        lv_obj_t* scr=lv_screen_active();
        if(!scr) return;
        lv_point_t pt={ (lv_coord_t)wx, (lv_coord_t)wy };
        lv_obj_t* o=lv_indev_search_obj(scr,&pt);
        lv_obj_t* arc=o;
        while(arc && !lv_obj_check_type(arc,&lv_arc_class)) arc=lv_obj_get_parent(arc);
        if(!arc){ cancelLearn(); return; }   // empty space (or 2nd click) cancels
        const intptr_t ud=(intptr_t)lv_obj_get_user_data(arc);
        if(ud<0 || ud>=(intptr_t)PluginMultiScaleBody::kNumInputParams){ cancelLearn(); return; }
        armLearn((int)ud);
    }
    // wave-4: MIDI learn arms a non-blocking keyboard-header chip instead of
    // a full-screen shield — knobs stay playable while waiting for the CC.
    void armLearn(int pi){
        fLearnParam=pi;
        if(fLearnLbl){
            char b[64];
            snprintf(b,sizeof(b),"LEARN %s - move CC",parameterName((uint32_t)pi).c_str());
            lv_label_set_text(fLearnLbl,b);
        }
        if(fLearnChip) lv_obj_clear_flag(fLearnChip,LV_OBJ_FLAG_HIDDEN);
        setState("learn", std::to_string(pi).c_str());   // arm the plugin
    }
    static void learnCancelCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        if(ui) ui->cancelLearn();
    }
    void cancelLearn(){
        if(fLearnChip) lv_obj_add_flag(fLearnChip,LV_OBJ_FLAG_HIDDEN);
        if(fLearnParam>=0){
            setState("learn","");   // disarm the plugin's pending learn
            fLearnParam=-1;
        }
    }
    // ---- idea 13: microtonal scale editor (EDO selectors + .scl file load) ----
    // N-EDO menu index for a cached scale text ("<n>-edo" name line), -1 if not
    int autoEdoOf(const std::string& txt) const {
        static const int kEdos[13]={5,7,10,12,15,17,19,22,24,31,41,53,72};
        if(txt.empty()) return 3;                       // 12-EDO
        size_t p=txt.find("-edo");
        if(p==std::string::npos || p>4) return -1;
        const int n=std::atoi(txt.substr(0,p).c_str());
        for(int i=0;i<13;++i) if(kEdos[i]==n) return i;
        return -1;
    }
    static void edoDropdownCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        lv_obj_t* dd=(lv_obj_t*)lv_event_get_target(e);
        if(!ui||!dd) return;
        static const int kEdos[13]={5,7,10,12,15,17,19,22,24,31,41,53,72};
        const int sel=lv_dropdown_get_selected(dd);
        if(sel<0||sel>=13) return;
        ui->scaleTxtCached_=edoSclText(kEdos[sel]);
        ui->setState("scale",ui->scaleTxtCached_.c_str());   // plugin generates the table
        ui->updateScaleLabel();
    }
    static void scaleLoadCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        if(!ui) return;
        std::string txt;
        if(!loadSclFileDialog(txt)) return;              // cancelled
        ui->scaleTxtCached_=txt;
        ui->setState("scale",txt.c_str());
        ui->updateScaleLabel();
        if(ui->fEdoDropdown){
            const int idx=ui->autoEdoOf(txt);
            if(idx>=0) lv_dropdown_set_selected(ui->fEdoDropdown,idx);
        }
    }
    static void scaleClearCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        if(!ui) return;
        ui->scaleTxtCached_="";
        ui->setState("scale","");
        ui->updateScaleLabel();
        if(ui->fEdoDropdown) lv_dropdown_set_selected(ui->fEdoDropdown,3); // 12-EDO
    }
    static void materialDdCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        lv_obj_t* dd=(lv_obj_t*)lv_event_get_target(e);
        if(!ui||!dd) return;
        int sel=std::clamp((int)lv_dropdown_get_selected(dd),0,10);
        ui->editParameter(PluginMultiScaleBody::kParamMaterial,true);
        ui->setParamValue(PluginMultiScaleBody::kParamMaterial,(float)sel/10.f);
        ui->editParameter(PluginMultiScaleBody::kParamMaterial,false);
    }
    static void morphDdCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        lv_obj_t* dd=(lv_obj_t*)lv_event_get_target(e);
        if(!ui||!dd) return;
        int mx=modal::kNumPresets-1;
        int sel=std::clamp((int)lv_dropdown_get_selected(dd),0,mx);
        float v=mx?(float)sel/(float)mx:0.f;
        ui->editParameter(PluginMultiScaleBody::kParamMorphTarget,true);
        ui->setParamValue(PluginMultiScaleBody::kParamMorphTarget,v);
        ui->editParameter(PluginMultiScaleBody::kParamMorphTarget,false);
    }
    static void ecoBtnCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        lv_obj_t* btn=(lv_obj_t*)lv_event_get_target(e);
        if(!ui||!btn) return;
        bool on=lv_obj_has_state(btn,LV_STATE_CHECKED);
        ui->editParameter(PluginMultiScaleBody::kParamEcoMode,true);
        ui->setParamValue(PluginMultiScaleBody::kParamEcoMode,on?1.f:0.f);
        ui->editParameter(PluginMultiScaleBody::kParamEcoMode,false);
    }
    // ---- spectrum scrub target (idea 2): GAIN vs DECAY ---------------------
    static void scrubToggleCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        lv_obj_t* btn=(lv_obj_t*)lv_event_get_target(e);
        if(!ui||!btn) return;
        ui->fScrubMode=!ui->fScrubMode;
        lv_obj_t* lbl=lv_obj_get_child(btn,0);
        if(lbl && lv_obj_check_type(lbl,&lv_label_class))
            lv_label_set_text(lbl, ui->fScrubMode?"DECAY":"GAIN");
    }

    // ---- wave-3 (idea 10): disc motion recorder ------------------------------
    // REC arms capture of the drag path (up to 512 points at ~33 ms cadence);
    // PLAY replays it as timed strikes with velocity from gesture speed.
    static void recBtnCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        lv_obj_t* btn=(lv_obj_t*)lv_event_get_target(e);
        if(!ui||!btn) return;
        if(ui->fRecPlaying) ui->stopPlayback();
        ui->fRecOn=!ui->fRecOn;
        if(ui->fRecOn) ui->fRecN=0;
        if(ui->fRecOn) lv_obj_add_state(btn,LV_STATE_CHECKED);
        else lv_obj_clear_state(btn,LV_STATE_CHECKED);
    }
    static void playBtnCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        lv_obj_t* btn=(lv_obj_t*)lv_event_get_target(e);
        if(!ui||!btn) return;
        if(ui->fRecPlaying){ ui->stopPlayback(); return; }
        if(ui->fRecN<=0) return;
        ui->fRecOn=false;
        if(ui->fRecBtn) lv_obj_clear_state(ui->fRecBtn,LV_STATE_CHECKED);
        ui->fRecPlaying=true; ui->fRecCursor=0; ui->fPlayHeld=false;
        ui->editParameter(PluginMultiScaleBody::kParamStrikeX,true);
        ui->editParameter(PluginMultiScaleBody::kParamStrikeY,true);
        lv_obj_add_state(btn,LV_STATE_CHECKED);
    }
    static void recClearCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        if(!ui) return;
        if(ui->fRecPlaying) ui->stopPlayback();
        ui->fRecOn=false; ui->fRecN=0;
        if(ui->fRecBtn) lv_obj_clear_state(ui->fRecBtn,LV_STATE_CHECKED);
    }
    void stopPlayback(){
        if(fPlayHeld){ sendNote((uint8_t)fPlayChannel,(uint8_t)fStrikeNote,0); fPlayHeld=false; }
        fRecPlaying=false;
        editParameter(PluginMultiScaleBody::kParamStrikeX,false);
        editParameter(PluginMultiScaleBody::kParamStrikeY,false);
        if(fPlayBtn) lv_obj_clear_state(fPlayBtn,LV_STATE_CHECKED);
    }
    // one playback step per 33 ms tick: strike the next recorded point with
    // velocity from the gesture speed between consecutive points.
    void playbackTick(){
        if(!fRecPlaying) return;
        if(fRecCursor>=fRecN){ stopPlayback(); return; }
        if(fPlayHeld){ sendNote((uint8_t)fPlayChannel,(uint8_t)fStrikeNote,0); fPlayHeld=false; }
        float px=fRecX[fRecCursor], py=fRecY[fRecCursor];
        float vel=100.f;
        if(fRecCursor>0){
            float dx=px-fRecX[fRecCursor-1], dy=py-fRecY[fRecCursor-1];
            float dist=std::sqrt(dx*dx+dy*dy);
            vel=std::clamp(30.f+dist*400.f,30.f,127.f);
        }
        setParamValue(PluginMultiScaleBody::kParamStrikeX,px);
        setParamValue(PluginMultiScaleBody::kParamStrikeY,py);
        fPlayChannel=fNextStrikeChannel;
        fNextStrikeChannel=(fNextStrikeChannel%15)+1;
        sendNote((uint8_t)fPlayChannel,(uint8_t)fStrikeNote,(uint8_t)vel);
        fPlayHeld=true;
        ++fRecCursor;
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
        lv_obj_t* kbTitle=addLabel(kbHead,"KEYBOARD  -  3 OCTAVES  -  CLICK TO AUDITION",getScaledMicroFont(),COL_HIGHLIGHT,2);
        lv_obj_set_style_text_opa(kbTitle,LV_OPA_80,0);
        // wave-4: scale status only (the tuning controls live inline now — no modal)
        lv_obj_t* scaleCluster=makeRow(kbHead,scaled(150),scaled(lay::BTN_H),scaled(8),LV_FLEX_ALIGN_CENTER);
        fScaleLbl=addLabel(scaleCluster,"SCALE: 12-EDO",getScaledMicroFont(),PLATE_TEXT_DIM,1);
        {
            const char* sn=scaleName(scaleTxtCached_).c_str();
            char bu[48]; snprintf(bu,sizeof(bu),"SCALE: %s",sn);
            lv_label_set_text(fScaleLbl,bu);
        }
        // wave-4: tuning selects inline — EDO applies immediately, LOAD reads
        // a .scl file, CLEAR restores 12-EDO (same handlers as the old menu)
        lv_obj_t* selCluster=makeRow(kbHead,scaled(lay::KB_SEL_W),scaled(lay::BTN_H),scaled(8),LV_FLEX_ALIGN_CENTER);
        fEdoDropdown=lv_dropdown_create(selCluster);
        lv_dropdown_set_options(fEdoDropdown,"5\n7\n10\n12\n15\n17\n19\n22\n24\n31\n41\n53\n72");
        lv_dropdown_set_selected(fEdoDropdown,3); // 12-EDO default
        lv_obj_set_width(fEdoDropdown,scaled(lay::MODEL_EDO_W));
        lv_obj_add_style(fEdoDropdown,&styles.compactSelectMain,0);
        lv_obj_set_style_bg_color(fEdoDropdown,PLATE_WELL,0);
        lv_obj_set_style_border_color(fEdoDropdown,PLATE_EDGE,0);
        lv_obj_set_style_radius(fEdoDropdown,scaled(lay::RADIUS_SM),0);
        { lv_obj_t* list=lv_dropdown_get_list(fEdoDropdown);
          if(list){ lv_obj_add_style(list,&styles.compactSelectListMain,0);
                    lv_obj_set_style_max_height(list,scaled(lay::DROPDOWN_MAX_ROWS*lay::DROPDOWN_ROW_H),0); } }
        lv_group_remove_obj(fEdoDropdown);
        lv_obj_add_event_cb(fEdoDropdown,edoDropdownCb,LV_EVENT_VALUE_CHANGED,this);
        if(autoEdoOf(scaleTxtCached_)>=0) lv_dropdown_set_selected(fEdoDropdown,autoEdoOf(scaleTxtCached_));
        lv_obj_t* sclLoad=addButton(selCluster,lay::MODEL_LOAD_W,lay::BTN_H,"LOAD .SCL",COL_HIGHLIGHT);
        lv_obj_add_event_cb(sclLoad,scaleLoadCb,LV_EVENT_CLICKED,this);
        lv_obj_t* sclClear=addButton(selCluster,lay::MODEL_CLEAR_W,lay::BTN_H,"CLEAR",PLATE_TEXT_MID);
        lv_obj_add_event_cb(sclClear,scaleClearCb,LV_EVENT_CLICKED,this);
        // wave-4: MIDI-learn status chip (non-blocking; replaces the modal shield)
        fLearnChip=makeRow(kbHead,scaled(lay::LEARN_CHIP_W),scaled(lay::BTN_H),scaled(8),LV_FLEX_ALIGN_CENTER);
        fLearnLbl=addLabel(fLearnChip,"LEARN",getScaledMicroFont(),COL_HIGHLIGHT,1);
        lv_obj_t* learnX=addButton(fLearnChip,28,lay::BTN_H,"X",PLATE_TEXT_MID);
        lv_obj_add_event_cb(learnX,learnCancelCb,LV_EVENT_CLICKED,this);
        lv_obj_add_flag(fLearnChip,LV_OBJ_FLAG_HIDDEN);
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
        kbOctLabel=lv_label_create(octRow); lv_label_set_text(kbOctLabel,"C3 - B5");
        lv_obj_set_width(kbOctLabel,scaled(lay::OCTLBL_W));
        lv_obj_set_style_text_align(kbOctLabel,LV_TEXT_ALIGN_CENTER,0);
        lv_obj_set_style_text_color(kbOctLabel,COL_TEXT_DIM,0);
        lv_obj_set_style_text_font(kbOctLabel,getScaledMicroFont(),0);
        lv_obj_t* octUp=lv_btn_create(octRow); lv_obj_set_size(octUp,scaled(lay::BTN_W_OCT),scaled(lay::BTN_H)); lv_obj_add_style(octUp,&styles.btnMain,0); lv_obj_add_style(octUp,&styles.btnHovered,LV_STATE_HOVERED); lv_obj_add_style(octUp,&styles.btnPressed,LV_STATE_PRESSED); lv_obj_set_style_radius(octUp,scaled(lay::RADIUS_SM),0); lv_obj_set_style_pad_all(octUp,0,0);
        lv_obj_set_user_data(octUp,(void*)(intptr_t)12); lv_obj_add_event_cb(octUp,octaveBtnCb,LV_EVENT_CLICKED,this);
        lv_obj_t* ul=lv_label_create(octUp); lv_label_set_text(ul,">"); lv_obj_center(ul); lv_obj_set_style_text_color(ul,COL_TEXT,0);
        // keys row: the keybed IS the footer width now - 3 octaves (C3-B5),
        // 21 white keys derived from lay::KEY_WHITE_W so the comb fills the
        // strip inner exactly (round-6: the old fixed 404px keybed sat
        // centered with ~500px dead either side - the user read the footer
        // as "keyboard doesn't extend over the full footer").
        lv_obj_t* kbRow=makeRow(kbContainer,lv_pct(100),scaled(lay::KEY_H),0,LV_FLEX_ALIGN_CENTER);
        // keybed arithmetic @s=1: 21 white x 64 + 20 x 2 gap = 1384 wide
        const int whiteW=scaled(lay::KEY_WHITE_W), whiteH=scaled(lay::KEY_H);
        const int blackW=scaled(lay::KEY_BLACK_W), blackH=scaled(lay::KEY_BLACK_H);
        const int gap=scaled(lay::KEY_GAP);
        const int keysW=lay::KEY_WHITE_N*whiteW+(lay::KEY_WHITE_N-1)*gap;
        lv_obj_t* keysBox=makeBox(kbRow,keysW,whiteH);
        lv_obj_set_layout(keysBox,LV_LAYOUT_NONE);
        static const char* whiteName[7]={"C","D","E","F","G","A","B"}; static const int whiteOff[7]={0,2,4,5,7,9,11}; static const int blackOff[5]={1,3,6,8,10};
        const int stepW=whiteW+gap;
        for(int i=0;i<lay::KEY_WHITE_N;++i){
            lv_obj_t* w=lv_btn_create(keysBox); lv_obj_set_size(w,whiteW,whiteH); lv_obj_set_pos(w,i*stepW,0);
            lv_obj_set_style_bg_color(w,COL_KNOB,0); lv_obj_set_style_bg_color(w,COL_KNOB_LIGHT,LV_STATE_HOVERED); lv_obj_set_style_bg_opa(w,LV_OPA_COVER,0); lv_obj_set_style_border_color(w,COL_HAIRLINE,0); lv_obj_set_style_border_width(w,1,0); lv_obj_set_style_radius(w,scaled(5),0); lv_obj_set_style_pad_all(w,0,0);
            const int note=kbBaseNote+(i/7)*12+whiteOff[i%7]; lv_obj_set_user_data(w,(void*)(intptr_t)note);
            lv_obj_add_event_cb(w,keyEventCb,LV_EVENT_PRESSED,this); lv_obj_add_event_cb(w,keyEventCb,LV_EVENT_RELEASED,this); lv_obj_add_event_cb(w,keyEventCb,LV_EVENT_PRESS_LOST,this); lv_obj_add_event_cb(w,keyEventCb,LV_EVENT_LEAVE,this);
            kbWhite[i]=w; lv_obj_t* lbl=lv_label_create(w); int oct=note/12-1; char buf[8]; snprintf(buf,sizeof(buf),"%s%d",whiteName[i%7],oct); lv_label_set_text(lbl,buf);
            lv_obj_add_style(lbl,&styles.labelSmall,0); lv_obj_set_style_text_color(lbl,COL_PANEL_DARK,0); lv_obj_set_style_text_font(lbl,getScaledMicroFont(),0); lv_obj_align(lbl,LV_ALIGN_BOTTOM_MID,0,-scaled(4)); lv_obj_clear_flag(lbl,LV_OBJ_FLAG_CLICKABLE);
        }
        for(int i=0;i<lay::KEY_BLACK_N;++i){
            const int o=i/5, k=i%5;
            // black key sits on the boundary after white {0,1,3,4,5} of its octave
            static const int blackAfterWhite[5]={0,1,3,4,5};
            const int bx=(o*7+blackAfterWhite[k]+1)*stepW-blackW/2;
            lv_obj_t* b=lv_btn_create(keysBox); lv_obj_set_size(b,blackW,blackH); lv_obj_set_pos(b,bx,0);
            lv_obj_set_style_bg_color(b,KB_BLACK,0); lv_obj_set_style_bg_color(b,KB_BLACK_HI,LV_STATE_HOVERED); lv_obj_set_style_bg_grad_color(b,KB_BLACK_HI,0); lv_obj_set_style_bg_grad_dir(b,LV_GRAD_DIR_VER,0); lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0);
            lv_obj_set_style_border_color(b,COL_HAIRLINE,0); lv_obj_set_style_border_width(b,1,0); lv_obj_set_style_radius(b,scaled(lay::RADIUS_SM),0); lv_obj_set_style_pad_all(b,0,0);
            lv_obj_set_style_shadow_width(b,scaled(4),0); lv_obj_set_style_shadow_color(b,COL_BLACK,0); lv_obj_set_style_shadow_opa(b,LV_OPA_30,0);
            const int note=kbBaseNote+o*12+blackOff[k]; lv_obj_set_user_data(b,(void*)(intptr_t)note);
            lv_obj_add_event_cb(b,keyEventCb,LV_EVENT_PRESSED,this); lv_obj_add_event_cb(b,keyEventCb,LV_EVENT_RELEASED,this); lv_obj_add_event_cb(b,keyEventCb,LV_EVENT_PRESS_LOST,this); lv_obj_add_event_cb(b,keyEventCb,LV_EVENT_LEAVE,this);
            kbBlack[i]=b;
        }
        updateKeyboardNotes();
    }

    // ---- wave-4 PHYSICS strip (no modals) ----------------------------------
    // Persistent full-width card between stage and keyboard: the 7 machined
    // physics knobs + ECO toggle + material/morph dropdowns, all visible at
    // once. Budget @s=1: 24 pad + 22 head + 6 + 72 knob row = 124 exact.
    void stripKnob(lv_obj_t* row,uint32_t pi){
        ArcVisualSpec spec=normalArcSpec();
        spec.containerW=scaled(92); spec.containerH=scaled(lay::MODEL_KNOB_H); spec.arcSize=scaled(lay::MODEL_ARC);
        spec.capInset=7; spec.needleTopOffset=2; spec.needleBottomInset=3;
        lv_obj_t* arc=UIWidgets::createArcKnob(row,pi,this,styles,spec);
        lv_obj_add_event_cb(arc,valueFormatCb,LV_EVENT_ALL,this);
        widgets[pi]=arc;
        lv_obj_t* cont=lv_obj_get_parent(arc);
        lv_obj_t* lbl=cont?lv_obj_get_child(cont,lv_obj_get_child_count(cont)-1):nullptr;
        if(lbl&&lv_obj_check_type(lbl,&lv_label_class)){
            char b[24];
            formatParamValue(pi,paramCache[pi],b,sizeof(b));
            lv_label_set_text(lbl,b);
        }
    }
    static void styleModelDropdown(MultiScaleBodyUI* ui,lv_obj_t* dd){
        lv_obj_add_style(dd,&ui->styles.compactSelectMain,0);
        lv_obj_set_style_bg_color(dd,PLATE_WELL,0);
        lv_obj_set_style_border_color(dd,PLATE_EDGE,0);
        lv_obj_set_style_radius(dd,scaled(lay::RADIUS_SM),0);
        lv_obj_t* list=lv_dropdown_get_list(dd);
        if(list){ lv_obj_add_style(list,&ui->styles.compactSelectListMain,0);
                  lv_obj_set_style_max_height(list,scaled(lay::DROPDOWN_MAX_ROWS*lay::DROPDOWN_ROW_H),0); }
        lv_group_remove_obj(dd);
    }
    void buildModelStrip(lv_obj_t* root){
        using P=PluginMultiScaleBody;
        lv_obj_t* strip=makeCard(root,lv_pct(100),scaled(lay::MODEL_STRIP_H),scaled(6),LV_FLEX_ALIGN_START);
        lv_obj_t* head=makeRow(strip,lv_pct(100),scaled(lay::HEAD_H),0,LV_FLEX_ALIGN_SPACE_BETWEEN);
        addLabel(head,"PHYSICAL MODEL",getScaledSmallFont(),COL_HIGHLIGHT,2);
        addLabel(head,"paper-47 extensions",getScaledMicroFont(),PLATE_TEXT_DIM,1);
        lv_obj_t* row=makeRow(strip,lv_pct(100),scaled(lay::MODEL_KNOB_H),scaled(8),LV_FLEX_ALIGN_CENTER);
        stripKnob(row,P::kParamSupport);
        stripKnob(row,P::kParamHoldDamp);
        stripKnob(row,P::kParamResMorph);
        stripKnob(row,P::kParamMorphAmt);
        stripKnob(row,P::kParamRayleighA);
        stripKnob(row,P::kParamRayleighB);
        stripKnob(row,P::kParamEcoBudget);
        fEcoBtn=lv_btn_create(row);
        lv_obj_set_size(fEcoBtn,scaled(lay::MODEL_ECO_W),scaled(lay::BTN_H));
        styles.applyToggleButton(fEcoBtn,paramCache[P::kParamEcoMode]>0.5f);
        lv_obj_set_style_radius(fEcoBtn,scaled(lay::RADIUS_SM),0);
        lv_obj_set_style_pad_all(fEcoBtn,0,0);
        lv_obj_add_event_cb(fEcoBtn,ecoBtnCb,LV_EVENT_VALUE_CHANGED,this);
        lv_obj_t* elbl=lv_label_create(fEcoBtn); lv_label_set_text(elbl,"ECO"); lv_obj_center(elbl);
        lv_obj_t* matCol=makeCol(row,scaled(lay::MODEL_SEL_W),scaled(lay::MODEL_KNOB_H),scaled(4),LV_FLEX_ALIGN_CENTER);
        addLabel(matCol,"MATERIAL",getScaledMicroFont(),PLATE_TEXT_DIM,1);
        fMaterialDd=lv_dropdown_create(matCol);
        lv_dropdown_set_options(fMaterialDd,"DEFAULT\nALUMINIUM\nSTEEL\nBRONZE\nPINE\nROSEWOOD\nMAHOGANY\nGLASS\nBRASS\nTITANIUM\nCARBON");
        lv_dropdown_set_selected(fMaterialDd,std::clamp((int)std::lround(paramCache[P::kParamMaterial]*10.f),0,10));
        lv_obj_set_width(fMaterialDd,scaled(lay::MODEL_SEL_W));
        styleModelDropdown(this,fMaterialDd);
        lv_obj_add_event_cb(fMaterialDd,materialDdCb,LV_EVENT_VALUE_CHANGED,this);
        lv_obj_t* morphCol=makeCol(row,scaled(lay::MODEL_SEL_W),scaled(lay::MODEL_KNOB_H),scaled(4),LV_FLEX_ALIGN_CENTER);
        addLabel(morphCol,"MORPH TARGET",getScaledMicroFont(),PLATE_TEXT_DIM,1);
        fMorphDd=lv_dropdown_create(morphCol);
        { std::string opts; for(int i=0;i<modal::kNumPresets;++i){ if(i) opts+="\n"; opts+=modal::kPresets[i].name; }
          lv_dropdown_set_options(fMorphDd,opts.c_str()); }
        lv_dropdown_set_selected(fMorphDd,std::clamp((int)std::lround(paramCache[P::kParamMorphTarget]*(float)(modal::kNumPresets-1)),0,modal::kNumPresets-1));
        lv_obj_set_width(fMorphDd,scaled(lay::MODEL_SEL_W));
        styleModelDropdown(this,fMorphDd);
        lv_obj_add_event_cb(fMorphDd,morphDdCb,LV_EVENT_VALUE_CHANGED,this);
    }
    // ==== BUILD =============================================================
    // Layout hierarchy (matches Serum 2 main-view grammar, paper-faithful):
    //   ROOT  (PLATE_BG)
    //   +-- TOP-BAR  (identity: brand mark | preset browser | master knob | zoom)
    //   +-- STAGE
    //   |   +-- LEFT  (4 dial groups: BODY/RESONATE/EXCITER/SPACE)
    //   |   +-- CENTER  (hero strike disc + preset row + spec strip)
    //   |   +-- RIGHT  (spectrum card + scope card)
    //   +-- PHYSICS  (wave-4: 7 machined knobs + ECO + material/morph selects)
    //   +-- KEYBOARD  (octave + keys + ARP + tuning selects + learn chip)
    // vertical budget @s=1: 32 + 72 + 24 + 610 + 124 + 128 = 990 (exact).
    void buildUI(lv_obj_t* parent=nullptr){
        lv_obj_t* surface=parent ? parent : lv_screen_active();
        if(!surface){ lv_display_t* d=lv_display_get_default(); if(d) surface=lv_display_get_screen_active(d); }
        if(!surface) return;
        fUIBuilt=true; fMarkerPlaced=false;

        // --- SURFACE + FIXED-ASPECT PLATE -----------------------------------
        // User report (2026-09-06): "why when we zoom do we move the presets
        // and keyboard? dont do that". Root cause: the topbar/stage/keyboard
        // used to be DIRECT children of the flex SCREEN with lv_pct(100)
        // width, so at any surface not exactly 1440:990 - a big zoom step
        // clamped to the monitor working area, or a free host resize - they
        // stretched to the FULL window width and re-centered / re-flowed
        // INDEPENDENT of the stage. The presets (top bar) and keyboard
        // decoupled from the plate the moment the window got wider/taller
        // than the base aspect.
        //   Fix: the whole chassis lives in a rigid fixed-aspect PLATE of
        // scaled(BASE_W x BASE_H) centered on the surface. The chassis bg
        // letterboxes any slack and EVERYTHING inside the plate keeps its
        // exact relative position at any surface size - zoom scales the plate
        // as one unit, it never re-arranges its regions. At exact zoom steps
        // the plate fills the window (zero letterbox, pixel-identical to the
        // old layout).
        //   On rebuild, buildUI(parent=stash) re-runs widget construction
        // against a hidden full-surface overlay (see rebuildForScale); the
        // plate is still created there so the swap moves exactly one flexible
        // child back onto the real screen.
        if(!parent){
            // surface = the real screen: chassis backdrop ONLY (layout NONE so
            // the plate can center itself freely). Styling is first-build-only.
            lv_obj_set_style_bg_color(surface,PLATE_BG,0); lv_obj_set_style_bg_opa(surface,LV_OPA_COVER,0);
            lv_obj_set_layout(surface,LV_LAYOUT_NONE);
            lv_obj_set_scrollbar_mode(surface,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(surface,LV_OBJ_FLAG_SCROLLABLE);
        }
        lv_obj_t* root=lv_obj_create(surface);
        lv_obj_set_size(root,scaled(lay::BASE_W),scaled(lay::BASE_H));
        lv_obj_center(root);
        lv_obj_set_style_bg_opa(root,LV_OPA_TRANSP,0);
        lv_obj_set_style_border_width(root,0,0);
        lv_obj_set_style_pad_all(root,0,0);
        lv_obj_clear_flag(root,LV_OBJ_FLAG_SCROLLABLE); lv_obj_clear_flag(root,LV_OBJ_FLAG_CLICKABLE);
        // the plate owns the flex column (SPACE_BETWEEN + PAD) the regions need
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
        lv_obj_t* brandCol=makeCol(topbar,scaled(lay::BRAND_W),lv_pct(100),scaled(2));
        // Round-6: the title is MEASURED and steps down font buckets until it
        // fits the column - "MULTI-SCALE BODY" at 26px + space 4 needed ~370px
        // and clipped to "MULTI-SCALE B" in a 240px column (fitKnobText
        // pattern from the gauntlet kit: never ship a truncated headline).
        lv_obj_t* titleLbl=nullptr;
        {
            struct BrandFont{ const lv_font_t* f; int ls; };
            const BrandFont cand[]={
                {getDisplayFont26(),2},{getDisplayFont(),2},
                {getScaledFont(),2},{getScaledSmallFont(),2}};
            const char* txt="MULTI-SCALE BODY";
            const lv_coord_t avail=scaled(lay::BRAND_W);
            // measure by creating the label and forcing its layout: SIZE_CONTENT
            // resolves to the real text width, so lv_obj_get_width is the truth
            for(const BrandFont& c:cand){
                lv_obj_t* l=addLabel(brandCol,txt,c.f,PLATE_TITLE,c.ls);
                lv_obj_update_layout(l);
                if(lv_obj_get_width(l)<=avail){ titleLbl=l; break; }
                lv_obj_delete(l);
            }
            if(!titleLbl) titleLbl=addLabel(brandCol,txt,cand[3].f,PLATE_TITLE,cand[3].ls);
        }
        // Round-6: sub-line shortened to the synth class - the DAFX-09/PAPER 47
        // identity already lives right-aligned in the nav strip below, and the
        // duplicated suffix truncated at the old 240px column.
        lv_obj_t* authorsLbl=addLabel(brandCol,"MODAL SYNTH",getScaledMicroFont(),PLATE_TEXT_DIM,1);
        lv_obj_set_style_text_opa(authorsLbl,LV_OPA_50,0);
        lv_obj_set_style_text_letter_space(authorsLbl,1,0);
        // R5: vertical divider between the brand mark and the preset
        // browser - the R4 critic read the top bar as label soup (title,
        // "BAKED MODAL PRESETS" caption, dropdown, OUTPUT all jammed
        // together). Dividers group the bar into three legible zones:
        // BRAND | PRESET | MASTER+ZOOM. The "BAKED MODAL PRESETS" suffix
        // was redundant with the brand sub-line "MODAL SYNTH" and was
        // dropped; the caption now reads simply "PRESET".
        addDivider(topbar,scaled(lay::HEADER_H-20));
        // preset browser - move from the center to the top bar so the hero gets air
        lv_obj_t* presetBar=makeCol(topbar,0,lv_pct(100),scaled(3));
        lv_obj_set_flex_grow(presetBar,1);
        lv_obj_set_style_pad_hor(presetBar,scaled(10),0);
        lv_obj_set_flex_align(presetBar,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START);
        addLabel(presetBar,"PRESET",getScaledMicroFont(),PLATE_TEXT_DIM,2);
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
        // FIX: the VALUE_CHANGED + CLICKED handlers were defined
        // (dropdownCb/presetArrowCb) but never registered, so the whole
        // preset browser was dead. Wire them here.
        lv_obj_add_event_cb(presetDropdown,dropdownCb,LV_EVENT_VALUE_CHANGED,this);
        lv_obj_add_event_cb(presetPrevBtn,presetArrowCb,LV_EVENT_CLICKED,this);
        lv_obj_add_event_cb(presetNextBtn,presetArrowCb,LV_EVENT_CLICKED,this);
        // R5: vertical divider between the preset browser and the master
        // cluster - second of three separators in the top bar so each zone
        // (brand | preset | master+zoom) reads as a distinct module.
        addDivider(topbar,scaled(lay::HEADER_H-20));
        // master knob - synthesized for piece-1 (paper's modal energy level).
        // Topbar inner height is only 50px (72 - 2*11 pad), so the full
        // dial-bank createArcKnob (116px container) does NOT fit: its flex
        // centering spilled 41px past both edges (title at y=-14, value chip
        // bleeding into the nav strip - the d3 BOUNDS-FAIL in the harness).
        // Compact layout: captioned row (OUTPUT label + arc + value chip)
        // with the arc at 44px so the whole cluster centers in 50px.
        lv_obj_t* masterCol=makeCol(topbar,scaled(150),lv_pct(100),scaled(2),LV_FLEX_ALIGN_CENTER);
        lv_obj_set_flex_align(masterCol,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
        addLabel(masterCol,"VOLUME",getScaledMicroFont(),PLATE_LABEL_ACCENT,2);
        lv_obj_t* masterRow=makeRow(masterCol,lv_pct(100),scaled(30),0,LV_FLEX_ALIGN_CENTER);
        {
            ArcVisualSpec mSpec=normalArcSpec();
            mSpec.containerW=scaled(44); mSpec.containerH=scaled(30);
            mSpec.arcSize=scaled(28);
            mSpec.capInset=5; mSpec.needleTopOffset=1; mSpec.needleBottomInset=2;
            mSpec.labelMarginBottom=0; mSpec.valueMarginTop=0;
            lv_obj_t* masterArc=UIWidgets::createArcKnob(masterRow,PluginMultiScaleBody::kParamVolume,this,styles,mSpec);
            regExtraWidget(PluginMultiScaleBody::kParamVolume, masterArc);
            lv_obj_add_event_cb(masterArc,valueFormatCb,LV_EVENT_ALL,this);
            // FIX: the widget stacks title + arc(28) + chip(~13) = ~41px in
            // a 30px container, so the chip spilled out of the 30px row and
            // clipped (the "cut off" text). Hide BOTH built-in labels - the
            // VOLUME caption above already names the cluster - and own a
            // value label beside the arc. Also hide this instance's white
            // PART_KNOB tip tick: at 28px it collides with the white needle
            // into "2 white lines"; the blue indicator arc still shows value.
            lv_obj_set_style_bg_opa(masterArc,LV_OPA_TRANSP,LV_PART_KNOB);
            lv_obj_t* cont=lv_obj_get_parent(masterArc);
            if(cont && lv_obj_get_child_count(cont)>0){
                for(uint32_t ci=0;ci<(uint32_t)lv_obj_get_child_count(cont);++ci){
                    lv_obj_t* ch=lv_obj_get_child(cont,(int32_t)ci);
                    if(ch && ch!=masterArc && lv_obj_check_type(ch,&lv_label_class))
                        lv_obj_add_flag(ch,LV_OBJ_FLAG_HIDDEN);
                }
            }
            fMasterValLbl=addLabel(masterRow,"",getScaledSmallFont(),PLATE_TEXT,0);
            {
                char b[24];
                formatParamValue(PluginMultiScaleBody::kParamVolume,paramCache[PluginMultiScaleBody::kParamVolume],b,sizeof(b));
                lv_label_set_text(fMasterValLbl,b);
            }
        }
        // zoom stepper - the rightmost cluster, vertical divider before it
        addDivider(topbar,scaled(lay::HEADER_H-20));
        const int clusterW=lay::ZOOM_BTN*2+lay::ZOOM_LBL_W+12;
        lv_obj_t* zoomRow=makeRow(topbar,scaled(clusterW),scaled(lay::ZOOM_LBL_H),scaled(4),LV_FLEX_ALIGN_CENTER);
        zoomMinus=addButton(zoomRow,lay::ZOOM_BTN,lay::ZOOM_LBL_H,"-",PLATE_TEXT_MID);
        lv_obj_set_user_data(zoomMinus,(void*)(intptr_t)-1);
        lv_obj_add_event_cb(zoomMinus,zoomBtnCb,LV_EVENT_CLICKED,this);
        { char zb[16]; snprintf(zb,sizeof(zb),"%d%%",lay::ZOOM_STEPS[std::clamp(fZoomIdx,0,lay::ZOOM_STEP_COUNT-1)]); zoomValLbl=addLabel(zoomRow,zb,getScaledSmallFont(),PLATE_AMBER,1); }
        lv_obj_set_width(zoomValLbl,scaled(lay::ZOOM_LBL_W));
        lv_obj_set_style_text_align(zoomValLbl,LV_TEXT_ALIGN_CENTER,0);
        zoomPlus=addButton(zoomRow,lay::ZOOM_BTN,lay::ZOOM_LBL_H,"+",PLATE_TEXT_MID);
        lv_obj_set_user_data(zoomPlus,(void*)(intptr_t)1);
        lv_obj_add_event_cb(zoomPlus,zoomBtnCb,LV_EVENT_CLICKED,this);
        refreshZoomWidgets();   // rebuild wipes the label/disabled states - re-assert from fZoomIdx
        // --- STAGE ROW (h = 610): dial bank | hero plate | analysis tower -----
        lv_obj_t* stage=makeRow(root,lv_pct(100),scaled(lay::STAGE_H),scaled(lay::GUTTER));
        // LEFT - FORGE: four labeled knob clusters, spread over the full column
        // heights: BODY 16+6+116=138, others 16+6+98=120; SPACE_BETWEEN spreads
        // the leftover 112 across three inter-cluster gaps (~37) - deliberate air
        lv_obj_t* left=makeCol(stage,scaled(lay::LEFT_W),scaled(lay::STAGE_H),0,LV_FLEX_ALIGN_SPACE_BETWEEN);
        const uint32_t groupParams[5][4]={
            {PluginMultiScaleBody::kParamPitch,PluginMultiScaleBody::kParamDecay,PluginMultiScaleBody::kParamBrightness,PluginMultiScaleBody::kParamModeCount},
            {PluginMultiScaleBody::kParamWidth,PluginMultiScaleBody::kParamRadiation,PluginMultiScaleBody::kParamDetune,PluginMultiScaleBody::kParamGlide},
            {PluginMultiScaleBody::kParamVelStrike,PluginMultiScaleBody::kParamExciteMix,PluginMultiScaleBody::kParamAttack,PluginMultiScaleBody::kParamRelease},
            {PluginMultiScaleBody::kParamLFORate,PluginMultiScaleBody::kParamLFODepth,PluginMultiScaleBody::kParamWet,PluginMultiScaleBody::kParamMono},
            {PluginMultiScaleBody::kParamBow,PluginMultiScaleBody::kParamDamper,PluginMultiScaleBody::kParamInharm,PluginMultiScaleBody::kParamSlideMode}
        };
        // FEEL = the wave-2 excitation/modulation row: bow, felt damper,
        // inharmonicity, MPE slide routing. Compact knobs (arc 48) so the
        // 5th row fits the 610px column: 138+120+120+120+106 = 604 <= 610.
        const char* groupNames[5]={"BODY","RESONATE","EXCITER","SPACE","FEEL"};
        for(int g=0;g<5;++g){
            const bool primary=(g==0);
            const bool feel=(g==4);
            const int kh=primary?lay::KNOB_H_N:(feel?lay::KNOB_H_FEEL:lay::KNOB_H_C);
            lv_obj_t* sec=makeCol(left,scaled(lay::LEFT_W),scaled(lay::SEC_LABEL_H+lay::SEC_GAP+kh),scaled(lay::SEC_GAP));
            lv_obj_set_flex_align(sec,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START);
            // R4: per-section color identity (Pigments-style categorical
            // vocabulary) - a 3px x SEC_LABEL_H vertical rule beside the
            // label + the label itself tinted in the section color. The R2
            // fix that defined SEC_* was lost in a revert cycle (macros
            // existed but were never applied) - the R3 critic read the left
            // bank as "single cyan accent across every section".
            static const lv_color_t secColors[5]={SEC_BODY,SEC_RESONATE,SEC_EXCITER,SEC_SPACE,SEC_EXCITER};
            lv_obj_t* secLabelRow=makeRow(sec,lv_pct(100),scaled(lay::SEC_LABEL_H),scaled(6),LV_FLEX_ALIGN_START);
            lv_obj_t* secRule=makeBox(secLabelRow,scaled(3),scaled(lay::SEC_LABEL_H));
            lv_obj_set_style_bg_color(secRule,secColors[g],0);
            lv_obj_set_style_bg_opa(secRule,LV_OPA_COVER,0);
            lv_obj_clear_flag(secRule,LV_OBJ_FLAG_CLICKABLE);
            // group caption: BODY keeps full lead-blue (active lead group),
            // others carry their own section tint at 80% so the categorical
            // identity is legible but the lead still leads.
            lv_obj_t* glbl=addLabel(secLabelRow,groupNames[g],getScaledMicroFont(),primary?PLATE_LABEL_ACCENT:secColors[g],5);
            if(!primary) lv_obj_set_style_text_opa(glbl,LV_OPA_80,0);
            // knob grid: explicit single-row width so nothing ever wraps
            lv_obj_t* grid=makeRow(sec,scaled(lay::LEFT_W),scaled(kh),scaled(lay::GRID_GUT_X));
            ArcVisualSpec groupSpec=normalArcSpec();
            // R5: uniform knob pitch across all 4 groups - the R4 critic
            // read the left bank as "inconsistent widths, gaps, and
            // densities (exciter knobs bunched, modal knobs sparse)".
            // Secondary groups now share BODY's container width (92) so
            // every row is 4*92+3*8 = 392px exact; arc size stays compact
            // (64) so the visual size hierarchy still reads (BODY knobs
            // are visibly larger). The 4-pixel added per container is
            // padding inside, not a wider arc - no UIWidgets clipping.
            if(primary){ /* default: 92/116/76 */ }
            else if(feel){ groupSpec.containerW=scaled(lay::KNOB_W_N); groupSpec.containerH=scaled(lay::KNOB_H_FEEL); groupSpec.arcSize=scaled(lay::KNOB_ARC_FEEL); groupSpec.capInset=9; groupSpec.needleTopOffset=2; groupSpec.needleBottomInset=3; }
            else { groupSpec.containerW=scaled(lay::KNOB_W_N); groupSpec.containerH=scaled(lay::KNOB_H_C); groupSpec.arcSize=scaled(lay::KNOB_ARC_C); }
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
        // wave-3 (idea 10): disc motion recorder — REC captures the drag path,
        // PLAY replays it as a strike sequence, X clears.
        fRecBtn=addButton(cHead,40,lay::BTN_H,"REC",PLATE_TEXT_MID);
        styles.applyToggleButton(fRecBtn,false);
        lv_obj_add_event_cb(fRecBtn,recBtnCb,LV_EVENT_CLICKED,this);
        fPlayBtn=addButton(cHead,40,lay::BTN_H,"PLAY",PLATE_TEXT_MID);
        styles.applyToggleButton(fPlayBtn,false);
        lv_obj_add_event_cb(fPlayBtn,playBtnCb,LV_EVENT_CLICKED,this);
        lv_obj_t* recClearBtn=addButton(cHead,28,lay::BTN_H,"X",PLATE_TEXT_MID);
        lv_obj_add_event_cb(recClearBtn,recClearCb,LV_EVENT_CLICKED,this);
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
        // ROUND-9: richer outer glow - the disc reads as a lit physical panel
        // (matches the Pigments wavetable's halo). Wider shadow with accent
        // color bleed and spread makes the disc the hero element.
        lv_obj_set_style_shadow_width(strikeDisc,scaled(24),0);
        lv_obj_set_style_shadow_color(strikeDisc,COL_HIGHLIGHT,0);
        lv_obj_set_style_shadow_opa(strikeDisc,LV_OPA_30,0);
        lv_obj_set_style_shadow_spread(strikeDisc,scaled(2),0);
        lv_obj_set_style_shadow_offset_y(strikeDisc,scaled(lay::SHADOW_OFF_Y),0);
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
        lv_obj_set_pos(strikeDot,(int)(paramCache[PluginMultiScaleBody::kParamStrikeX]*(D-scaled(12))),
                                (int)((1.f-paramCache[PluginMultiScaleBody::kParamStrikeY])*(D-scaled(12))));
        lv_obj_clear_flag(strikeDot,LV_OBJ_FLAG_CLICKABLE);

        // === ROUND-6: MODE MAP (per-mode strike-gain comb) ================
        // ROUND-9: DISC HEATMAP - the disc interior projects the current
        // preset's strike-gain sound map (same bilinear 15x15 grid the MODE
        // MAP reads from) as a 10x10 grid of dim accent dots. This kills the
        // "vast empty dark void" reading (R2 critic) by giving the disc real
        // rendered content that reads as a topographic modal surface.
        // wave-3 (idea 5): painted by paintDiscHeatmap() so the node-focus
        // overlay and gain-affecting params can repaint without a rebuild.
        paintDiscHeatmap();
        // Round-5's MODE ACTIVITY panel re-plotted the SAME 16 env[] bands as
        // the MODE SPECTRUM - the user read it as a duplicated spectrum. It
        // is now a 128-slot per-MODE comb instead: bar height = that mode's
        // strike-position gain (bilinear sound-map, the same ModalData math
        // the idle spectrum preview uses), mode order = ascending baked
        // frequency. A different QUESTION than the live spectrum ("what does
        // this body do under the mallet" vs "what is it sounding now"), and
        // it visibly morphs as the strike disc / preset / Modes knob move.
        // Budget @s=1 (discCol = 610, 6px flex gaps): 22 head + 6 + 280 disc
        //   + 6 + 22 head + 6 + 268 card = 610 EXACT - zero dead band.
        lv_obj_t* actHead=makeRow(discCol,scaled(lay::DISC_D),scaled(lay::HEAD_H),0,LV_FLEX_ALIGN_SPACE_BETWEEN);
        addLabel(actHead,"MODE MAP",getScaledSmallFont(),PLATE_TEXT,2);
        addLabel(actHead,"STRIKE GAINS",getScaledMicroFont(),PLATE_TEXT_DIM,1);
        lv_obj_t* actCard=makeCard(discCol,scaled(lay::DISC_D),scaled(lay::MAP_CARD_H),scaled(4));
        lv_obj_t* actRow=makeRow(actCard,lv_pct(100),scaled(lay::MAP_BARS_H),1,LV_FLEX_ALIGN_START);
        lv_obj_set_flex_align(actRow,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_END,LV_FLEX_ALIGN_CENTER);
        for(int m=0;m<modal::kMaxModes;++m){
            lv_obj_t* bar=lv_obj_create(actRow);
            lv_obj_set_width(bar,scaled(lay::MAP_BAR_W));
            lv_obj_set_height(bar,1);
            lv_obj_set_style_radius(bar,0,0);
            // non-peak bars use the dim amber (round-7: PLATE_EDGE read as
            // invisible at 1px - the critic saw a near-empty rectangle. The
            // dim amber still obeys the one-accent rule (only PEAK is full
            // round-8: the comb must read as real data at a glance. Solid
            // amber at high opacity for non-peak (the spectrum already uses
            // amber bars so this is consistent), full highlight for peak.
            lv_obj_set_style_bg_color(bar,PLATE_AMBER,0);
            lv_obj_set_style_bg_opa(bar,LV_OPA_70,0);
            lv_obj_set_style_border_width(bar,0,0);
            lv_obj_set_style_pad_all(bar,0,0);
            lv_obj_clear_flag(bar,LV_OBJ_FLAG_SCROLLABLE);
            // wave-3 (idea 5): bars are clickable targets for node-focus
            // selection (safe: the card has no other handlers underneath).
            lv_obj_add_flag(bar,LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(bar,(void*)(intptr_t)m);
            lv_obj_add_event_cb(bar,modeBarCb,LV_EVENT_CLICKED,this);
            fModeBars[m]=bar;
        }
        // shared peak cell: PEAK + "M<n> . <freq> HZ" readout
        lv_obj_t* actPeakCell=makeRow(actCard,lv_pct(100),scaled(lay::MAP_PEAK_H),0,LV_FLEX_ALIGN_SPACE_BETWEEN);
        addLabel(actPeakCell,"PEAK",getScaledMicroFont(),PLATE_TEXT_DIM,1);
        fModePeakLbl=addLabel(actPeakCell,"M1",getScaledMicroFont(),PLATE_AMBER,1);

        // === INFO COL (right of the disc) ================================
        // The body-info cluster spread to the right of the disc instead of
        // below - this is the r1->r2 anchor shift that puts the disc in
        // dialog with the body it represents, not stacked over a cramped 3-label
        // caption row. Also hosts the coord readout.
        // section header
        lv_obj_t* infoHead=makeRow(infoCol,lv_pct(100),scaled(lay::HEAD_H),0,LV_FLEX_ALIGN_START);
        addLabel(infoHead,"BODY",getScaledSmallFont(),PLATE_LABEL_ACCENT,2);
        addLabel(infoHead,"PRESET / MATERIAL / MODES",getScaledMicroFont(),PLATE_TEXT_DIM,2);
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
        addLabel(infoTextCol,"MATERIAL PREVIEW",getScaledMicroFont(),PLATE_TEXT_DIM,1);
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
        // R5: DAMPING card - the 16-band tail-time map that fills the
        // infoCol's dead band (R4 critic: "plate floats in dead space").
        // Each row is one frequency band (B1..B16 = ascending mode index
        // ranges); bar length tracks 1/<band-mean decay> normalized to the
        // longest band in the current preset. The decay RATE is inverted
        // (rate in -> time out) so longer-bar = longer-tail, matching the
        // user's knob intuition. The DECAY knob rescales the display in
        // lockstep with the engine (decayScale_ mirror) so the user sees
        // the knob's effect immediately. Card height 400 leaves 166px of
        // padding absorbed by the infoCol's 6px flex gaps and the spec
        // strip above (no dead band remains).
        // CARD height is the ONLY fixed slot; the spec strip above is
        // content-sized (height 0) and the head + coord wrap + dividers
        // contribute their fixed sizes - the makeCol gap of 6 carries the
        // remaining vertical slack. To keep the card at the bottom of the
        // column without floating, we push the preceding content up via
        // a flex-grow spacer (infoColSpacer) above the card.
        lv_obj_t* infoColSpacer=lv_obj_create(infoCol);
        lv_obj_set_size(infoColSpacer,1,1);
        lv_obj_set_style_bg_opa(infoColSpacer,LV_OPA_TRANSP,0);
        lv_obj_set_style_border_width(infoColSpacer,0,0);
        lv_obj_set_style_pad_all(infoColSpacer,0,0);
        lv_obj_set_flex_grow(infoColSpacer,1);
        lv_obj_clear_flag(infoColSpacer,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* dampCard=makeCard(infoCol,lv_pct(100),scaled(lay::DAMP_CARD_H),scaled(2));
        // dampCard head: matches the discCol MODE MAP header grammar
        // ("MODE MAP" / "STRIKE GAINS") for cross-column reading rhythm.
        lv_obj_t* dampHead=makeRow(dampCard,lv_pct(100),scaled(lay::HEAD_H),0,LV_FLEX_ALIGN_SPACE_BETWEEN);
        addLabel(dampHead,"DAMPING",getScaledSmallFont(),COL_HIGHLIGHT,2);
        addLabel(dampHead,"BY BAND",getScaledMicroFont(),PLATE_TEXT_DIM,1);
        // 16 band rows. Each row: [Bxx label][bar (lv_bar, flex_grow)][t60 value].
        // Bars are lv_bar widgets styled like the fLevelBar meter so the
        // chassis reads as one instrument (meterTrack style + PLATE_AMBER
        // indicator at LV_OPA_70 so the panels don't compete for amber).
        for(int b=0;b<16;++b){
            lv_obj_t* row=makeRow(dampCard,lv_pct(100),scaled(lay::DAMP_ROW_H),scaled(4),LV_FLEX_ALIGN_CENTER);
            char lab[8]; snprintf(lab,sizeof(lab),"B%d",b+1);
            addLabel(row,lab,getScaledMicroFont(),PLATE_TEXT_DIM,1);
            // 1px narrower than the label spec (22) so the B-cell and
            // value cell actually fit a 194-wide card: 22 + 4 + flex + 4 + 40
            // = 70; remaining 124 for the bar - generous. The bar's
            // background is a dark track, the fill is amber.
            lv_obj_t* bar=lv_bar_create(row);
            lv_obj_set_height(bar,scaled(8));
            lv_obj_set_flex_grow(bar,1);
            lv_obj_set_style_radius(bar,scaled(2),0);
            lv_obj_set_style_radius(bar,scaled(2),LV_PART_INDICATOR);
            lv_obj_add_style(bar,&styles.meterTrack,LV_PART_MAIN);
            lv_obj_set_style_bg_color(bar,PLATE_AMBER,LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(bar,LV_OPA_70,LV_PART_INDICATOR);
            lv_obj_set_style_border_width(bar,0,0);
            lv_obj_clear_flag(bar,LV_OBJ_FLAG_CLICKABLE);
            lv_bar_set_range(bar,0,1000);
            lv_bar_set_value(bar,0,LV_ANIM_OFF);
            fDampBars[b]=bar;
            fDampVals[b]=addLabel(row,"-",getScaledMicroFont(),PLATE_TEXT,0);
        }
        // initial fill: bar widths depend on baked decay, not on params -
        // call once on first build, and again on preset+Decay changes.
        updateDampingDisplay();

        // === ROUND-5 (issue #1): hero-column density ======================
        // Judged from fresh capture r4_mpe_check.png: the 280px disc interior
        // was ~96-98% empty charcoal and ~260px below the disc was dead space;
        // the center column read as a big dark hole between the knob bank and
        // the analyzer tower. Fix: keep the disc the hero but make the space
        // AROUND it work - the disc column gains a live mode-activity panel
        // under the disc (mode energy strip driven by the same fVizBins data
        // the spectrum chart uses, so the hero column shows WHAT the body is
        // doing, not just where to hit it), and the info column spreads its
        // existing content with larger gaps instead of clustering at 25%.


        // RIGHT - ANALYSIS TOWER: spectrum card 360 + gutter 6 + scope card 244 = 610 exact
        lv_obj_t* right=makeCol(stage,0,scaled(lay::STAGE_H),scaled(lay::GUTTER)); // explicit-height parent: grow legal
        lv_obj_set_flex_grow(right,1);   // absorb remaining width (no horizontal overflow)

        // spectrum card: 24 pad + 22 head + 8 + 294 chart + 8 + 14 band ticks = 370
        // (the B1..B16 strip makes the chart read as an analyzer, not a bar chart)
        lv_obj_t* spectrumCard=makeCard(right,lv_pct(100),scaled(lay::SPECTRUM_CARD_H),scaled(8));
        lv_obj_t* specHead=makeRow(spectrumCard,lv_pct(100),scaled(lay::HEAD_H),0,LV_FLEX_ALIGN_SPACE_BETWEEN);
        addLabel(specHead,"MODE SPECTRUM",getScaledSmallFont(),COL_HIGHLIGHT,2);
        lv_obj_t* specBtns=makeRow(specHead,scaled(2*96+6),scaled(lay::BTN_H),scaled(6));
        // idea 2: scrub target toggle — drag writes per-band GAIN (default)
        // or per-band DECAY trim (drag levels the tail time).
        fScrubToggle=addButton(specBtns,lay::RND_W,lay::BTN_H,"GAIN",PLATE_TEXT_MID);
        lv_obj_add_event_cb(fScrubToggle,scrubToggleCb,LV_EVENT_CLICKED,this);
        lv_obj_t* rndBtn=addButton(specBtns,lay::RND_W,lay::BTN_H,"RANDOMIZE",COL_HIGHLIGHT);
        lv_obj_add_event_cb(rndBtn,rndBtnCb,LV_EVENT_CLICKED,this);
        lv_obj_t* chart=lv_chart_create(spectrumCard);
        lv_obj_set_size(chart,lv_pct(100),scaled(lay::CHART_H));
        lv_chart_set_type(chart,LV_CHART_TYPE_BAR); lv_chart_set_point_count(chart,16); lv_chart_set_range(chart,LV_CHART_AXIS_PRIMARY_Y,0,1000);
        lv_obj_set_style_bg_color(chart,PLATE_WELL,0); lv_obj_set_style_bg_opa(chart,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(chart,PLATE_LINE,0); lv_obj_set_style_border_width(chart,1,0);
        lv_obj_set_style_radius(chart,scaled(lay::RADIUS),0);
        lv_chart_set_div_line_count(chart,4,16);
        // ROUND-9: dim the bar series (LV_PART_ITEMS) so the spectrum stops
        // out-shouting the disc + wordmark (R2 critic).
        lv_obj_set_style_line_opa(chart,LV_OPA_70,LV_PART_ITEMS);
        lv_obj_set_style_line_width(chart,scaled(1),LV_PART_ITEMS);
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
        // round-7: thicker series line + a dim-amber AREA fill underneath
        // (LV_PART_ITEMS) so the scope reads as a real waveform, not a hair.
        lv_obj_set_style_line_color(scope,PLATE_LINE,LV_PART_MAIN);
        lv_obj_set_style_line_width(scope,1,LV_PART_MAIN);
        lv_obj_set_style_line_opa(scope,LV_OPA_30,LV_PART_MAIN);
        lv_obj_set_style_line_width(scope,scaled(2),LV_PART_ITEMS);
        lv_obj_set_style_line_color(scope,COL_HIGHLIGHT,LV_PART_ITEMS);
        lv_obj_set_style_line_opa(scope,LV_OPA_COVER,LV_PART_ITEMS);
        // round-7: simpler fix - just make the line thicker + add a dim area
        // fill via a second series that mirrors the line. Both seeded.
        // (area series is added first so it paints behind the line)
        lv_chart_series_t* ssArea=lv_chart_add_series(scope,PLATE_AMBER_DIM,LV_CHART_AXIS_PRIMARY_Y);
        lv_obj_set_style_line_color(scope,PLATE_AMBER_DIM,LV_PART_ITEMS);
        lv_obj_set_style_line_opa(scope,LV_OPA_50,LV_PART_ITEMS);
        lv_obj_set_style_line_width(scope,scaled(1),LV_PART_ITEMS);
        // widen the line a hair
        // round-7: main (bright) line series — declared first so it paints over
        // the dim-amber area series
        lv_chart_series_t* ss=lv_chart_add_series(scope,COL_HIGHLIGHT,LV_CHART_AXIS_PRIMARY_Y);
        lv_obj_set_style_line_color(scope,COL_HIGHLIGHT,LV_PART_ITEMS);
        lv_obj_set_style_line_opa(scope,LV_OPA_COVER,LV_PART_ITEMS);
        lv_obj_set_style_line_width(scope,scaled(2),LV_PART_ITEMS);
        lv_obj_set_style_size(scope,0,0,LV_PART_INDICATOR);
        // round-8: seed the area series so the dim-amber fill actually renders
        for(int i=0;i<128;++i) lv_chart_set_next_value(scope,ssArea,0);
        for(int i=0;i<128;++i) lv_chart_set_next_value(scope,ss,0);
        fScopeChart=scope; fScopeSeries=(void*)ss; fScopeAreaSeries=(void*)ssArea;

        // wave-4 PHYSICS strip (persistent, full-width, between stage + keyboard)
        buildModelStrip(root);
        // keyboard strip (full-width row under the physics strip)
        createKeyboard(root);

        // sync readouts now that all labels exist
        updateBodyInfo();
        updateBodyPreview();

        // settle the layout synchronously: the passive per-frame pass can sit at
        // a stale fixed point after a rescale rebuild; one explicit pass from the
        // root converges the whole tree - THEN position anything that measured
        lv_obj_update_layout(root);
        layoutMeterMarks();
        // prime both right-column charts with real data NOW: the 33ms spectrum
        // timer is only serviced by lv_timer_handler() inside the DGL idle
        // callback, which several hosts (and the capture harness) never run
        // before their first present. One synchronous pass seeds the analyzer
        // bars and the full idle decay trace, so the panels never present blank.
        updateSpectrumDisplay();
    }

    // R3: build the idle decay-envelope preview trace. Uses ONLY baked
    // ModalData (per-mode decay rates + gains at the current strike point),
    // so it works with no audio thread alive. Mirrors the engine exactly:
    // shapeDecayRate() geometric pull toward the preset anchor, then the
    // mirrored Decay-knob scale; envelope = exp(-t*rate). The full 128-point
    // curve is seeded into the chart so the scope shows the complete
    // strike-then-ring envelope immediately, not after 4 s of ticks.
    void buildScopePreview(){
        using namespace modal;
        fScopePreviewReady=false;
        int mx = kNumPresets - 1;
        int preset = (int)std::round(paramCache[PluginMultiScaleBody::kParamPreset]*(float)mx);
        preset = std::clamp(preset,0,mx);
        const auto& pr = kPresets[preset];
        int n = std::clamp((int)(8 + paramCache[PluginMultiScaleBody::kParamModeCount]*120.f), 8, pr.n);
        // strike point -> bilinear mode gains (same math as the spectrum preview)
        float sx = paramCache[PluginMultiScaleBody::kParamStrikeX];
        float sy = paramCache[PluginMultiScaleBody::kParamStrikeY];
        float fx=sx*15.f, fy=sy*15.f; int x0=(int)fx, y0=(int)fy;
        x0=std::clamp(x0,0,14); y0=std::clamp(y0,0,14);
        int x1=x0+1, y1=y0+1; float dx=fx-x0, dy=fy-y0;
        float w00=(1-dx)*(1-dy), w10=dx*(1-dy), w01=(1-dx)*dy, w11=dx*dy;
        // engine decay semantics: pull each rate toward the preset anchor
        // (shapeDecayRate, beta=0.45, anchor=dmin/6), then the mirrored
        // Decay-knob scale; tau = 1/rate
        float dmn=pr.decay[0];
        for(int i=1;i<pr.n;++i) if(pr.decay[i]<dmn) dmn=pr.decay[i];
        const float ref=dmn/6.f;
        float dk = paramCache[PluginMultiScaleBody::kParamDecay];
        float dScale = 0.1f*std::pow(100.f,1.f-dk);
        // wave-3: morph target + live rate factors (engine mirrors)
        float mAmt=paramCache[PluginMultiScaleBody::kParamMorphAmt];
        float rm=paramCache[PluginMultiScaleBody::kParamResMorph];
        const int mtx=std::clamp((int)std::lround(paramCache[PluginMultiScaleBody::kParamMorphTarget]*(float)mx),0,mx);
        const auto& pt = kPresets[mtx];
        const int tN=std::max(1,pt.n);
        const float supMul = 1.f + 0.5f*paramCache[PluginMultiScaleBody::kParamSupport];
        float edge=std::max(std::fabs(sx-0.5f),std::fabs(sy-0.5f))*2.f;
        const float holdMul = 1.f + 2.f*paramCache[PluginMultiScaleBody::kParamHoldDamp]*std::max(0.f,1.f-edge);
        const float rA=paramCache[PluginMultiScaleBody::kParamRayleighA];
        const float rB=paramCache[PluginMultiScaleBody::kParamRayleighB];
        const float rayA2 = 5.f*rA*rA, rayB2 = 1.2436e-4f*rB*rB;
        // 4.3 s window across 128 points; 3 ms attack ramp first
        const float dt = 4.3f/128.f;
        float peak=1e-12f;
        for(int i=0;i<128;++i){
            float t = i*dt;
            float attack = std::min(t/0.003f, 1.f);
            float e=0.f;
            for(int m=0;m<n;++m){
                float g = pr.gain[m][y0][x0]*w00 + pr.gain[m][y0][x1]*w10
                        + pr.gain[m][y1][x0]*w01 + pr.gain[m][y1][x1]*w11;
                float dm = pr.decay[m], fm = pr.freq[m];
                if(rm>1e-4f){ dm = dm + (pr.fineDecay[m]-dm)*rm; fm = fm + (pr.fineFreq[m]-fm)*rm; }
                if(mAmt>1e-4f){ const int ti=std::min(m,tN-1); dm = dm + (pt.decay[ti]-dm)*mAmt; fm = fm + (pt.freq[ti]-fm)*mAmt;
                    float tg = pt.gain[ti][y0][x0]*w00 + pt.gain[ti][y0][x1]*w10
                             + pt.gain[ti][y1][x0]*w01 + pt.gain[ti][y1][x1]*w11;
                    g = g + (tg-g)*mAmt; }
                float rate = std::pow(std::max(0.2f,dm),0.55f)*std::pow(ref,0.45f)*dScale*supMul*holdMul
                             + rayA2 + rayB2*fm*fm;
                if(rate<0.2f) rate=0.2f; if(rate>8000.f) rate=8000.f;
            }
            e *= attack;
            fScopePreview[i]=e;
            peak=std::max(peak,e);
        }
        for(int i=0;i<128;++i) fScopePreview[i]/=peak;
        if(fScopeChart && fScopeSeries){
            for(int i=0;i<128;++i){
                const int32_t v=(int32_t)(fScopePreview[i]*980.f);
                lv_chart_set_next_value(fScopeChart,(lv_chart_series_t*)fScopeSeries,v);
                if(fScopeAreaSeries)
                    lv_chart_set_next_value(fScopeChart,(lv_chart_series_t*)fScopeAreaSeries,v);
            }
            lv_chart_refresh(fScopeChart);
        }
    }
    void updateSpectrumDisplay(){
        if(!fSpectrumChart) return;
        // place the strike marker once the disc has real geometry
        if(!fMarkerPlaced && strikeDisc && lv_obj_get_width(strikeDisc)>0){ fMarkerPlaced=true; updateStrikeMarker(); }
        lv_chart_series_t* s=lv_chart_get_series_next(fSpectrumChart,nullptr); if(!s) return;
        float bins[16]={};
        float totalE=0.f;
        // live iff metering arrived within the last ~1.5s (45 ticks);
        // otherwise fall back to the baked preview so panels keep moving.
        if(fLiveAge<1000) ++fLiveAge;
        const bool live = fGotLiveViz && fLiveAge<45;
        if(live){
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
            // wave-3 (idea 4): blend the morph target's sound map
            float mAmtS=paramCache[PluginMultiScaleBody::kParamMorphAmt];
            const auto& ptS=(mAmtS>1e-4f)?kPresets[std::clamp((int)std::lround(paramCache[PluginMultiScaleBody::kParamMorphTarget]*(float)mx),0,mx)]:pr;
            const int tNS=std::max(1,ptS.n);
            for(int m=0;m<n;++m){
                float g = pr.gain[m][y0][x0]*w00 + pr.gain[m][y0][x1]*w10 + pr.gain[m][y1][x0]*w01 + pr.gain[m][y1][x1]*w11;
                if(mAmtS>1e-4f){ const int ti=std::min(m,tNS-1);
                    float tg = ptS.gain[ti][y0][x0]*w00 + ptS.gain[ti][y0][x1]*w10 + ptS.gain[ti][y1][x0]*w01 + ptS.gain[ti][y1][x1]*w11;
                    g = g + (tg-g)*mAmtS; }
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
        // ROUND-6: MODE MAP bars (hero column). Per-mode strike gains from the
        // bilinear sound-map (baked ModalData + current preset / strike X/Y /
        // Modes / band trims from paramCache). Recomputed only when one of
        // those params changes (fModeMapDirty, set in parameterChanged);
        // heights are cached so idle ticks do no LVGL work. Amber only for
        // the peak-gain mode (one-accent discipline), PLATE_EDGE for the rest.
        if(fModeBars[0] && fModeMapDirty){
            fModeMapDirty=false;
            using namespace modal;
            int mxp=kNumPresets-1;
            int preset=(int)std::round(paramCache[PluginMultiScaleBody::kParamPreset]*(float)mxp);
            const auto& pr=kPresets[std::clamp(preset,0,mxp)];
            int n=std::clamp((int)(8+paramCache[PluginMultiScaleBody::kParamModeCount]*120.f),8,pr.n);
            float sx=paramCache[PluginMultiScaleBody::kParamStrikeX];
            float sy=paramCache[PluginMultiScaleBody::kParamStrikeY];
            float fx=sx*15.f, fy=sy*15.f;
            int x0=std::clamp((int)fx,0,14), y0=std::clamp((int)fy,0,14);
            int x1=x0+1, y1=y0+1; float dx=fx-x0, dy=fy-y0;
            float w00=(1-dx)*(1-dy), w10=dx*(1-dy), w01=(1-dx)*dy, w11=dx*dy;
            // wave-3 (idea 4): blend the morph target's sound map
            float mAmtM=paramCache[PluginMultiScaleBody::kParamMorphAmt];
            const auto& ptM=(mAmtM>1e-4f)?kPresets[std::clamp((int)std::lround(paramCache[PluginMultiScaleBody::kParamMorphTarget]*(float)mxp),0,mxp)]:pr;
            const int tNM=std::max(1,ptM.n);
            float gmax=1e-9f;
            for(int m=0;m<n;++m){
                float g=pr.gain[m][y0][x0]*w00+pr.gain[m][y0][x1]*w10
                       +pr.gain[m][y1][x0]*w01+pr.gain[m][y1][x1]*w11;
                if(mAmtM>1e-4f){ const int ti=std::min(m,tNM-1);
                    float tg=ptM.gain[ti][y0][x0]*w00+ptM.gain[ti][y0][x1]*w10
                           +ptM.gain[ti][y1][x0]*w01+ptM.gain[ti][y1][x1]*w11;
                    g=g+(tg-g)*mAmtM; }
                g=std::fabs(g);
                int band=(m*16)/n;
                float trim=paramCache[PluginMultiScaleBody::kParamBand0+std::clamp(band,0,15)]*2.f;
                fModeGain[m]=g*trim;
                gmax=std::max(gmax,fModeGain[m]);
            }
            int peakM=0;
            float peakG=-1.f;
            const lv_coord_t maxH=(lv_coord_t)(scaled(lay::MAP_BARS_H)-2);
            for(int m=0;m<modal::kMaxModes;++m){
                // FIX: heights were never written - bars kept their 1px
                // construction height (the flat dotted line). Active modes
                // scale with gain/gmax over a 4% ambient floor; idle slots
                // stay 1px ticks. Peak is tracked here, styled next pass.
                lv_coord_t hh2=1;
                if(m<n && gmax>1e-9f){
                    float f=0.04f+0.96f*(fModeGain[m]/gmax);
                    hh2=(lv_coord_t)std::max(1,(int)std::lround(f*maxH));
                    if(fModeGain[m]>peakG){ peakG=fModeGain[m]; peakM=m; }
                }
                fModeMapH[m]=hh2;
                lv_obj_t* bar=fModeBars[m];
                if(bar) lv_obj_set_size(bar,scaled(lay::MAP_BAR_W),hh2);
            }
            for(int m=0;m<modal::kMaxModes;++m){
                lv_obj_t* bar=fModeBars[m];
                if(!bar) continue;
                if(m==peakM && fModeMapH[m]>1){
                    lv_obj_set_style_bg_color(bar,COL_HIGHLIGHT,0);
                    lv_obj_set_style_bg_opa(bar,LV_OPA_COVER,0);
                } else {
                    lv_obj_set_style_bg_color(bar,PLATE_AMBER,0);
                    lv_obj_set_style_bg_opa(bar,LV_OPA_80,0);
                }
            }   // end style pass (peak known)
            if(fModePeakLbl){
                // wave-4: always refresh (the peak label now tracks the
                // physical model: morph/res/support/material/Tune move the
                // effective frequency even when the peak mode is unchanged)
                fModePeakIdx=peakM;
                char nm[24];
                snprintf(nm,sizeof(nm),"M%d  -  %.0f HZ",peakM+1,
                         uiEffFreqHz(pr,peakM,preset,n));
                lv_label_set_text(fModePeakLbl,nm);
            }
        }   // end if(fModeBars[0] && fModeMapDirty)
        lv_chart_refresh(fSpectrumChart);
        // wave-3 (idea 5): repaint the disc heatmap when gain-affecting
        // params, the mode count, or the node focus changed (serviced here so
        // repaints batch with the 33 ms mode-map cadence, never per gesture).
        if(fHeatDirty){ fHeatDirty=false; paintDiscHeatmap(); }
        // wave-3 (idea 10): advance the motion-recorder playback (33 ms steps)
        playbackTick();
        if(fRippleCooldown>0) --fRippleCooldown;
        bool onset=(totalE>fPrevEnergy+std::max(0.02f,fPrevEnergy*1.1f)) && totalE>0.04f;
        fPrevEnergy=std::max(totalE,fPrevEnergy*0.90f);
        if(onset && fRippleCooldown==0 && live){ spawnRipple(); fRippleCooldown=9; }
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
        // R3: decay-scope idle preview — when there's no audio clock yet, the
        // scope draws the preset's real per-mode decay envelope (attack ramp
        // + exponential tail computed from ModalData). This is the "scope
        // does something" fix: the right column stops reading as dead.
        if(fScopeChart && fScopeSeries && !live){
            if(!fScopePreviewReady) buildScopePreview();
            if(fScopePreviewReady){
                int idx=fScopePreviewIdx;
                int32_t v=(int32_t)(fScopePreview[idx]*980.f);
                lv_chart_set_next_value(fScopeChart,(lv_chart_series_t*)fScopeSeries,v);
                fScopePreviewIdx=(fScopePreviewIdx+1)&127;
            }
        }
        gScopeMax=std::max(std::max(totalE,gScopeMax*0.995f),0.03f);
        fLevelEnv+=(totalE-fLevelEnv)*(totalE>fLevelEnv?0.55f:0.12f);
        float lvl=std::clamp(fLevelEnv/gScopeMax,0.f,1.f);
        if(live && fScopeChart && fScopeSeries)
            lv_chart_set_next_value(fScopeChart,(lv_chart_series_t*)fScopeSeries,(int32_t)(lvl*980.f));
        if(strikeDisc)
            lv_obj_set_style_border_opa(strikeDisc,(lv_opa_t)(70+185.f*lvl),0);
        if(lfoDot)
            lv_obj_set_style_bg_opa(lfoDot,(lv_opa_t)(40+215.f*lvl),0);
        // ~500 ms peak hold then decay (15 frames @ 30 fps)
        if(fLevelBar){
            float v=std::clamp(live?fVizLevel:0.f,0.f,1.f);
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
    // fRebuildInFlight: true while rebuildForScale() is mid-rebuild. uiReshape
    // and uiIdle both call rebuildForScale; if a resize re-enters while the
    // first rebuild is still constructing the new tree, the second pass can
    // lv_obj_clean mid-construction and leave the screen blank. Guard it.
    bool fRebuildInFlight=false;
    lv_obj_t* widgets[PluginMultiScaleBody::kParameterCount]={};
    float paramCache[PluginMultiScaleBody::kParameterCount]={};

    // master knob value label (re-uses the widget's own label, but the chip in
    // the dial bank's Wet knob is the canonical one - master just inherits it)
    // FIX: the widget's own chip stays hidden (it overflows the 30px master
    // row); fMasterValLbl is an owned label beside the arc, updated by
    // valueFormatCb + explicit Wet sync paths.
    lv_obj_t* fMasterValLbl=nullptr;
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
    void* fScopeAreaSeries=nullptr;
    lv_obj_t* fLevelBar=nullptr;
    lv_obj_t* fLevelPeak=nullptr;
    lv_obj_t* zoneWarnMark=nullptr;
    lv_obj_t* zoneHotMark=nullptr;
    float fLevelEnv=0.f;
    float fPrevEnergy=0.f;
    float gScopeMax=0.05f;
    float fMeterEnv=0.f;
    float fMeterPeak=0.f;
    int fStrikeNote=60;
    bool fStrikeHeld=false;
    // UI-strike MPE channel: disc hits rotate over member channels 1..15
    // so each strike is its own MPE note (per-note bend/pressure from the
    // host applies per channel; voices are keyed note+channel). The
    // keyboard stays ch0 legacy piano. fNextStrikeChannel is the next
    // member channel to allocate; fStrikeChannel is the live held one.
    int fStrikeChannel=0;
    int fNextStrikeChannel=1;
    bool fMarkerPlaced=false;
    lv_obj_t* strikeLastMark=nullptr;        // round-2: small amber dot that persists ~0.5s post-hit
    int fLastStrikeAgeMs=0;                 // ms since placeLastStrike; -1 = inactive
    int fRippleCooldown=0;
    // written level, so PRESSING writes only on real change (no host spam)
    int fScrubBand=-1;
    float fScrubLevel=-1.f;
    float fVizLevel=0.f;
    float fVizBins[16]={};
    bool fGotLiveViz=false;
    // Live-data freshness: metering outputs reset fLiveAge to 0 on every
    // audio block; updateSpectrumDisplay increments it each 33ms tick and
    // treats age>=45 (~1.5s of silence) as idle so the decay preview
    // resumes instead of flatlining forever after the first note.
    int fLiveAge=1000;
    // R3: idle decay-envelope preview — the scope chart draws the preset's
    // real per-mode decay curve (from ModalData, no audio needed) so the
    // right column never reads as dead hardware. When live viz arrives,
    // the trace switches to the audio-rate envelope.
    float fScopePreview[128]={};
    bool fScopePreviewReady=false;
    int fScopePreviewIdx=0;
    lv_obj_t* arpBtn=nullptr;
    bool arpOnLocal=false;
    // keyboard - round-6: 3 octaves (C3-B5) spanning the full footer strip
    lv_obj_t* kbContainer=nullptr;
    lv_obj_t* kbWhite[lay::KEY_WHITE_N];
    lv_obj_t* kbBlack[lay::KEY_BLACK_N];
    lv_obj_t* kbOctLabel=nullptr;
    int kbBaseNote=48;
    int kbHeldNote=-1;
    // header zoom stepper
    lv_obj_t* zoomMinus=nullptr;
    lv_obj_t* zoomPlus=nullptr;
    lv_obj_t* zoomValLbl=nullptr;
    int fZoomIdx=2;   // 100% (index into lay::ZOOM_STEPS)
    // spectrum peak-hold caps (falling-hold markers above the bars)
    float fSpecPeaks[16]={};
    int fSpecHoldAge[16]={};
    // ROUND-6: MODE MAP (per-mode strike-gain comb, disc column). Gains come
    // from the baked ModalData sound-map at the current strike position;
    // recomputation is gated by fModeMapDirty (preset/strike/modes/band
    // changes) and cached into fModeMapH so idle ticks do no LVGL work.
    lv_obj_t* fModeBars[modal::kMaxModes]={};
    float fModeGain[modal::kMaxModes]={};
    lv_coord_t fModeMapH[modal::kMaxModes]={};
    lv_obj_t* fModePeakLbl=nullptr;
    int fModePeakIdx=-1;
    bool fModeMapDirty=true;
    // wave-3 (idea 5): disc heatmap repaint gating + node-focus overlay.
    // fHeatDots tracks painted dots for deletion; fModeFocus selects the
    // mode whose |gain| map is shown (-1 = aggregate max over modes).
    lv_obj_t* fHeatDots[100]={};
    int fHeatCount=0;
    bool fHeatDirty=true;
    int fModeFocus=-1;
    // R5: DAMPING panel (per-band tail-time map, infoCol dead-band fill).
    // 16 bars, one per frequency band, each holding a normalized 0..1000
    // fill value; 16 value labels read "<T60> s" per band. fDampMax
    // caches the longest-band T60 (in seconds) for the current preset so
    // subsequent updates skip the loop when neither preset nor DECAY knob
    // moved (same gating as fModeMapDirty).
    lv_obj_t* fDampBars[16]={};
    lv_obj_t* fDampVals[16]={};
    float fDampMax=1.f;
    int fDampPresetCache=-1;
    float fDampDecayCache=-1.f;
    float fDampBandSumCache=-1.f;
    float fDampPhysSumCache=-1.f;   // wave-3 gate: support/hold/res/morph/rayleigh sum
    // idea 2: spectrum scrub target (0 = per-band GAIN, 1 = per-band DECAY)
    int fScrubMode=0;
    int fScrubParamIdx=-1;
    lv_obj_t* fScrubToggle=nullptr;
    // wave-4: MIDI-learn status chip (non-blocking; right-click knob -> bind next CC)
    lv_obj_t* fLearnChip=nullptr;
    lv_obj_t* fLearnLbl=nullptr;
    int fLearnParam=-1;
    // idea 13: microtonal scale status label (editor controls live inline now)
    lv_obj_t* fEdoDropdown=nullptr;
    lv_obj_t* fScaleLbl=nullptr;
    // wave-3 (idea 10): disc motion recorder (UI-local, not persisted)
    lv_obj_t* fRecBtn=nullptr;
    lv_obj_t* fPlayBtn=nullptr;
    bool fRecOn=false;
    bool fRecPlaying=false;
    float fRecX[512]={};
    float fRecY[512]={};
    int fRecN=0;
    int fRecCursor=0;
    int fPlayChannel=0;
    bool fPlayHeld=false;
    // wave-4: MODEL strip controls (persistent full-width card, no modal)
    lv_obj_t* fMaterialDd=nullptr;
    lv_obj_t* fMorphDd=nullptr;
    lv_obj_t* fEcoBtn=nullptr;
    std::string scaleTxtCached_;
};
UI* createUI(){ return new MultiScaleBodyUI(); }
END_NAMESPACE_DISTRHO
