#ifndef UI_WIDGETS_HPP
#define UI_WIDGETS_HPP
#include "UIStyles.hpp"
#include "lvgl.h"
#include <unordered_map>
#include <cstdio>
#include <algorithm>
#include "UICommon.hpp"
START_NAMESPACE_DISTRHO

// === Geometry (kept for PluginUI compatibility) ===
struct ArcVisualSpec {
    lv_coord_t containerW=0; lv_coord_t containerH=0; lv_coord_t arcSize=0;
    lv_coord_t labelMarginBottom=1; lv_coord_t valueMarginTop=1;
    int capInset=12; int needleTopOffset=3; int needleBottomInset=4;
};
inline ArcVisualSpec normalArcSpec() noexcept {
    ArcVisualSpec spec{}; spec.containerW=scaled(92); spec.containerH=scaled(116); spec.arcSize=scaled(76); return spec;
}

namespace {
struct ArcDragState{ lv_point_t startPoint{0,0}; int startValue=0; bool active=false; };
struct ArcVisualBinding{ lv_obj_t* face=nullptr; lv_obj_t* valueLabel=nullptr; };
static std::unordered_map<lv_obj_t*, ArcDragState> gArcDragStates;
static std::unordered_map<lv_obj_t*, ArcVisualBinding> gArcVisualBindings;
static std::unordered_map<lv_obj_t*, int> gArcParamIndex;

// Exact cymbals formula: 2250 = 225° => arc 135° + face pivot offset (needle up = north 270°, so 135-270 = -135 = 225°)
static int sharedArcAngleFromValue(int v){ int c=std::clamp(v,0,1000); return (2250 + (c*2700/1000))%3600; }
static void syncSharedArcVisual(lv_obj_t* arc){
    if(!arc) return;
    auto it=gArcVisualBindings.find(arc); if(it==gArcVisualBindings.end()) return;
    if(it->second.face){ lv_obj_set_style_transform_angle(it->second.face, sharedArcAngleFromValue(lv_arc_get_value(arc)),0); lv_obj_invalidate(it->second.face); }
}
static void sharedArcVisualEventCb(lv_event_t* e){
    lv_obj_t* arc=(lv_obj_t*)lv_event_get_target(e); auto code=lv_event_get_code(e);
    if(code==LV_EVENT_VALUE_CHANGED){
        syncSharedArcVisual(arc);
        auto it=gArcVisualBindings.find(arc);
        if(it!=gArcVisualBindings.end() && it->second.valueLabel){ char buf[32]; snprintf(buf,sizeof(buf),"%.2f",lv_arc_get_value(arc)/1000.f); lv_label_set_text(it->second.valueLabel,buf); }
        return;
    }
    if(code==LV_EVENT_DELETE){ gArcVisualBindings.erase(arc); gArcDragStates.erase(arc); gArcParamIndex.erase(arc); }
}
static void allowChildOverflow(lv_obj_t* obj){
    lv_obj_add_flag(obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(obj, [](lv_event_t* e){
        if(lv_event_get_code(e)==LV_EVENT_REFR_EXT_DRAW_SIZE){
            int32_t* ext=(int32_t*)lv_event_get_param(e);
            *ext+=scaled(14);
            lv_event_stop_bubbling(e);
        }
    }, LV_EVENT_ALL, nullptr);
}
static void arcDragCb(lv_event_t* e){
    lv_obj_t* arc=(lv_obj_t*)lv_event_get_target(e); auto code=lv_event_get_code(e);
    if(code==LV_EVENT_DELETE){ gArcDragStates.erase(arc); return; }
    lv_indev_t* indev=lv_indev_get_act(); if(!indev) return;
    if(code==LV_EVENT_PRESSED){
        lv_point_t p; lv_indev_get_point(indev,&p);
        ArcDragState& st=gArcDragStates[arc]; st.startPoint=p; st.startValue=lv_arc_get_value(arc);
        AbstractMultiScaleBodyUI* ui=(AbstractMultiScaleBodyUI*)lv_event_get_user_data(e);
        if(ui && !st.active){
            int paramIndex=(int)(intptr_t)lv_obj_get_user_data(arc);
            ui->editParameter((uint32_t)paramIndex,true);
        }
        st.active=true; lv_event_stop_bubbling(e); return;
    }
    if(code==LV_EVENT_PRESSING){
        auto it=gArcDragStates.find(arc); if(it==gArcDragStates.end()||!it->second.active) return;
        lv_point_t p; lv_indev_get_point(indev,&p);
        float travel=(float)(p.x - it->second.startPoint.x) - (float)(p.y - it->second.startPoint.y);
        int next=std::clamp((int)std::lround((float)it->second.startValue + travel*3.5f),0,1000);
        if(next!=lv_arc_get_value(arc)){
            lv_arc_set_value(arc,next);
            // Direct host update — shared handler only syncs visuals, not params (its user_data is nullptr)
            AbstractMultiScaleBodyUI* ui=(AbstractMultiScaleBodyUI*)lv_event_get_user_data(e);
            if(ui){
                int paramIndex=(int)(intptr_t)lv_obj_get_user_data(arc);
                ui->setParamValue((uint32_t)paramIndex, next/1000.f);
            }
            syncSharedArcVisual(arc);
            auto it2=gArcVisualBindings.find(arc);
            if(it2!=gArcVisualBindings.end() && it2->second.valueLabel){ char buf[32]; snprintf(buf,sizeof(buf),"%.2f",next/1000.f); lv_label_set_text(it2->second.valueLabel,buf); }
        }
        lv_event_stop_bubbling(e); return;
    }
    if(code==LV_EVENT_RELEASED){
        auto it=gArcDragStates.find(arc); if(it!=gArcDragStates.end()) it->second.active=false;
        AbstractMultiScaleBodyUI* ui=(AbstractMultiScaleBodyUI*)lv_event_get_user_data(e);
        if(ui){
            int paramIndex=(int)(intptr_t)lv_obj_get_user_data(arc);
            ui->editParameter((uint32_t)paramIndex,false);
        }
    }
}
} // anonymous namespace

struct UIWidgets {
    static lv_obj_t* createArcKnob(lv_obj_t* parent, uint32_t paramIndex, AbstractMultiScaleBodyUI* ui, UIStyles& styles, const ArcVisualSpec& spec){
        // Container
        lv_obj_t* cont=lv_obj_create(parent);
        lv_obj_set_size(cont,spec.containerW,spec.containerH);
        lv_obj_set_style_bg_opa(cont,0,0); lv_obj_set_style_border_width(cont,0,0);
        lv_obj_set_style_pad_row(cont,2,0); lv_obj_set_layout(cont,LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(cont,LV_FLEX_FLOW_COLUMN); lv_obj_set_flex_align(cont,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(cont,LV_SCROLLBAR_MODE_OFF); lv_obj_clear_flag(cont,LV_OBJ_FLAG_SCROLLABLE);
        allowChildOverflow(cont);
        // Title
        lv_obj_t* titleLbl=lv_label_create(cont);
        lv_label_set_text(titleLbl, ui->parameterName(paramIndex).c_str());
        lv_obj_add_style(titleLbl,&styles.knobTitleSmall,0);
        lv_obj_set_style_text_align(titleLbl,LV_TEXT_ALIGN_CENTER,0);
        lv_obj_set_style_margin_bottom(titleLbl,spec.labelMarginBottom,0);
        lv_obj_set_width(titleLbl,spec.containerW);
        // Arc — exact cymbals geometry: 135° rotation, 270° sweep, 0-1000
        lv_obj_t* arc=lv_arc_create(cont);
        lv_obj_set_size(arc,spec.arcSize,spec.arcSize);
        lv_arc_set_rotation(arc,135); lv_arc_set_bg_angles(arc,0,270);
        lv_arc_set_range(arc,0,1000); lv_arc_set_value(arc,(int)(ui->getParamValue(paramIndex)*1000.f));
        lv_obj_add_style(arc,&styles.arcMain,LV_PART_MAIN);
        lv_obj_add_style(arc,&styles.arcIndicator,LV_PART_INDICATOR);
        lv_obj_add_style(arc,&styles.arcKnob,LV_PART_KNOB);
        lv_obj_clear_flag(arc,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_anim_duration(arc,200,0);
        lv_obj_set_user_data(arc,(void*)(intptr_t)paramIndex);
        gArcParamIndex[arc]=paramIndex;
        lv_obj_add_event_cb(arc,arcDragCb,LV_EVENT_ALL,ui);
        lv_obj_add_event_cb(arc,sharedArcVisualEventCb,LV_EVENT_VALUE_CHANGED,nullptr);
        lv_obj_add_event_cb(arc,sharedArcVisualEventCb,LV_EVENT_DELETE,nullptr);
        // Cap / rim / ticks / face / needle — exact cymbals sizes, cap is CHILD OF ARC so it overlays correctly (flex would stack if child of cont)
        const int capSize=std::max(spec.arcSize-spec.capInset,8);
        lv_obj_t* cap=lv_obj_create(arc);
        lv_obj_set_size(cap,capSize,capSize);
        lv_obj_center(cap);
        lv_obj_add_style(cap,&styles.knobCap,0); lv_obj_clear_flag(cap,LV_OBJ_FLAG_CLICKABLE); lv_obj_clear_flag(cap,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* rim=lv_obj_create(cap); lv_obj_set_size(rim,capSize,capSize); lv_obj_center(rim);
        lv_obj_add_style(rim,&styles.knobRim,0); lv_obj_clear_flag(rim,LV_OBJ_FLAG_CLICKABLE);
        if(capSize>=28){
            int tickCount=(capSize>=40)?12:8; int tickLen=std::max(2,capSize/12);
            for(int t=0;t<tickCount;++t){ lv_obj_t* tick=lv_obj_create(cap); lv_obj_set_size(tick,1,tickLen);
                lv_obj_set_style_bg_color(tick,COL_BORDER,0); lv_obj_set_style_bg_opa(tick,LV_OPA_80,0);
                lv_obj_set_style_border_width(tick,0,0); lv_obj_set_style_radius(tick,0,0);
                lv_obj_set_pos(tick,capSize/2-1,1);
                lv_obj_set_style_transform_pivot_x(tick,1,0); lv_obj_set_style_transform_pivot_y(tick,capSize/2-1,0);
                lv_obj_set_style_transform_angle(tick,t*(3600/tickCount),0); lv_obj_clear_flag(tick,LV_OBJ_FLAG_CLICKABLE);
            }
        }
        lv_obj_t* face=lv_obj_create(cap); lv_obj_set_size(face,capSize,capSize); lv_obj_center(face);
        lv_obj_add_style(face,&styles.knobFace,0); lv_obj_clear_flag(face,LV_OBJ_FLAG_CLICKABLE); lv_obj_clear_flag(face,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* shading=lv_obj_create(face); lv_obj_set_size(shading,lv_pct(100),lv_pct(100)); lv_obj_center(shading);
        lv_obj_add_style(shading,&styles.knobShading,0); lv_obj_clear_flag(shading,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* needle=lv_obj_create(face); lv_obj_add_style(needle,&styles.knobNeedle,0);
        int needleW=gUIScale>=1.5f?2:1;
        lv_obj_set_size(needle,needleW,std::max(2,capSize/2-spec.needleBottomInset));
        lv_obj_align(needle,LV_ALIGN_TOP_MID,0,spec.needleTopOffset); lv_obj_clear_flag(needle,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_transform_pivot_x(face,capSize/2,0); lv_obj_set_style_transform_pivot_y(face,capSize/2,0);
        lv_obj_t* valueLbl=lv_label_create(cont); char buf[32]; snprintf(buf,sizeof(buf),"%.2f",ui->getParamValue(paramIndex));
        lv_label_set_text(valueLbl,buf); lv_obj_add_style(valueLbl,&styles.compactValueLabel,0);
        lv_obj_set_style_margin_top(valueLbl,spec.valueMarginTop,0); lv_obj_set_width(valueLbl,spec.containerW);
        gArcVisualBindings[arc]={face,valueLbl}; syncSharedArcVisual(arc);
        return arc;
    }
    static void syncFromParam(lv_obj_t* arc,float v){
        if(!arc) return;
        lv_obj_remove_event_cb(arc,sharedArcVisualEventCb);
        lv_arc_set_value(arc,(int)(std::clamp(v,0.f,1.f)*1000.f)); syncSharedArcVisual(arc);
        lv_obj_add_event_cb(arc,sharedArcVisualEventCb,LV_EVENT_VALUE_CHANGED,nullptr);
        auto it=gArcVisualBindings.find(arc);
        if(it!=gArcVisualBindings.end()&&it->second.valueLabel){ char buf[32]; snprintf(buf,sizeof(buf),"%.2f",v); lv_label_set_text(it->second.valueLabel,buf); }
        // also sync the hidden modRing if present? not needed for this synth
    }
    static void updateLabel(lv_obj_t* label,float v){ if(label){ char buf[32]; snprintf(buf,sizeof(buf),"%.2f",v); lv_label_set_text(label,buf); } }
};
END_NAMESPACE_DISTRHO
#endif
