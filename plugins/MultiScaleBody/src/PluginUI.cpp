#include "DistrhoPluginInfo.h"
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
static constexpr float CONTENT_H = 824.f;
class MultiScaleBodyUI;
static const char* sParamNames[PluginMultiScaleBody::kParameterCount] = {
    "Tune","Decay","Bright","Strike X","Strike Y","Modes","Width","Body",
    "B1","B2","B3","B4","B5","B6","B7","B8","B9","B10","B11","B12","B13","B14","B15","B16",
    "Radiation","Attack","Release","LFO Rate","LFO Depth",
    "Exciter","Vel Strike","Imperfect","Glide","Reverb","Mono",
    "Level Out",
    "B1 Out","B2 Out","B3 Out","B4 Out","B5 Out","B6 Out","B7 Out","B8 Out",
    "B9 Out","B10 Out","B11 Out","B12 Out","B13 Out","B14 Out","B15 Out","B16 Out"
};
static float peakOf(const float* bins){
    float m=0.f; for(int b=0;b<16;++b) m=std::max(m,bins[b]); return m;
}
// === STRIKE PLATE ripple machinery (expanding rings from the hit point) ===
static void rippleSizeCb(void* var,int32_t v){ lv_obj_t* r=(lv_obj_t*)var; if(r) lv_obj_set_size(r,v,v); }
static void rippleOpaCb(void* var,int32_t v){ lv_obj_t* r=(lv_obj_t*)var; if(r) lv_obj_set_style_border_opa(r,(lv_opa_t)v,0); }
static void rippleDelCb(lv_anim_t* a){ lv_obj_t* r=(lv_obj_t*)a->var; if(r) lv_obj_del(r); }

