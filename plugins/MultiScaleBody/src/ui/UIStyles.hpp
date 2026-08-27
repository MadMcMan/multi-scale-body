#ifndef UI_STYLES_HPP
#define UI_STYLES_HPP

#include "UICommon.hpp"

START_NAMESPACE_DISTRHO

struct UIStyles {

    lv_style_t arcMain;
    lv_style_t arcIndicator;
    lv_style_t arcKnob;
    lv_style_t btnMain;
    lv_style_t btnHovered;
    lv_style_t btnPressed;
    lv_style_t btnChecked;
    lv_style_t btnCheckedPressed;
    lv_style_t labelSmall;
    lv_style_t knobTitleSmall;
    lv_style_t compactValueLabel;
    lv_style_t compactSelectMain;
    lv_style_t compactSelectListMain;
    lv_style_t meterTrack;

    lv_style_t knobCap;
    lv_style_t knobRim;
    lv_style_t knobFace;
    lv_style_t knobShading;
    lv_style_t knobNeedle;


    // Release style property allocations before re-init (uiReshape re-runs
    // init() on already-initialized styles; lv_style_init alone would leak).
    void reset() {
        lv_style_reset(&arcMain);
        lv_style_reset(&arcIndicator);
        lv_style_reset(&arcKnob);
        lv_style_reset(&btnMain);
        lv_style_reset(&btnHovered);
        lv_style_reset(&btnPressed);
        lv_style_reset(&btnChecked);
        lv_style_reset(&btnCheckedPressed);
        lv_style_reset(&labelSmall);
        lv_style_reset(&knobTitleSmall);
        lv_style_reset(&compactValueLabel);
        lv_style_reset(&compactSelectMain);
        lv_style_reset(&compactSelectListMain);
        lv_style_reset(&meterTrack);
        lv_style_reset(&knobCap);
        lv_style_reset(&knobRim);
        lv_style_reset(&knobFace);
        lv_style_reset(&knobShading);
        lv_style_reset(&knobNeedle);
    }

    void init() {

        // Level meter track: inset well, hairline border; indicator color is
        // set live per zone (safe/amber/hot) so it stays a dynamic state color
        lv_style_init(&meterTrack);
        lv_style_set_bg_color(&meterTrack, PLATE_WELL);
        lv_style_set_bg_opa(&meterTrack, LV_OPA_COVER);
        lv_style_set_border_color(&meterTrack, PLATE_LINE);
        lv_style_set_border_width(&meterTrack, 1);
        lv_style_set_radius(&meterTrack, scaled(3));
        lv_style_set_pad_all(&meterTrack, 0);


        // Arc styles (Knob Ring): thin, dark track; subtle indicator, no glow
        lv_style_init(&arcMain);
        lv_style_set_arc_color(&arcMain, COL_KNOB_RING_BG);
        lv_style_set_arc_width(&arcMain, scaled(2));
        lv_style_set_radius(&arcMain, LV_RADIUS_CIRCLE);
        lv_style_set_arc_rounded(&arcMain, true);

        lv_style_init(&arcIndicator);
        lv_style_set_arc_color(&arcIndicator, COL_KNOB_INDICATOR);
        lv_style_set_arc_width(&arcIndicator, scaled(2));
        lv_style_set_radius(&arcIndicator, LV_RADIUS_CIRCLE);
        lv_style_set_arc_rounded(&arcIndicator, true);
        lv_style_set_shadow_width(&arcIndicator, scaled(4));
        lv_style_set_shadow_color(&arcIndicator, COL_KNOB_INDICATOR);
        lv_style_set_shadow_opa(&arcIndicator, LV_OPA_10);

        // Knob Cap style: dark metallic, hairline rim, minimal shadow
        // piece-2: knob cap carries a 1px darker hairline border (COL_BORDER at
        // 40% alpha) - the "machined bezel" reads as an inset ring on the cap
        // circumference. The amber arc indicator is the PRIMARY value cue; the
        // bezel is decoration, so it stays low-OPA so the arc wins the eye.
        lv_style_init(&knobCap);
        lv_style_set_bg_color(&knobCap, COL_KNOB_CAP);
        lv_style_set_bg_opa(&knobCap, LV_OPA_COVER);
        lv_style_set_radius(&knobCap, LV_RADIUS_CIRCLE);
        lv_style_set_border_color(&knobCap, COL_BORDER);
        lv_style_set_border_width(&knobCap, 1);
        lv_style_set_border_opa(&knobCap, LV_OPA_40);
        lv_style_set_shadow_width(&knobCap, scaled(2));
        lv_style_set_shadow_color(&knobCap, COL_BLACK);
        lv_style_set_shadow_opa(&knobCap, LV_OPA_20);
        lv_style_set_shadow_offset_y(&knobCap, scaled(lay::SHADOW_OFF_Y));

        lv_style_init(&knobRim);
        lv_style_set_radius(&knobRim, LV_RADIUS_CIRCLE);
        lv_style_set_border_color(&knobRim, COL_WHITE);
        lv_style_set_border_width(&knobRim, 1);
        lv_style_set_border_side(&knobRim, static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT));
        lv_style_set_border_opa(&knobRim, LV_OPA_20);
        lv_style_set_bg_opa(&knobRim, LV_OPA_TRANSP);

