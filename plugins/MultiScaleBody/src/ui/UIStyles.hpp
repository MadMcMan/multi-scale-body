#ifndef UI_STYLES_HPP
#define UI_STYLES_HPP

#include "UICommon.hpp"

START_NAMESPACE_DISTRHO

struct UIStyles {
    struct KnobVisualGeometry {
        int capInset = 12;
        int needleTopOffset = 3;
        int needleBottomInset = 4;
    };

    lv_style_t panel;
    lv_style_t sliderMain;
    lv_style_t sliderIndicator;
    lv_style_t sliderKnob;
    lv_style_t label;
    lv_style_t title;
    lv_style_t arcMain;
    lv_style_t arcIndicator;
    lv_style_t arcKnob;
    lv_style_t tabBtn;
    lv_style_t tabSelected;
    lv_style_t tabViewBg;
    lv_style_t chartMain;
    lv_style_t chartSeries;
    lv_style_t chartBand;
    lv_style_t btnMain;
    lv_style_t btnPressed;
    lv_style_t btnChecked;
    lv_style_t btnCheckedPressed;
    lv_style_t labelSmall;
    lv_style_t knobTitleSmall;
    lv_style_t compactValueLabel;
    lv_style_t compactSelectMain;
    lv_style_t compactSelectIndicator;
    lv_style_t compactSelectListMain;
    lv_style_t compactSelectListSelected;
    lv_style_t card;

    lv_style_t knobCap;
    lv_style_t knobRim;
    lv_style_t knobFace;
    lv_style_t knobShading;
    lv_style_t knobNeedle;

    static constexpr KnobVisualGeometry normalKnobGeometry() noexcept {
        return KnobVisualGeometry{};
    }

    static constexpr KnobVisualGeometry compactKnobGeometry() noexcept {
        KnobVisualGeometry spec{};
        spec.capInset = 6;
        spec.needleTopOffset = 2;
        spec.needleBottomInset = 3;
        return spec;
    }

    // Release style property allocations before re-init (uiReshape re-runs
    // init() on already-initialized styles; lv_style_init alone would leak).
    void reset() {
        lv_style_reset(&panel);
        lv_style_reset(&sliderMain);
        lv_style_reset(&sliderIndicator);
        lv_style_reset(&sliderKnob);
        lv_style_reset(&label);
        lv_style_reset(&title);
        lv_style_reset(&arcMain);
        lv_style_reset(&arcIndicator);
        lv_style_reset(&arcKnob);
        lv_style_reset(&tabBtn);
        lv_style_reset(&tabSelected);
        lv_style_reset(&tabViewBg);
        lv_style_reset(&chartMain);
        lv_style_reset(&chartSeries);
        lv_style_reset(&chartBand);
        lv_style_reset(&btnMain);
        lv_style_reset(&btnPressed);
        lv_style_reset(&btnChecked);
        lv_style_reset(&btnCheckedPressed);
        lv_style_reset(&labelSmall);
        lv_style_reset(&knobTitleSmall);
        lv_style_reset(&compactValueLabel);
        lv_style_reset(&compactSelectMain);
        lv_style_reset(&compactSelectIndicator);
        lv_style_reset(&compactSelectListMain);
        lv_style_reset(&compactSelectListSelected);
        lv_style_reset(&card);
        lv_style_reset(&knobCap);
        lv_style_reset(&knobRim);
        lv_style_reset(&knobFace);
        lv_style_reset(&knobShading);
        lv_style_reset(&knobNeedle);
    }