class MultiScaleBodyUI : public UI, public AbstractMultiScaleBodyUI {
public:
    MultiScaleBodyUI(): UI(DISTRHO_UI_DEFAULT_WIDTH, DISTRHO_UI_DEFAULT_HEIGHT),
        fLVGL(nullptr){
        // widget maps + param cache MUST be initialized before buildUI():
        // buildUI() reads paramCache (strike dot pos, preset selection) and writes widgets[]
        for(uint32_t i=0;i<PluginMultiScaleBody::kParameterCount;++i){ widgets[i]=nullptr; labels[i]=nullptr; paramCache[i]=0.5f; }
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
        for(int i=0;i<5;++i) kbBlack[i]=nullptr;
        for(int i=0;i<7;++i) kbWhite[i]=nullptr;
        setSize(DISTRHO_UI_DEFAULT_WIDTH, DISTRHO_UI_DEFAULT_HEIGHT);
        fLVGL = new DGL_NAMESPACE::LVGLTopLevelWidget(getWindow());
        styles.init(); spec = normalArcSpec();
        buildUI();
        if(fUIBuilt && !fSpectrumTimer){
            fSpectrumTimer = lv_timer_create([](lv_timer_t* t){
                auto ui=(MultiScaleBodyUI*)lv_timer_get_user_data(t); if(ui) ui->updateSpectrumDisplay();
            },33,this);
        }
    }
    ~MultiScaleBodyUI() override { if(fSpectrumTimer){ lv_timer_del(fSpectrumTimer); fSpectrumTimer=nullptr; } delete fLVGL; }
    std::string parameterName(uint32_t i) const override { if(i<PluginMultiScaleBody::kParameterCount) return sParamNames[i]; return {}; }
    float getParamValue(uint32_t i) const override { if(i<PluginMultiScaleBody::kParameterCount) return paramCache[i]; return 0.f; }
    void setParamValue(uint32_t i,float v) override {
        if(i>=PluginMultiScaleBody::kParameterCount) return;
        paramCache[i]=v; setParameterValue(i,v); syncParamWidget(i,v);
        if(i==PluginMultiScaleBody::kParamStrikeX || i==PluginMultiScaleBody::kParamStrikeY) updateStrikeMarker();
        if(i==PluginMultiScaleBody::kParamPreset){ syncPresetDropdown(v); if(bodySubLabel) updateBodyInfo(); updateBodyPreview(); }
    }
    void editParameter(uint32_t i,bool s) override { if(i<PluginMultiScaleBody::kParameterCount) UI::editParameter(i,s); }
    void syncParamWidget(uint32_t i,float v) override {
        if(i>=PluginMultiScaleBody::kParameterCount) return;
        if(widgets[i]) UIWidgets::syncFromParam(widgets[i],v);
        if(labels[i]) UIWidgets::updateLabel(labels[i],v);
    }
    void parameterChanged(uint32_t i,float v) override {
        // metering outputs arrive here every audio block - the bridge-safe DSP->UI link
        if(i==PluginMultiScaleBody::kParamOutLevel){ fVizLevel=v; return; }
        if(i>=PluginMultiScaleBody::kParamOutBand0 && i<PluginMultiScaleBody::kParameterCount){
            fVizBins[i-PluginMultiScaleBody::kParamOutBand0]=v;
            fGotLiveViz=true;
            return;
        }
        if(i<PluginMultiScaleBody::kParameterCount){ paramCache[i]=v; syncParamWidget(i,v);
            if(i==PluginMultiScaleBody::kParamStrikeX || i==PluginMultiScaleBody::kParamStrikeY) updateStrikeMarker();
            if(i==PluginMultiScaleBody::kParamPreset){ syncPresetDropdown(v); if(bodySubLabel) updateBodyInfo(); updateBodyPreview(); } }
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
        // uiReshape - keep the scale in sync from the real window size
        const DGL_NAMESPACE::Size<uint> sz=getSize();
        float nsW=(float)sz.getWidth()/(float)DISTRHO_UI_DEFAULT_WIDTH;
        float nsH=(float)sz.getHeight()/CONTENT_H;
        float ns=std::clamp(nsW < nsH ? nsW : nsH,0.5f,2.5f);
        if(std::abs(ns-::DISTRHO::gUIScale)>=0.02f) rebuildForScale(ns);
        UI::uiIdle();
    }
    void uiReshape(uint w,uint h) override {
        UI::uiReshape(w,h);
        float nsW=(float)w/(float)DISTRHO_UI_DEFAULT_WIDTH;
        float nsH=(float)h/CONTENT_H;
        float ns=std::clamp(nsW < nsH ? nsW : nsH,0.5f,2.5f);
        rebuildForScale(ns);
    }
    void rebuildForScale(float ns){
        if(std::abs(ns-::DISTRHO::gUIScale)<0.02f) return;
        ::DISTRHO::gUIScale=ns;
        if(!fUIBuilt) return;
        styles.reset(); styles.init(); spec=normalArcSpec();
        if(kbHeldNote>=0 && kbHeldNote<=127){ sendNote(0,(uint8_t)kbHeldNote,0); kbHeldNote=-1; }
        for(int o=0;o<12;++o){ int n=kbBaseNote+o; if(n>=0&&n<=127) sendNote(0,(uint8_t)n,0); }
        lv_obj_t* root=lv_screen_active(); if(root) lv_obj_clean(root);
        fUIBuilt=false;
        for(uint32_t i=0;i<PluginMultiScaleBody::kParameterCount;++i){ widgets[i]=nullptr; labels[i]=nullptr; }
        strikeDot=nullptr; presetDropdown=nullptr; fSpectrumChart=nullptr; bodySubLabel=nullptr; strikeCoordLabel=nullptr;
        bodyPreview=nullptr; lfoDot=nullptr;
        strikeDisc=nullptr; hdrBodyVal=nullptr; hdrMatVal=nullptr; hdrModeVal=nullptr; hdrF0Val=nullptr;
        fScopeChart=nullptr; fScopeSeries=nullptr; lfoDot=nullptr;
        fPrevEnergy=0.f; fLevelEnv=0.f; gScopeMax=0.05f; fRippleCooldown=0; fStrikeHeld=false;
        fMarkerPlaced=false;
        arpBtn=nullptr;
        kbContainer=nullptr; kbOctLabel=nullptr;
        for(int i=0;i<7;++i) kbWhite[i]=nullptr;
        for(int i=0;i<5;++i) kbBlack[i]=nullptr;
        buildUI();
        if(fUIBuilt && !fSpectrumTimer){
            fSpectrumTimer = lv_timer_create([](lv_timer_t* t){
                auto ui=(MultiScaleBodyUI*)lv_timer_get_user_data(t); if(ui) ui->updateSpectrumDisplay();
            },33,this);
        }
    }
    bool onScroll(const Widget::ScrollEvent& e) override {
        float dx=e.delta.getX(); float dy=e.delta.getY();
        if(dx!=0||dy!=0){
            lv_point_t p; p.x=(lv_coord_t)e.pos.getX(); p.y=(lv_coord_t)e.pos.getY();
            lv_obj_t* hovered=lv_indev_search_obj(lv_layer_top(),&p);
            if(!hovered) hovered=lv_indev_search_obj(lv_scr_act(),&p);
            lv_obj_t* target=nullptr; lv_obj_t* node=hovered;
            while(node){ if(lv_obj_has_flag(node,LV_OBJ_FLAG_SCROLLABLE)){ target=node; break; } node=lv_obj_get_parent(node); }
            if(target){ lv_obj_scroll_by_bounded(target,(lv_coord_t)(dx*50),(lv_coord_t)(dy*50),LV_ANIM_OFF); }
        }
        return true;
    }
private:
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
        else if(n=="Bar" || n=="Chime" || n=="Plate" || n=="Gong") mat="Steel";
        else if(n=="Bell" || n=="Shell") mat="Bronze";
        else if(n=="Bowl" || n=="Blade") mat="Aluminium";
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
        const int g=4;
        int cell = scaled(18); int gap=scaled(2);
        int grid = g*cell + (g-1)*gap;
        int off = (scaled(72)-grid)/2;
        for(int y=0;y<g;++y){
            for(int x=0;x<g;++x){
                float occ=0;
                if(name=="Bowl"){ bool inner = x>=1 && x<=2 && y>=1 && y<=2; occ = inner?0.15f:1.f; }
                else if(name=="Plate"){ occ = (y==3)?1.f:(y==2?0.5f:0.f); }
                else if(name=="Squirrel"){ occ = (x>=1&&x<=2&&y>=1&&y<=2)?1.f:0.f; if(x==2&&y==3) occ=0.7f; if(x==0&&y==2) occ=0.5f; }
                else if(name=="Blade"){ occ = (x>=1&&x<=2&&y>=1&&y<=2)?1.f:0.f; if((x==0||x==3)&&y==2) occ=0.9f; }
                else if(name=="Shell"){ float dx=x-1.5f, dy=y-1.5f; float r=std::sqrt(dx*dx+dy*dy); if(r>1.2f&&r<1.9f) occ=1.f; else if(r>1.f&&r<2.1f) occ=0.5f; }
                else if(name=="Bar"){ occ=(y==1)?1.f:0.f; }
                else if(name=="Membrane"){ float dx=x-1.5f, dy=y-1.5f; if(dx*dx+dy*dy < 3.2f) occ=1.f; }
                else if(name=="Bell"){ float dx=x-1.5f, dy=y-1.5f; float r=std::sqrt(dx*dx+dy*dy); if(r>1.f&&r<1.9f) occ=1.f; else if(r>0.9f&&r<2.05f) occ=0.45f; occ = std::max(occ, (y==0?0.6f:0.f)); }
                else if(name=="Glass"){ bool inner = x>=1&&x<=2&&y>=1&&y<=2; occ = inner?0.08f:1.f; }
                else if(name=="Chime"){ float dy=y-1.5f; float r=std::abs(dy); if(r>0.7f&&r<1.15f) occ=1.f; else if(r>0.6f&&r<1.25f) occ=0.4f; }
                else if(name=="Gong"){ float dx=x-1.5f, dy=y-1.5f; float rad=std::sqrt(dx*dx+dy*dy); if(rad<1.9f) occ=1.f; else if(rad<2.08f) occ=0.35f; if(rad<0.88f) occ=1.f; }
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
                    lv_color_t base = COL_HIGHLIGHT;
                    if(name=="WoodBlock"||name=="Squirrel") base = MAT_WOOD;
                    else if(name=="Glass") base = MAT_GLASS;
                    else if(name=="Membrane") base = MAT_MEMBRANE;
                    else if(name=="Bell"||name=="Gong"||name=="Shell") base = COL_HIGHLIGHT;
                    else if(name=="Plate"||name=="Bar"||name=="Chime") base = MAT_STEEL;
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
    void updateStrikeMarker(){
        if(!strikeDisc || !strikeDot) return;
        lv_coord_t pw = lv_obj_get_width(strikeDisc);
        lv_coord_t ph = lv_obj_get_height(strikeDisc);
        int dotS = scaled(12);
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
            int d0=scaled(14);
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
    static void dropdownCb(lv_event_t* e){
        auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e);
        lv_obj_t* dd=(lv_obj_t*)lv_event_get_target(e);
        int sel=lv_dropdown_get_selected(dd);
        int mx = modal::kNumPresets - 1;
        float v = mx ? (float)sel/(float)mx : 0.f;
        if(ui){ ui->editParameter(PluginMultiScaleBody::kParamPreset,true); ui->setParamValue(PluginMultiScaleBody::kParamPreset, v); ui->editParameter(PluginMultiScaleBody::kParamPreset,false); }
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
            if(code==LV_EVENT_PRESSED) ui->editParameter(PluginMultiScaleBody::kParamBand0+band,true);
            ui->setParamValue(PluginMultiScaleBody::kParamBand0+band, level);
        } else if(code==LV_EVENT_RELEASED){
            ui->editParameter(PluginMultiScaleBody::kParamBand0+band,false);
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
    void toggleFullscreen(){
        if(!isFullscreen){
            preFsSize=getSize();
            int sw=1920, sh=1080;
            const double aspect=(double)DISTRHO_UI_DEFAULT_WIDTH/(double)DISTRHO_UI_DEFAULT_HEIGHT;
            int fsW=int(sh*aspect+0.5), fsH=sh;
            if(fsW>sw){ fsW=sw; fsH=int(fsW/aspect+0.5); }
            setSize(fsW,fsH); isFullscreen=true;
        } else { setSize(preFsSize); isFullscreen=false; }
    }
    static void fsBtnCb(lv_event_t* e){ auto* ui=(MultiScaleBodyUI*)lv_event_get_user_data(e); if(ui) ui->toggleFullscreen(); }
    void createKeyboard(lv_obj_t* parent){
        kbContainer=lv_obj_create(parent);
        // full-width anchor strip pinned under the stage - compact, explicit height
        lv_obj_set_size(kbContainer, lv_pct(100), scaled(148));
        lv_obj_set_style_bg_color(kbContainer, KB_WELL,0);
        lv_obj_set_style_bg_opa(kbContainer, LV_OPA_COVER,0);
        lv_obj_set_style_border_color(kbContainer, COL_HAIRLINE,0);
        lv_obj_set_style_border_width(kbContainer,1,0);
        lv_obj_set_style_radius(kbContainer, scaled(10),0);
        lv_obj_set_style_pad_all(kbContainer, scaled(8),0);
        lv_obj_set_style_pad_row(kbContainer, scaled(6),0);
        lv_obj_set_layout(kbContainer, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(kbContainer, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(kbContainer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(kbContainer, LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(kbContainer, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* kbHeader=lv_obj_create(kbContainer);
        lv_obj_set_size(kbHeader, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(kbHeader, LV_OPA_TRANSP,0); lv_obj_set_style_border_width(kbHeader,0,0); lv_obj_set_style_pad_all(kbHeader,0,0);
        lv_obj_set_layout(kbHeader, LV_LAYOUT_FLEX); lv_obj_set_flex_flow(kbHeader, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(kbHeader, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(kbHeader, LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(kbHeader, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* kbTitle=lv_label_create(kbHeader); lv_label_set_text(kbTitle,"KEYBOARD  -  1 octave  -  click to audition"); lv_obj_add_style(kbTitle,&styles.knobTitleSmall,0);
        lv_obj_set_style_text_color(kbTitle, COL_HIGHLIGHT,0); lv_obj_set_style_text_font(kbTitle,&lv_font_montserrat_12,0);
        lv_obj_t* octRow=lv_obj_create(kbHeader);
        lv_obj_set_style_bg_opa(octRow, LV_OPA_TRANSP,0); lv_obj_set_style_border_width(octRow,0,0); lv_obj_set_style_pad_all(octRow,0,0);
        lv_obj_set_layout(octRow, LV_LAYOUT_FLEX); lv_obj_set_flex_flow(octRow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(octRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(octRow, scaled(6),0); lv_obj_set_scrollbar_mode(octRow, LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(octRow, LV_OBJ_FLAG_SCROLLABLE);
        arpBtn=lv_btn_create(octRow); lv_obj_set_size(arpBtn,scaled(46),scaled(22));
        styles.applyToggleButton(arpBtn,arpOnLocal);
        lv_obj_set_style_radius(arpBtn,scaled(4),0); lv_obj_set_style_pad_all(arpBtn,0,0);
        lv_obj_add_event_cb(arpBtn,arpBtnCb,LV_EVENT_VALUE_CHANGED,this);
        lv_obj_t* albl=lv_label_create(arpBtn); lv_label_set_text(albl,"ARP"); lv_obj_center(albl);
        lv_obj_t* octDown=lv_btn_create(octRow); lv_obj_set_size(octDown, scaled(28), scaled(22)); lv_obj_add_style(octDown,&styles.btnMain,0); lv_obj_set_style_radius(octDown, scaled(4),0); lv_obj_set_style_pad_all(octDown,0,0);
        lv_obj_set_user_data(octDown, (void*)(intptr_t)-12); lv_obj_add_event_cb(octDown, octaveBtnCb, LV_EVENT_CLICKED, this);
        lv_obj_t* dl=lv_label_create(octDown); lv_label_set_text(dl,"<"); lv_obj_center(dl); lv_obj_set_style_text_color(dl, COL_TEXT,0);
        kbOctLabel=lv_label_create(octRow); lv_label_set_text(kbOctLabel,"C4 - B4"); lv_obj_add_style(kbOctLabel,&styles.labelSmall,0); lv_obj_set_style_text_color(kbOctLabel, COL_TEXT_DIM,0); lv_obj_set_style_text_font(kbOctLabel,&lv_font_montserrat_10,0); lv_obj_set_width(kbOctLabel, scaled(70)); lv_obj_set_style_text_align(kbOctLabel, LV_TEXT_ALIGN_CENTER,0);
        lv_obj_t* octUp=lv_btn_create(octRow); lv_obj_set_size(octUp, scaled(28), scaled(22)); lv_obj_add_style(octUp,&styles.btnMain,0); lv_obj_set_style_radius(octUp, scaled(4),0); lv_obj_set_style_pad_all(octUp,0,0);
        lv_obj_set_user_data(octUp, (void*)(intptr_t)12); lv_obj_add_event_cb(octUp, octaveBtnCb, LV_EVENT_CLICKED, this);
        lv_obj_t* ul=lv_label_create(octUp); lv_label_set_text(ul,">"); lv_obj_center(ul); lv_obj_set_style_text_color(ul, COL_TEXT,0);
        lv_obj_t* kbRow=lv_obj_create(kbContainer);
        lv_obj_set_size(kbRow, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(kbRow, LV_OPA_TRANSP,0); lv_obj_set_style_border_width(kbRow,0,0); lv_obj_set_style_pad_all(kbRow,0,0);
        lv_obj_set_layout(kbRow, LV_LAYOUT_FLEX); lv_obj_set_flex_flow(kbRow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(kbRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(kbRow, LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(kbRow, LV_OBJ_FLAG_SCROLLABLE);
        int whiteW = scaled(56); int whiteH = scaled(80); int blackW = scaled(28); int blackH = scaled(48); int gap = scaled(2);
        int keysW = 7*whiteW + 6*gap; int keysH = whiteH;
        lv_obj_t* keysBox=lv_obj_create(kbRow);
        lv_obj_set_size(keysBox, keysW, keysH);
        lv_obj_set_style_bg_opa(keysBox, LV_OPA_TRANSP,0); lv_obj_set_style_border_width(keysBox,0,0); lv_obj_set_style_pad_all(keysBox,0,0); lv_obj_clear_flag(keysBox, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(keysBox, LV_LAYOUT_NONE);
        static const char* whiteName[7]={"C","D","E","F","G","A","B"}; static const int whiteOff[7]={0,2,4,5,7,9,11}; static const int blackOff[5]={1,3,6,8,10};
        int bX[5]; int stepW=whiteW+gap; bX[0]=stepW*1-blackW/2; bX[1]=stepW*2-blackW/2; bX[2]=stepW*4-blackW/2; bX[3]=stepW*5-blackW/2; bX[4]=stepW*6-blackW/2;
        for(int i=0;i<7;++i){
            lv_obj_t* w=lv_btn_create(keysBox); lv_obj_set_size(w,whiteW,whiteH); lv_obj_set_pos(w,i*stepW,0);
            lv_obj_set_style_bg_color(w,COL_KNOB,0); lv_obj_set_style_bg_opa(w,LV_OPA_COVER,0); lv_obj_set_style_border_color(w,COL_HAIRLINE,0); lv_obj_set_style_border_width(w,1,0); lv_obj_set_style_radius(w,scaled(5),0); lv_obj_set_style_pad_all(w,0,0);
            lv_obj_clear_flag(w,LV_OBJ_FLAG_SCROLLABLE);
            int note=kbBaseNote+whiteOff[i]; lv_obj_set_user_data(w,(void*)(intptr_t)note);
            lv_obj_add_event_cb(w,keyEventCb,LV_EVENT_PRESSED,this); lv_obj_add_event_cb(w,keyEventCb,LV_EVENT_RELEASED,this); lv_obj_add_event_cb(w,keyEventCb,LV_EVENT_PRESS_LOST,this); lv_obj_add_event_cb(w,keyEventCb,LV_EVENT_LEAVE,this);
            kbWhite[i]=w; lv_obj_t* lbl=lv_label_create(w); int oct=note/12-1; char buf[8]; snprintf(buf,sizeof(buf),"%s%d",whiteName[i],oct); lv_label_set_text(lbl,buf);
            lv_obj_add_style(lbl,&styles.labelSmall,0); lv_obj_set_style_text_color(lbl,COL_PANEL_DARK,0); lv_obj_set_style_text_font(lbl,&lv_font_montserrat_10,0); lv_obj_align(lbl,LV_ALIGN_BOTTOM_MID,0,-scaled(4)); lv_obj_clear_flag(lbl,LV_OBJ_FLAG_CLICKABLE);
        }
        for(int i=0;i<5;++i){
            lv_obj_t* b=lv_btn_create(keysBox); lv_obj_set_size(b,blackW,blackH); lv_obj_set_pos(b,bX[i],0);
            lv_obj_set_style_bg_color(b,KB_BLACK,0); lv_obj_set_style_bg_grad_color(b,KB_BLACK_HI,0); lv_obj_set_style_bg_grad_dir(b,LV_GRAD_DIR_VER,0); lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0);
            lv_obj_set_style_border_color(b,COL_HAIRLINE,0); lv_obj_set_style_border_width(b,1,0); lv_obj_set_style_radius(b,scaled(4),0); lv_obj_set_style_pad_all(b,0,0);
            lv_obj_set_style_shadow_width(b,scaled(4),0); lv_obj_set_style_shadow_color(b,COL_BLACK,0); lv_obj_set_style_shadow_opa(b,LV_OPA_30,0);
            lv_obj_clear_flag(b,LV_OBJ_FLAG_SCROLLABLE);
            int note=kbBaseNote+blackOff[i]; lv_obj_set_user_data(b,(void*)(intptr_t)note);
            lv_obj_add_event_cb(b,keyEventCb,LV_EVENT_PRESSED,this); lv_obj_add_event_cb(b,keyEventCb,LV_EVENT_RELEASED,this); lv_obj_add_event_cb(b,keyEventCb,LV_EVENT_PRESS_LOST,this); lv_obj_add_event_cb(b,keyEventCb,LV_EVENT_LEAVE,this);
            kbBlack[i]=b;
        }
        updateKeyboardNotes();
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
    void buildUI(){
        lv_obj_t* root=lv_screen_active();
        if(!root){ lv_display_t* d=lv_display_get_default(); if(d) root=lv_display_get_screen_active(d); }
        if(!root) return;
        fUIBuilt=true; fMarkerPlaced=false;
        // === STRIKE PLATE: warm true-black chassis, industrial amber, exposed structure ===
        lv_obj_set_style_bg_color(root,PLATE_BG,0); lv_obj_set_style_bg_opa(root,LV_OPA_COVER,0);
        lv_obj_set_layout(root,LV_LAYOUT_FLEX); lv_obj_set_flex_flow(root,LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(root,LV_FLEX_ALIGN_SPACE_BETWEEN,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_all(root,scaled(16),0); lv_obj_set_style_pad_row(root,scaled(10),0);
        lv_obj_set_scrollbar_mode(root,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(root,LV_OBJ_FLAG_SCROLLABLE);

        // ---- HEADER: Teenage-Engineering grid-of-specs (flat cells, hairline rules) ----
        lv_obj_t* header=lv_obj_create(root);
        lv_obj_set_size(header,lv_pct(100),scaled(64));
        lv_obj_set_style_bg_color(header,PLATE_PANEL,0); lv_obj_set_style_bg_opa(header,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(header,PLATE_LINE,0); lv_obj_set_style_border_width(header,1,0);
        lv_obj_set_style_radius(header,scaled(6),0);
        lv_obj_set_style_pad_hor(header,scaled(14),0); lv_obj_set_style_pad_ver(header,scaled(8),0); lv_obj_set_style_pad_column(header,scaled(12),0);
        lv_obj_set_layout(header,LV_LAYOUT_FLEX); lv_obj_set_flex_flow(header,LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(header,LV_FLEX_ALIGN_SPACE_BETWEEN,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(header,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(header,LV_OBJ_FLAG_SCROLLABLE);
        // Title cell
        lv_obj_t* hLeft=lv_obj_create(header);
        lv_obj_set_flex_grow(hLeft,3); lv_obj_set_height(hLeft,lv_pct(100));
        lv_obj_set_style_bg_opa(hLeft,0,0); lv_obj_set_style_border_width(hLeft,0,0); lv_obj_set_style_pad_all(hLeft,0,0);
        lv_obj_set_layout(hLeft,LV_LAYOUT_FLEX); lv_obj_set_flex_flow(hLeft,LV_FLEX_FLOW_COLUMN); lv_obj_set_flex_align(hLeft,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(hLeft,scaled(3),0);
        lv_obj_set_scrollbar_mode(hLeft,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(hLeft,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* title=lv_label_create(hLeft); lv_label_set_text(title,"MULTI-SCALE MODAL BODY");
        lv_obj_set_style_text_font(title,&lv_font_montserrat_24,0); lv_obj_set_style_text_color(title,PLATE_TITLE,0);
        lv_obj_set_style_text_letter_space(title,3,0);
        lv_obj_t* sub=lv_label_create(hLeft); lv_label_set_text(sub,"PICARD - FAURE - KRY - DRETTAKIS   -   DAFx-09 PAPER 47");
        lv_obj_set_style_text_font(sub,&lv_font_montserrat_10,0); lv_obj_set_style_text_color(sub,PLATE_TEXT_DIM,0);
        lv_obj_set_style_text_letter_space(sub,2,0);
        auto addDivider=[&](lv_obj_t* parent){
            lv_obj_t* d=lv_obj_create(parent);
            lv_obj_set_size(d,1,scaled(34));
            lv_obj_set_style_bg_color(d,PLATE_LINE,0); lv_obj_set_style_bg_opa(d,LV_OPA_COVER,0);
            lv_obj_set_style_border_width(d,0,0); lv_obj_set_style_pad_all(d,0,0);
            lv_obj_clear_flag(d,LV_OBJ_FLAG_CLICKABLE); lv_obj_clear_flag(d,LV_OBJ_FLAG_SCROLLABLE);
        };
        addDivider(header);
        // Spec cells: label over value - engineering documentation, not marketing copy
        auto addSpecCell=[&](const char* lab,lv_obj_t** valOut,lv_color_t valCol,int minw)->lv_obj_t*{
            lv_obj_t* cell=lv_obj_create(header);
            lv_obj_set_height(cell,lv_pct(100)); lv_obj_set_style_min_width(cell,scaled(minw),0);
            lv_obj_set_style_bg_opa(cell,0,0); lv_obj_set_style_border_width(cell,0,0);
            lv_obj_set_style_pad_all(cell,0,0);
            lv_obj_set_layout(cell,LV_LAYOUT_FLEX); lv_obj_set_flex_flow(cell,LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(cell,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_row(cell,scaled(2),0);
            lv_obj_set_scrollbar_mode(cell,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(cell,LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t* l=lv_label_create(cell); lv_label_set_text(l,lab);
            lv_obj_set_style_text_font(l,&lv_font_montserrat_10,0); lv_obj_set_style_text_color(l,PLATE_TEXT_DIM,0);
            lv_obj_set_style_text_letter_space(l,2,0);
            lv_obj_t* v=lv_label_create(cell);
            lv_obj_set_style_text_font(v,&lv_font_montserrat_14,0); lv_obj_set_style_text_color(v,valCol,0);
            lv_label_set_text(v,"-");
            *valOut=v;
            return cell;
        };
        addSpecCell("BODY",&hdrBodyVal,PLATE_AMBER,86);
        addSpecCell("MATERIAL",&hdrMatVal,PLATE_TEXT,92);
        addSpecCell("MODES",&hdrModeVal,PLATE_TEXT,62);
        addSpecCell("F0",&hdrF0Val,PLATE_AMBER,70);
        addDivider(header);
        lv_obj_t* fsBtn=lv_btn_create(header); lv_obj_set_size(fsBtn,scaled(104),scaled(30));
        lv_obj_set_style_bg_color(fsBtn,PLATE_PANEL,0); lv_obj_set_style_bg_opa(fsBtn,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(fsBtn,PLATE_EDGE,0); lv_obj_set_style_border_width(fsBtn,1,0);
        lv_obj_set_style_radius(fsBtn,scaled(5),0); lv_obj_set_style_pad_all(fsBtn,0,0);
        lv_obj_set_style_shadow_width(fsBtn,0,0);
        lv_obj_add_event_cb(fsBtn,fsBtnCb,LV_EVENT_CLICKED,this);
        lv_obj_t* fsLbl=lv_label_create(fsBtn); lv_label_set_text(fsLbl,"FULLSCREEN"); lv_obj_center(fsLbl);
        lv_obj_set_style_text_font(fsLbl,&lv_font_montserrat_10,0); lv_obj_set_style_text_color(fsLbl,PLATE_TEXT_MID,0);
        lv_obj_set_style_text_letter_space(fsLbl,2,0);

        // ---- STAGE: three zones - forge dials | the body | analysis ----
        // content-sized (left dial column defines height); root spreads sections
        fStagePtr=lv_obj_create(root);
        lv_obj_t* stage=fStagePtr;
        lv_obj_set_size(stage,lv_pct(100),LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(stage,0,0); lv_obj_set_style_border_width(stage,0,0);
        lv_obj_set_style_pad_all(stage,0,0); lv_obj_set_style_pad_column(stage,scaled(10),0);
        lv_obj_set_layout(stage,LV_LAYOUT_FLEX); lv_obj_set_flex_flow(stage,LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(stage,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START);
        lv_obj_set_scrollbar_mode(stage,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(stage,LV_OBJ_FLAG_SCROLLABLE);

        // LEFT - FORGE: four labeled dial groups (fixed machined widths)
        fLeftPtr=lv_obj_create(stage);
        lv_obj_t* left=fLeftPtr;
        lv_obj_set_width(left,scaled(404)); lv_obj_set_height(left,LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(left,0,0); lv_obj_set_style_border_width(left,0,0);
        lv_obj_set_style_pad_all(left,0,0); lv_obj_set_style_pad_row(left,scaled(4),0);
        lv_obj_set_layout(left,LV_LAYOUT_FLEX); lv_obj_set_flex_flow(left,LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scrollbar_mode(left,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(left,LV_OBJ_FLAG_SCROLLABLE);
        const uint32_t groupParams[4][4]={
            {PluginMultiScaleBody::kParamPitch,PluginMultiScaleBody::kParamDecay,PluginMultiScaleBody::kParamBrightness,PluginMultiScaleBody::kParamModeCount},
            {PluginMultiScaleBody::kParamWidth,PluginMultiScaleBody::kParamRadiation,PluginMultiScaleBody::kParamDetune,PluginMultiScaleBody::kParamGlide},
            {PluginMultiScaleBody::kParamVelStrike,PluginMultiScaleBody::kParamExciteMix,PluginMultiScaleBody::kParamAttack,PluginMultiScaleBody::kParamRelease},
            {PluginMultiScaleBody::kParamLFORate,PluginMultiScaleBody::kParamLFODepth,PluginMultiScaleBody::kParamWet,PluginMultiScaleBody::kParamMono}
        };
        const char* groupNames[4]={"BODY","RESONATE","EXCITER","SPACE"};
        for(int g=0;g<4;++g){
            lv_obj_t* sec=lv_obj_create(left);
            lv_obj_set_size(sec,lv_pct(100),LV_SIZE_CONTENT);
            lv_obj_set_flex_grow(sec,1);
            lv_obj_set_style_bg_opa(sec,0,0); lv_obj_set_style_border_width(sec,0,0);
            lv_obj_set_style_pad_all(sec,0,0); lv_obj_set_style_pad_row(sec,scaled(4),0);
            lv_obj_set_layout(sec,LV_LAYOUT_FLEX); lv_obj_set_flex_flow(sec,LV_FLEX_FLOW_COLUMN);
            lv_obj_set_scrollbar_mode(sec,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(sec,LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t* slab=lv_label_create(sec); lv_label_set_text(slab,groupNames[g]);
            lv_obj_set_style_text_font(slab,&lv_font_montserrat_10,0);
            lv_obj_set_style_text_color(slab,g==0?PLATE_LABEL_ACCENT:PLATE_TEXT_DIM,0);
            lv_obj_set_style_text_letter_space(slab,3,0);
            lv_obj_t* grid=lv_obj_create(sec);
            lv_obj_set_size(grid,lv_pct(100),LV_SIZE_CONTENT);
            lv_obj_set_style_bg_opa(grid,0,0); lv_obj_set_style_border_width(grid,0,0);
            lv_obj_set_style_pad_all(grid,0,0); lv_obj_set_style_pad_column(grid,scaled(8),0); lv_obj_set_style_pad_row(grid,scaled(6),0);
            lv_obj_set_layout(grid,LV_LAYOUT_FLEX); lv_obj_set_flex_flow(grid,LV_FLEX_FLOW_ROW_WRAP);
            lv_obj_set_flex_align(grid,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START);
            lv_obj_set_scrollbar_mode(grid,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(grid,LV_OBJ_FLAG_SCROLLABLE);
            for(int k=0;k<4;++k)
                widgets[groupParams[g][k]]=UIWidgets::createArcKnob(grid,groupParams[g][k],this,styles,spec);
        }

        // CENTER - THE BODY: playable machined disc
        fCenterPtr=lv_obj_create(stage);
        lv_obj_t* center=fCenterPtr;
        lv_obj_set_width(center,scaled(372)); lv_obj_set_height(center,LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(center,PLATE_PANEL,0); lv_obj_set_style_bg_opa(center,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(center,PLATE_LINE,0); lv_obj_set_style_border_width(center,1,0);
        lv_obj_set_style_radius(center,scaled(6),0);
        lv_obj_set_style_pad_all(center,scaled(12),0); lv_obj_set_style_pad_row(center,scaled(8),0);
        lv_obj_set_layout(center,LV_LAYOUT_FLEX); lv_obj_set_flex_flow(center,LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(center,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(center,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(center,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* cHead=lv_obj_create(center);
        lv_obj_set_size(cHead,lv_pct(100),LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(cHead,0,0); lv_obj_set_style_border_width(cHead,0,0); lv_obj_set_style_pad_all(cHead,0,0);
        lv_obj_set_layout(cHead,LV_LAYOUT_FLEX); lv_obj_set_flex_flow(cHead,LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(cHead,LV_FLEX_ALIGN_SPACE_BETWEEN,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(cHead,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(cHead,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* cTitle=lv_label_create(cHead); lv_label_set_text(cTitle,"STRIKE THE BODY");
        lv_obj_set_style_text_font(cTitle,&lv_font_montserrat_12,0); lv_obj_set_style_text_color(cTitle,COL_HIGHLIGHT,0);
        lv_obj_set_style_text_letter_space(cTitle,2,0);
        lv_obj_t* cHint=lv_label_create(cHead); lv_label_set_text(cHint,"CLICK TO HIT");
        lv_obj_set_style_text_font(cHint,&lv_font_montserrat_10,0); lv_obj_set_style_text_color(cHint,PLATE_TEXT_DIM,0);
        lv_obj_set_style_text_letter_space(cHint,1,0);
        // The disc - top view of the resonant body
        const lv_coord_t D=scaled(300);
        strikeDisc=lv_obj_create(center);
        lv_obj_set_size(strikeDisc,D,D);
        lv_obj_set_style_radius(strikeDisc,LV_RADIUS_CIRCLE,0);
        lv_obj_set_style_bg_color(strikeDisc,PLATE_WELL,0);
        lv_obj_set_style_bg_grad_color(strikeDisc,PLATE_WELL_HI,0);
        lv_obj_set_style_bg_grad_dir(strikeDisc,LV_GRAD_DIR_VER,0);
        lv_obj_set_style_bg_opa(strikeDisc,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(strikeDisc,PLATE_EDGE,0);
        lv_obj_set_style_border_width(strikeDisc,1,0);
        lv_obj_set_style_border_opa(strikeDisc,70,0);
        lv_obj_set_style_pad_all(strikeDisc,0,0);
        lv_obj_set_style_shadow_width(strikeDisc,scaled(26),0);
        lv_obj_set_style_shadow_color(strikeDisc,COL_BLACK,0);
        lv_obj_set_style_shadow_opa(strikeDisc,LV_OPA_50,0);
        lv_obj_clear_flag(strikeDisc,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(strikeDisc,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(strikeDisc,padPressCb,LV_EVENT_PRESSED,this);
        lv_obj_add_event_cb(strikeDisc,padPressCb,LV_EVENT_PRESSING,this);
        lv_obj_add_event_cb(strikeDisc,padPressCb,LV_EVENT_RELEASED,this);
        lv_obj_add_event_cb(strikeDisc,padPressCb,LV_EVENT_PRESS_LOST,this);
        // concentric modal rings
        for(int r=0;r<3;++r){
            lv_coord_t rd=(lv_coord_t)(D*(0.30f+0.30f*r));
            lv_obj_t* ring=lv_obj_create(strikeDisc);
            lv_obj_set_size(ring,rd,rd);
            lv_obj_align(ring,LV_ALIGN_CENTER,0,0);
            lv_obj_set_style_radius(ring,LV_RADIUS_CIRCLE,0);
            lv_obj_set_style_bg_opa(ring,LV_OPA_TRANSP,0);
            lv_obj_set_style_border_color(ring,PLATE_LINE,0);
            lv_obj_set_style_border_width(ring,1,0);
            lv_obj_set_style_border_opa(ring,(lv_opa_t)(70-r*15),0);
            lv_obj_set_style_pad_all(ring,0,0);
            lv_obj_clear_flag(ring,LV_OBJ_FLAG_CLICKABLE); lv_obj_clear_flag(ring,LV_OBJ_FLAG_SCROLLABLE);
        }
        // crosshair
        lv_obj_t* chH=lv_obj_create(strikeDisc);
        lv_obj_set_size(chH,D,1); lv_obj_set_pos(chH,0,D/2);
        lv_obj_set_style_bg_color(chH,PLATE_LINE,0); lv_obj_set_style_bg_opa(chH,LV_OPA_40,0);
        lv_obj_set_style_border_width(chH,0,0); lv_obj_set_style_radius(chH,0,0); lv_obj_set_style_pad_all(chH,0,0);
        lv_obj_clear_flag(chH,LV_OBJ_FLAG_CLICKABLE); lv_obj_clear_flag(chH,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* chV=lv_obj_create(strikeDisc);
        lv_obj_set_size(chV,1,D); lv_obj_set_pos(chV,D/2,0);
        lv_obj_set_style_bg_color(chV,PLATE_LINE,0); lv_obj_set_style_bg_opa(chV,LV_OPA_40,0);
        lv_obj_set_style_border_width(chV,0,0); lv_obj_set_style_radius(chV,0,0); lv_obj_set_style_pad_all(chV,0,0);
        lv_obj_clear_flag(chV,LV_OBJ_FLAG_CLICKABLE); lv_obj_clear_flag(chV,LV_OBJ_FLAG_SCROLLABLE);
        // cardinal witness marks
        for(int t=0;t<4;++t){
            lv_obj_t* mk=lv_obj_create(strikeDisc);
            if(t==0||t==1) lv_obj_set_size(mk,2,scaled(10));
            else lv_obj_set_size(mk,scaled(10),2);
            lv_coord_t mx=(t==0||t==1)?D/2-1:(t==2?scaled(6):D-scaled(16));
            lv_coord_t my=(t<=1)?(t==0?scaled(6):D-scaled(16)):D/2-1;
            lv_obj_set_pos(mk,mx,my);
            lv_obj_set_style_bg_color(mk,PLATE_MARK,0); lv_obj_set_style_bg_opa(mk,LV_OPA_COVER,0);
            lv_obj_set_style_border_width(mk,0,0); lv_obj_set_style_radius(mk,0,0); lv_obj_set_style_pad_all(mk,0,0);
            lv_obj_clear_flag(mk,LV_OBJ_FLAG_CLICKABLE); lv_obj_clear_flag(mk,LV_OBJ_FLAG_SCROLLABLE);
        }
        // the mallet marker
        strikeDot=lv_obj_create(strikeDisc);
        lv_obj_set_size(strikeDot,scaled(12),scaled(12));
        lv_obj_set_style_bg_color(strikeDot,COL_HIGHLIGHT,0); lv_obj_set_style_bg_opa(strikeDot,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(strikeDot,PLATE_AMBER_PALE,0); lv_obj_set_style_border_width(strikeDot,1,0);
        lv_obj_set_style_radius(strikeDot,LV_RADIUS_CIRCLE,0); lv_obj_set_style_pad_all(strikeDot,0,0);
        lv_obj_set_style_shadow_width(strikeDot,scaled(14),0);
        lv_obj_set_style_shadow_color(strikeDot,COL_HIGHLIGHT,0);
        lv_obj_set_style_shadow_opa(strikeDot,LV_OPA_60,0);
        lv_obj_clear_flag(strikeDot,LV_OBJ_FLAG_CLICKABLE); lv_obj_clear_flag(strikeDot,LV_OBJ_FLAG_SCROLLABLE);
        lv_coord_t dsz=scaled(12);
        lv_obj_set_pos(strikeDot,(int)(paramCache[PluginMultiScaleBody::kParamStrikeX]*(D-dsz)),
                                (int)((1.f-paramCache[PluginMultiScaleBody::kParamStrikeY])*(D-dsz)));
        // coordinate readout
        strikeCoordLabel=lv_label_create(center);
        lv_label_set_text(strikeCoordLabel,"X 0.50  -  Y 0.50");
        lv_obj_set_style_text_font(strikeCoordLabel,&lv_font_montserrat_12,0);
        lv_obj_set_style_text_color(strikeCoordLabel,PLATE_AMBER,0);
        lv_obj_set_style_text_letter_space(strikeCoordLabel,1,0);
        // preset row: material jewel + selector
        lv_obj_t* presetRow=lv_obj_create(center);
        lv_obj_set_size(presetRow,lv_pct(100),LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(presetRow,0,0); lv_obj_set_style_border_width(presetRow,0,0);
        lv_obj_set_style_pad_all(presetRow,0,0); lv_obj_set_style_pad_column(presetRow,scaled(10),0);
        lv_obj_set_layout(presetRow,LV_LAYOUT_FLEX); lv_obj_set_flex_flow(presetRow,LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(presetRow,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(presetRow,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(presetRow,LV_OBJ_FLAG_SCROLLABLE);
        bodyPreview=lv_obj_create(presetRow);
        lv_obj_set_size(bodyPreview,scaled(52),scaled(52));
        lv_obj_set_style_bg_color(bodyPreview,PLATE_PREVIEW_BG,0); lv_obj_set_style_bg_opa(bodyPreview,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(bodyPreview,PLATE_EDGE,0); lv_obj_set_style_border_width(bodyPreview,1,0);
        lv_obj_set_style_radius(bodyPreview,scaled(8),0); lv_obj_set_style_pad_all(bodyPreview,scaled(4),0);
        lv_obj_clear_flag(bodyPreview,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* presetCol=lv_obj_create(presetRow);
        lv_obj_set_flex_grow(presetCol,1); lv_obj_set_height(presetCol,LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(presetCol,0,0); lv_obj_set_style_border_width(presetCol,0,0);
        lv_obj_set_style_pad_all(presetCol,0,0); lv_obj_set_style_pad_row(presetCol,scaled(4),0);
        lv_obj_set_layout(presetCol,LV_LAYOUT_FLEX); lv_obj_set_flex_flow(presetCol,LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scrollbar_mode(presetCol,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(presetCol,LV_OBJ_FLAG_SCROLLABLE);
        presetDropdown=lv_dropdown_create(presetCol);
        {
            std::string opts; for(int i=0;i<modal::kNumPresets;++i){ if(i) opts+="\n"; opts+=modal::kPresets[i].name; }
            lv_dropdown_set_options(presetDropdown,opts.c_str());
        }
        int mxp=modal::kNumPresets-1;
        int selp=(int)std::round(paramCache[PluginMultiScaleBody::kParamPreset]*(float)mxp);
        lv_dropdown_set_selected(presetDropdown,std::clamp(selp,0,mxp));
        lv_obj_set_width(presetDropdown,lv_pct(100));
        lv_obj_add_style(presetDropdown,&styles.compactSelectMain,0);
        lv_obj_set_style_bg_color(presetDropdown,PLATE_WELL,0);
        lv_obj_set_style_border_color(presetDropdown,PLATE_EDGE,0);
        lv_obj_t* list=lv_dropdown_get_list(presetDropdown); if(list) lv_obj_add_style(list,&styles.compactSelectListMain,0);
        lv_obj_add_event_cb(presetDropdown,dropdownCb,LV_EVENT_VALUE_CHANGED,this);
        bodySubLabel=lv_label_create(presetCol); lv_label_set_text(bodySubLabel,"");
        lv_obj_set_style_text_font(bodySubLabel,&lv_font_montserrat_10,0);
        lv_obj_set_style_text_color(bodySubLabel,PLATE_TEXT_MID,0);

        // RIGHT - ANALYSIS: mode spectrum + decay scope
        fRightPtr=lv_obj_create(stage);
        lv_obj_t* right=fRightPtr;
        lv_obj_set_height(right,LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(right,1);   // absorb remaining width (no horizontal overflow)
        lv_obj_set_style_bg_opa(right,0,0); lv_obj_set_style_border_width(right,0,0);
        lv_obj_set_style_pad_all(right,0,0); lv_obj_set_style_pad_row(right,scaled(10),0);
        lv_obj_set_layout(right,LV_LAYOUT_FLEX); lv_obj_set_flex_flow(right,LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scrollbar_mode(right,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(right,LV_OBJ_FLAG_SCROLLABLE);
        // spectrum card — fixed, generous height (matches the dial column)
        lv_obj_t* spectrumCard=lv_obj_create(right);
        lv_obj_set_size(spectrumCard,lv_pct(100),LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(spectrumCard,PLATE_PANEL,0); lv_obj_set_style_bg_opa(spectrumCard,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(spectrumCard,PLATE_LINE,0); lv_obj_set_style_border_width(spectrumCard,1,0);
        lv_obj_set_style_radius(spectrumCard,scaled(6),0);
        lv_obj_set_style_pad_all(spectrumCard,scaled(12),0); lv_obj_set_style_pad_row(spectrumCard,scaled(8),0);
        lv_obj_set_layout(spectrumCard,LV_LAYOUT_FLEX); lv_obj_set_flex_flow(spectrumCard,LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scrollbar_mode(spectrumCard,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(spectrumCard,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* specHead=lv_obj_create(spectrumCard);
        lv_obj_set_size(specHead,lv_pct(100),LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(specHead,0,0); lv_obj_set_style_border_width(specHead,0,0); lv_obj_set_style_pad_all(specHead,0,0);
        lv_obj_set_layout(specHead,LV_LAYOUT_FLEX); lv_obj_set_flex_flow(specHead,LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(specHead,LV_FLEX_ALIGN_SPACE_BETWEEN,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(specHead,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(specHead,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* spectrumTitle=lv_label_create(specHead); lv_label_set_text(spectrumTitle,"MODE SPECTRUM");
        lv_obj_set_style_text_font(spectrumTitle,&lv_font_montserrat_12,0); lv_obj_set_style_text_color(spectrumTitle,COL_HIGHLIGHT,0);
        lv_obj_set_style_text_letter_space(spectrumTitle,2,0);
        lv_obj_t* rndBtn=lv_btn_create(specHead); lv_obj_set_size(rndBtn,scaled(96),scaled(22));
        lv_obj_set_style_bg_color(rndBtn,PLATE_WELL,0); lv_obj_set_style_bg_opa(rndBtn,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(rndBtn,PLATE_EDGE,0); lv_obj_set_style_border_width(rndBtn,1,0);
        lv_obj_set_style_radius(rndBtn,scaled(4),0); lv_obj_set_style_pad_all(rndBtn,0,0);
        lv_obj_set_style_shadow_width(rndBtn,0,0);
        lv_obj_add_event_cb(rndBtn,rndBtnCb,LV_EVENT_CLICKED,this);
        lv_obj_t* rndLbl=lv_label_create(rndBtn); lv_label_set_text(rndLbl,"RANDOMIZE"); lv_obj_center(rndLbl);
        lv_obj_set_style_text_font(rndLbl,&lv_font_montserrat_10,0); lv_obj_set_style_text_color(rndLbl,COL_HIGHLIGHT,0);
        lv_obj_t* chart=lv_chart_create(spectrumCard);
        lv_obj_set_size(chart,lv_pct(100),scaled(290));
        lv_chart_set_type(chart,LV_CHART_TYPE_BAR); lv_chart_set_point_count(chart,16); lv_chart_set_range(chart,LV_CHART_AXIS_PRIMARY_Y,0,1000);
        lv_obj_set_style_bg_color(chart,PLATE_WELL,0); lv_obj_set_style_bg_opa(chart,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(chart,PLATE_LINE,0); lv_obj_set_style_border_width(chart,1,0);
        lv_obj_set_style_radius(chart,scaled(6),0);
        lv_obj_set_style_pad_all(chart,scaled(8),0); lv_obj_set_style_pad_column(chart,scaled(4),0);
        lv_chart_set_div_line_count(chart,4,12);
        lv_obj_set_style_line_color(chart,PLATE_LINE,LV_PART_MAIN); lv_obj_set_style_line_width(chart,1,LV_PART_MAIN); lv_obj_set_style_line_opa(chart,LV_OPA_30,LV_PART_MAIN);
        lv_chart_series_t* series=lv_chart_add_series(chart,COL_HIGHLIGHT,LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_series_color(chart,series,COL_HIGHLIGHT); lv_chart_set_update_mode(chart,LV_CHART_UPDATE_MODE_CIRCULAR);
        lv_obj_add_flag(chart,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(chart,spectrumBandCb,LV_EVENT_PRESSED,this);
        lv_obj_add_event_cb(chart,spectrumBandCb,LV_EVENT_PRESSING,this);
        lv_obj_add_event_cb(chart,spectrumBandCb,LV_EVENT_RELEASED,this);
        fSpectrumChart=chart; for(int i=0;i<16;++i) lv_chart_set_value_by_id(chart,series,i,0);
        // decay scope card - engine output, breathing
        lv_obj_t* scopeCard=lv_obj_create(right);
        lv_obj_set_size(scopeCard,lv_pct(100),LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(scopeCard,PLATE_PANEL,0); lv_obj_set_style_bg_opa(scopeCard,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(scopeCard,PLATE_LINE,0); lv_obj_set_style_border_width(scopeCard,1,0);
        lv_obj_set_style_radius(scopeCard,scaled(6),0);
        lv_obj_set_style_pad_all(scopeCard,scaled(12),0); lv_obj_set_style_pad_row(scopeCard,scaled(8),0);
        lv_obj_set_layout(scopeCard,LV_LAYOUT_FLEX); lv_obj_set_flex_flow(scopeCard,LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scrollbar_mode(scopeCard,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(scopeCard,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* scopeHead=lv_obj_create(scopeCard);
        lv_obj_set_size(scopeHead,lv_pct(100),LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(scopeHead,0,0); lv_obj_set_style_border_width(scopeHead,0,0); lv_obj_set_style_pad_all(scopeHead,0,0);
        lv_obj_set_layout(scopeHead,LV_LAYOUT_FLEX); lv_obj_set_flex_flow(scopeHead,LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(scopeHead,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(scopeHead,scaled(8),0);
        lv_obj_set_scrollbar_mode(scopeHead,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(scopeHead,LV_OBJ_FLAG_SCROLLABLE);
        lfoDot=lv_obj_create(scopeHead);
        lv_obj_set_size(lfoDot,scaled(8),scaled(8));
        lv_obj_set_style_radius(lfoDot,LV_RADIUS_CIRCLE,0);
        lv_obj_set_style_bg_color(lfoDot,COL_HIGHLIGHT,0); lv_obj_set_style_bg_opa(lfoDot,LV_OPA_40,0);
        lv_obj_set_style_border_width(lfoDot,0,0); lv_obj_set_style_pad_all(lfoDot,0,0);
        lv_obj_set_style_shadow_width(lfoDot,scaled(8),0); lv_obj_set_style_shadow_color(lfoDot,COL_HIGHLIGHT,0); lv_obj_set_style_shadow_opa(lfoDot,LV_OPA_40,0);
        lv_obj_clear_flag(lfoDot,LV_OBJ_FLAG_CLICKABLE); lv_obj_clear_flag(lfoDot,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* scopeTitle=lv_label_create(scopeHead); lv_label_set_text(scopeTitle,"DECAY SCOPE");
        lv_obj_set_style_text_font(scopeTitle,&lv_font_montserrat_12,0); lv_obj_set_style_text_color(scopeTitle,PLATE_TEXT,0);
        lv_obj_set_style_text_letter_space(scopeTitle,2,0);
        lv_obj_t* scopeHint=lv_label_create(scopeHead); lv_label_set_text(scopeHint,"LIVE ENGINE OUTPUT");
        lv_obj_set_style_text_font(scopeHint,&lv_font_montserrat_10,0); lv_obj_set_style_text_color(scopeHint,PLATE_TEXT_DIM,0);
        lv_obj_set_style_text_letter_space(scopeHint,1,0);
        lv_obj_t* scope=lv_chart_create(scopeCard);
        lv_obj_set_size(scope,lv_pct(100),scaled(140));
        lv_chart_set_type(scope,LV_CHART_TYPE_LINE); lv_chart_set_point_count(scope,128);
        lv_chart_set_range(scope,LV_CHART_AXIS_PRIMARY_Y,0,1000);
        lv_obj_set_style_bg_color(scope,PLATE_WELL,0); lv_obj_set_style_bg_opa(scope,LV_OPA_COVER,0);
        lv_obj_set_style_border_color(scope,PLATE_LINE,0); lv_obj_set_style_border_width(scope,1,0);
        lv_obj_set_style_radius(scope,scaled(6),0); lv_obj_set_style_pad_all(scope,scaled(6),0);
        lv_chart_set_div_line_count(scope,3,0);
        lv_obj_set_style_line_color(scope,PLATE_LINE,LV_PART_MAIN); lv_obj_set_style_line_width(scope,1,LV_PART_MAIN); lv_obj_set_style_line_opa(scope,LV_OPA_30,LV_PART_MAIN);
        lv_obj_set_style_line_width(scope,scaled(2),LV_PART_ITEMS);
        lv_chart_series_t* ss=lv_chart_add_series(scope,COL_HIGHLIGHT,LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_update_mode(scope,LV_CHART_UPDATE_MODE_CIRCULAR);
        for(int i=0;i<128;++i) lv_chart_set_next_value(scope,ss,0);
        fScopeChart=scope; fScopeSeries=(void*)ss;

        // sync readouts now that all labels exist
        updateBodyInfo();
        updateBodyPreview();
        createKeyboard(root);
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
        }
        lv_chart_refresh(fSpectrumChart);
        // === STRIKE PLATE aliveness ===
        if(fRippleCooldown>0) --fRippleCooldown;
        bool onset=(totalE>fPrevEnergy+std::max(0.02f,fPrevEnergy*1.1f)) && totalE>0.04f;
        fPrevEnergy=std::max(totalE,fPrevEnergy*0.90f);
        if(onset && fRippleCooldown==0 && fGotLiveViz){ spawnRipple(); fRippleCooldown=9; }
        gScopeMax=std::max(std::max(totalE,gScopeMax*0.995f),0.03f);
        fLevelEnv+=(totalE-fLevelEnv)*(totalE>fLevelEnv?0.55f:0.12f);
        float lvl=std::clamp(fLevelEnv/gScopeMax,0.f,1.f);
        if(fScopeChart && fScopeSeries)
            lv_chart_set_next_value(fScopeChart,(lv_chart_series_t*)fScopeSeries,(int32_t)(lvl*980.f));
        if(strikeDisc)
            lv_obj_set_style_border_opa(strikeDisc,(lv_opa_t)(70+185.f*lvl),0);
        if(lfoDot)
            lv_obj_set_style_bg_opa(lfoDot,(lv_opa_t)(40+215.f*lvl),0);
    }
    DGL_NAMESPACE::LVGLTopLevelWidget* fLVGL=nullptr;
    UIStyles styles; ArcVisualSpec spec{};
    lv_obj_t* widgets[PluginMultiScaleBody::kParameterCount]={};
    lv_obj_t* labels[PluginMultiScaleBody::kParameterCount]={};
    float paramCache[PluginMultiScaleBody::kParameterCount]={};
    bool fUIBuilt=false;
    lv_timer_t* fSpectrumTimer=nullptr;
    lv_obj_t* fSpectrumChart=nullptr;
    lv_obj_t* strikeDisc=nullptr;
    lv_obj_t* strikeDot=nullptr;
    lv_obj_t* strikeCoordLabel=nullptr;
    lv_obj_t* presetDropdown=nullptr;
    lv_obj_t* bodySubLabel=nullptr;
    lv_obj_t* bodyPreview=nullptr;
    lv_obj_t* lfoDot=nullptr;
    lv_obj_t* hdrBodyVal=nullptr;
    lv_obj_t* hdrMatVal=nullptr;
    lv_obj_t* hdrModeVal=nullptr;
    lv_obj_t* hdrF0Val=nullptr;
    lv_obj_t* fScopeChart=nullptr;
    void* fScopeSeries=nullptr;
    float fLevelEnv=0.f;
    float fPrevEnergy=0.f;
    float gScopeMax=0.05f;
    int fRippleCooldown=0;
    int fStrikeNote=60;
    bool fStrikeHeld=false;
    bool fMarkerPlaced=false;
    // live metering cache fed by output parameters (bridge-safe DSP->UI link)
    float fVizLevel=0.f;
    float fVizBins[16]={};
    bool fGotLiveViz=false;
    lv_obj_t* fStagePtr=nullptr; lv_obj_t* fLeftPtr=nullptr; lv_obj_t* fCenterPtr=nullptr; lv_obj_t* fRightPtr=nullptr;
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
    bool isFullscreen=false;
    DGL_NAMESPACE::Size<uint> preFsSize{0,0};
};
UI* createUI(){ return new MultiScaleBodyUI(); }
END_NAMESPACE_DISTRHO