        lv_style_init(&knobFace);
        lv_style_set_bg_opa(&knobFace, LV_OPA_TRANSP);
        lv_style_set_border_width(&knobFace, 0);
        lv_style_set_radius(&knobFace, LV_RADIUS_CIRCLE);
        lv_style_set_pad_all(&knobFace, 0);

        lv_style_init(&knobShading);
        lv_style_set_radius(&knobShading, LV_RADIUS_CIRCLE);
        lv_style_set_bg_opa(&knobShading, LV_OPA_COVER);
        lv_style_set_bg_color(&knobShading, COL_KNOB_CAP);
        lv_style_set_bg_grad_color(&knobShading, COL_BLACK);
        lv_style_set_bg_grad_dir(&knobShading, LV_GRAD_DIR_VER);
        lv_style_set_bg_main_stop(&knobShading, 0);
        lv_style_set_bg_grad_stop(&knobShading, 255);
        lv_style_set_bg_main_opa(&knobShading, LV_OPA_10);
        lv_style_set_bg_grad_opa(&knobShading, LV_OPA_50);
        lv_style_set_border_width(&knobShading, 0);

        lv_style_init(&knobNeedle);
        lv_style_set_bg_color(&knobNeedle, COL_WHITE);
        lv_style_set_bg_opa(&knobNeedle, LV_OPA_COVER);
        lv_style_set_radius(&knobNeedle, 1);
        lv_style_set_border_width(&knobNeedle, 0);
        // piece-2: value tick at the indicator's tip. The arc's KNOB part is
        // placed at the value angle; we style it as a 1px wide x 6px tall white
        // tick (the size is derived from pad_left/right/top/bottom of the KNOB
        // part, see lv_arc.c:812-816). Amber arc is still the primary value
        // cue; this is the small "where am I right now" dot at the sweep tip.
        lv_style_init(&arcKnob);
        lv_style_set_bg_color(&arcKnob, COL_WHITE);
        lv_style_set_bg_opa(&arcKnob, LV_OPA_COVER);
        lv_style_set_radius(&arcKnob, 1);
        lv_style_set_border_width(&arcKnob, 0);
        // knob size = 1 wide (pad_left=0, pad_right=0) x 6 tall (pad_top=2, pad_bottom=2)
        lv_style_set_pad_left(&arcKnob, 0);
        lv_style_set_pad_right(&arcKnob, 0);
        lv_style_set_pad_top(&arcKnob, 2);
        lv_style_set_pad_bottom(&arcKnob, 2);
        lv_style_set_shadow_width(&arcKnob, 0);
        lv_style_set_shadow_color(&arcKnob, COL_WHITE);
        lv_style_set_shadow_opa(&arcKnob, LV_OPA_30);
        lv_style_set_shadow_color(&knobNeedle, COL_WHITE);
        lv_style_set_shadow_opa(&knobNeedle, LV_OPA_0);





        // Transitions
        static const lv_style_prop_t btn_trans_props[] = {LV_STYLE_BG_COLOR, LV_STYLE_TRANSFORM_WIDTH, LV_STYLE_TRANSFORM_HEIGHT, (lv_style_prop_t)0};
        static lv_style_transition_dsc_t btn_trans_def;
        lv_style_transition_dsc_init(&btn_trans_def, btn_trans_props, lv_anim_path_ease_out, 200, 0, NULL);

        // Pressed style (Generic)
        lv_style_init(&btnPressed);
        lv_style_set_translate_y(&btnPressed, 1);
        lv_style_set_img_recolor_opa(&btnPressed, LV_OPA_20);
        lv_style_set_img_recolor(&btnPressed, COL_BLACK);
        lv_style_set_transition(&btnPressed, &btn_trans_def);

        // Universal Premium Base (Dark Metallic)
        lv_style_init(&btnMain);
        lv_style_set_radius(&btnMain, scaled(3));
        lv_style_set_bg_opa(&btnMain, LV_OPA_COVER);
        lv_style_set_bg_color(&btnMain, COL_ACCENT);
        lv_style_set_bg_grad_color(&btnMain, COL_PANEL_DARK);
        lv_style_set_bg_grad_dir(&btnMain, LV_GRAD_DIR_VER);
        lv_style_set_border_color(&btnMain, COL_BORDER);
        lv_style_set_border_width(&btnMain, 1);
        lv_style_set_border_opa(&btnMain, LV_OPA_60);
        lv_style_set_text_color(&btnMain, COL_TEXT_DIM);
        lv_style_set_text_font(&btnMain, getScaledSmallFont());
        lv_style_set_pad_hor(&btnMain, scaled(12));
        lv_style_set_pad_ver(&btnMain, scaled(6));
        lv_style_set_transition(&btnMain, &btn_trans_def);