    void init() {
        // Panel style: machined plate on the chassis - clearly lighter surface,
        // hairline border, soft drop shadow.
        lv_style_init(&panel);
        lv_style_set_bg_color(&panel, COL_PANEL);
        lv_style_set_bg_grad_color(&panel, lv_color_hex(0x131519));
        lv_style_set_bg_grad_dir(&panel, LV_GRAD_DIR_VER);
        lv_style_set_bg_opa(&panel, LV_OPA_COVER);
        lv_style_set_border_color(&panel, COL_HAIRLINE);
        lv_style_set_border_width(&panel, 1);
        lv_style_set_radius(&panel, scaled(6));
        lv_style_set_pad_all(&panel, scaled(10));
        lv_style_set_layout(&panel, LV_LAYOUT_FLEX);
        lv_style_set_flex_flow(&panel, LV_FLEX_FLOW_COLUMN);
        lv_style_set_pad_row(&panel, scaled(8));
        lv_style_set_shadow_width(&panel, scaled(12));
        lv_style_set_shadow_color(&panel, COL_BLACK);
        lv_style_set_shadow_opa(&panel, LV_OPA_20);

        // Card style: darker inset well inside a plate
        lv_style_init(&card);
        lv_style_set_bg_color(&card, COL_PANEL_DARK);
        lv_style_set_bg_opa(&card, LV_OPA_COVER);
        lv_style_set_border_color(&card, COL_ACCENT);
        lv_style_set_border_width(&card, 1);
        lv_style_set_radius(&card, scaled(6));
        lv_style_set_pad_all(&card, scaled(8));
        lv_style_set_pad_row(&card, scaled(6));
        lv_style_set_layout(&card, LV_LAYOUT_FLEX);
        lv_style_set_flex_flow(&card, LV_FLEX_FLOW_COLUMN);

        // Slider Main (Track)
        lv_style_init(&sliderMain);
        lv_style_set_bg_color(&sliderMain, COL_ACCENT);
        lv_style_set_radius(&sliderMain, scaled(6));
        lv_style_set_height(&sliderMain, scaled(6));

        // Slider Indicator (Filled part)
        lv_style_init(&sliderIndicator);
        lv_style_set_bg_color(&sliderIndicator, COL_SLIDER);
        lv_style_set_radius(&sliderIndicator, scaled(6));

        // Slider Knob
        lv_style_init(&sliderKnob);
        lv_style_set_bg_color(&sliderKnob, COL_WHITE);
        lv_style_set_radius(&sliderKnob, LV_RADIUS_CIRCLE);
        lv_style_set_pad_all(&sliderKnob, scaled(6));
        lv_style_set_shadow_width(&sliderKnob, scaled(10));
        lv_style_set_shadow_color(&sliderKnob, COL_BLACK);
        lv_style_set_shadow_opa(&sliderKnob, LV_OPA_40);

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
        lv_style_init(&knobCap);
        lv_style_set_bg_color(&knobCap, COL_KNOB_CAP);
        lv_style_set_bg_opa(&knobCap, LV_OPA_COVER);
        lv_style_set_radius(&knobCap, LV_RADIUS_CIRCLE);
        lv_style_set_border_color(&knobCap, COL_BORDER);
        lv_style_set_border_width(&knobCap, 1);
        lv_style_set_shadow_width(&knobCap, scaled(2));
        lv_style_set_shadow_color(&knobCap, COL_BLACK);
        lv_style_set_shadow_opa(&knobCap, LV_OPA_20);

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
        lv_style_set_shadow_width(&knobNeedle, 0);
        lv_style_set_shadow_color(&knobNeedle, COL_WHITE);
        lv_style_set_shadow_opa(&knobNeedle, LV_OPA_0);

        lv_style_init(&arcKnob);
        lv_style_set_bg_opa(&arcKnob, LV_OPA_TRANSP);
        lv_style_set_border_width(&arcKnob, 0);

        // Label style
        lv_style_init(&label);
        lv_style_set_text_color(&label, COL_TEXT);
        lv_style_set_text_font(&label, getScaledFont());

        // Section Title style: soft gray caps, accent reserved for values
        lv_style_init(&title);
        lv_style_set_text_color(&title, COL_TITLE);
        lv_style_set_text_font(&title, getScaledFont());

        // Tab buttons
        lv_style_init(&tabBtn);
        lv_style_set_bg_color(&tabBtn, COL_PANEL);
        lv_style_set_bg_opa(&tabBtn, LV_OPA_COVER);
        lv_style_set_text_color(&tabBtn, COL_TEXT_DIM);
        lv_style_set_border_width(&tabBtn, 0);
        lv_style_set_radius(&tabBtn, 0);

        lv_style_init(&tabSelected);
        lv_style_set_bg_color(&tabSelected, COL_BG);
        lv_style_set_text_color(&tabSelected, COL_HIGHLIGHT);
        lv_style_set_border_side(&tabSelected, LV_BORDER_SIDE_BOTTOM);
        lv_style_set_border_color(&tabSelected, COL_HIGHLIGHT);
        lv_style_set_border_width(&tabSelected, 3);
        lv_style_set_shadow_width(&tabSelected, scaled(10));
        lv_style_set_shadow_color(&tabSelected, COL_HIGHLIGHT);
        lv_style_set_shadow_opa(&tabSelected, LV_OPA_30);

        lv_style_init(&tabViewBg);
        lv_style_set_bg_color(&tabViewBg, COL_BG);

        // Chart styles
        lv_style_init(&chartMain);
        lv_style_set_bg_color(&chartMain, COL_PANEL_DARK);
        lv_style_set_bg_opa(&chartMain, LV_OPA_COVER);
        lv_style_set_border_width(&chartMain, 1);
        lv_style_set_border_color(&chartMain, COL_ACCENT);
        lv_style_set_radius(&chartMain, scaled(6));

        lv_style_init(&chartSeries);
        lv_style_set_line_width(&chartSeries, scaled(3));
        lv_style_set_line_color(&chartSeries, COL_HIGHLIGHT);
        lv_style_set_bg_color(&chartSeries, COL_HIGHLIGHT); 
        lv_style_set_bg_grad_color(&chartSeries, COL_BG); 
        lv_style_set_bg_grad_dir(&chartSeries, LV_GRAD_DIR_VER);
        lv_style_set_bg_opa(&chartSeries, LV_OPA_40); 

        lv_style_init(&chartBand);
        lv_style_set_line_width(&chartBand, 1);
        lv_style_set_bg_opa(&chartBand, LV_OPA_10);

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

        // Shared compact value label style
        lv_style_init(&compactValueLabel);
        lv_style_set_text_color(&compactValueLabel, COL_TEXT);
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

        lv_style_init(&compactSelectIndicator);
        lv_style_set_text_color(&compactSelectIndicator, COL_TEXT_DIM);
        lv_style_set_text_font(&compactSelectIndicator, getScaledSmallFont());

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

        lv_style_init(&compactSelectListSelected);
        lv_style_set_bg_color(&compactSelectListSelected, COL_ACCENT);
        lv_style_set_bg_opa(&compactSelectListSelected, LV_OPA_COVER);
        lv_style_set_text_color(&compactSelectListSelected, COL_TEXT);
        lv_style_set_text_font(&compactSelectListSelected, getScaledSmallFont());
    }

    void applyToggleButton(lv_obj_t* btn, bool isOn) const {
        lv_obj_add_style(btn, &btnMain, 0);
        lv_obj_add_style(btn, &btnPressed, LV_STATE_PRESSED);
        lv_obj_add_style(btn, &btnChecked, LV_STATE_CHECKED);
        lv_obj_add_style(btn, &btnCheckedPressed, LV_STATE_CHECKED | LV_STATE_PRESSED);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        if (isOn) lv_obj_add_state(btn, LV_STATE_CHECKED);
        else      lv_obj_clear_state(btn, LV_STATE_CHECKED);
    }

    void applyMomentaryButton(lv_obj_t* btn) const {
        lv_obj_add_style(btn, &btnMain, 0);
        lv_obj_add_style(btn, &btnPressed, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(btn, COL_TEXT, 0);
    }

    void updateButtonToggle(lv_obj_t* btn, bool isOn) const {
        if (isOn) lv_obj_add_state(btn, LV_STATE_CHECKED);
        else      lv_obj_clear_state(btn, LV_STATE_CHECKED);
    }
};

END_NAMESPACE_DISTRHO

#endif // UI_STYLES_HPP