        // Hover lift: subtle brightness step on chrome buttons
        lv_style_init(&btnHovered);
        lv_style_set_bg_color(&btnHovered, COL_BTN_HOVER);
        lv_style_set_text_color(&btnHovered, COL_TEXT);
        lv_style_set_transition(&btnHovered, &btn_trans_def);

        // Universal Premium Active (Glowing Cyan)
        lv_style_init(&btnChecked);
        lv_style_set_bg_color(&btnChecked, COL_HIGHLIGHT);
        lv_style_set_bg_grad_color(&btnChecked, COL_CHECKED_BG);
        lv_style_set_bg_grad_dir(&btnChecked, LV_GRAD_DIR_VER);
        lv_style_set_text_color(&btnChecked, COL_BG);
        lv_style_set_border_color(&btnChecked, COL_WHITE);
        lv_style_set_border_opa(&btnChecked, LV_OPA_40);
        lv_style_set_shadow_width(&btnChecked, scaled(12));
        lv_style_set_shadow_color(&btnChecked, COL_HIGHLIGHT);
        lv_style_set_shadow_opa(&btnChecked, LV_OPA_40);

        // Checked + Pressed
        lv_style_init(&btnCheckedPressed);
        lv_style_set_bg_color(&btnCheckedPressed, COL_CHECKED_BG);
        lv_style_set_translate_y(&btnCheckedPressed, 1);
        
        // Small Label style
        lv_style_init(&labelSmall);
        lv_style_set_text_color(&labelSmall, COL_TEXT);
        lv_style_set_text_font(&labelSmall, getScaledSmallFont());

        // Shared compact knob title style: quiet gray labels
        lv_style_init(&knobTitleSmall);
        lv_style_set_text_color(&knobTitleSmall, COL_KNOB_LABEL);
        lv_style_set_text_font(&knobTitleSmall, getScaledSmallFont());

        // piece-5: value label text color -> PLATE_AMBER_DIM (desaturated amber).
        // One-accent discipline: the only full-amber (COL_HIGHLIGHT / PLATE_AMBER)
        // marks in the chrome are the indicator arc + the mallet. Value labels
        // were previously full amber (competing with the arc); the dim variant
        // keeps the read order: arc wins, label is supporting evidence.
        lv_style_init(&compactValueLabel);
        lv_style_set_text_color(&compactValueLabel, PLATE_AMBER_DIM);
        lv_style_set_text_font(&compactValueLabel, getScaledSmallFont());
        lv_style_set_text_align(&compactValueLabel, LV_TEXT_ALIGN_CENTER);

        // Shared compact dropdown button style for EQ header/settings controls.
        lv_style_init(&compactSelectMain);
        lv_style_set_bg_color(&compactSelectMain, COL_PANEL_DARK);
        lv_style_set_bg_opa(&compactSelectMain, LV_OPA_COVER);
        lv_style_set_border_width(&compactSelectMain, 1);
        lv_style_set_border_color(&compactSelectMain, COL_BORDER);
        lv_style_set_radius(&compactSelectMain, scaled(4));
        lv_style_set_text_color(&compactSelectMain, COL_TEXT);
        lv_style_set_text_font(&compactSelectMain, getScaledSmallFont());
        lv_style_set_text_align(&compactSelectMain, LV_TEXT_ALIGN_LEFT);
        lv_style_set_pad_hor(&compactSelectMain, scaled(8));
        lv_style_set_pad_ver(&compactSelectMain, scaled(4));


        lv_style_init(&compactSelectListMain);
        lv_style_set_bg_color(&compactSelectListMain, COL_PANEL_DARK);
        lv_style_set_bg_opa(&compactSelectListMain, LV_OPA_COVER);
        lv_style_set_border_width(&compactSelectListMain, 1);
        lv_style_set_border_color(&compactSelectListMain, COL_BORDER);
        lv_style_set_radius(&compactSelectListMain, scaled(4));
        lv_style_set_text_color(&compactSelectListMain, COL_TEXT);
        lv_style_set_text_font(&compactSelectListMain, getScaledSmallFont());
        lv_style_set_text_align(&compactSelectListMain, LV_TEXT_ALIGN_LEFT);
        lv_style_set_pad_hor(&compactSelectListMain, scaled(6));
        lv_style_set_pad_ver(&compactSelectListMain, scaled(4));

    }

    void applyToggleButton(lv_obj_t* btn, bool isOn) const {
        lv_obj_add_style(btn, &btnMain, 0);
        lv_obj_add_style(btn, &btnHovered, LV_STATE_HOVERED);
        lv_obj_add_style(btn, &btnPressed, LV_STATE_PRESSED);
        lv_obj_add_style(btn, &btnChecked, LV_STATE_CHECKED);
        lv_obj_add_style(btn, &btnCheckedPressed, LV_STATE_CHECKED | LV_STATE_PRESSED);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        if (isOn) lv_obj_add_state(btn, LV_STATE_CHECKED);
        else      lv_obj_clear_state(btn, LV_STATE_CHECKED);
    }

};

END_NAMESPACE_DISTRHO

#endif // UI_STYLES_HPP
