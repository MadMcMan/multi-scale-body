/**
 * LVGL configuration for MultiScaleBody plugin
 * Copied from SphereEcho (DPF+LVGL9)
 */
#ifndef LV_CONF_H
#define LV_CONF_H
#include <stdint.h>
#define LV_COLOR_DEPTH 32
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB
#define LV_DEF_REFR_PERIOD  1
#define LV_DPI_DEF 130
#define LV_DRAW_BUF_STRIDE_ALIGN       1
#define LV_DRAW_BUF_ALIGN              4
#define LV_DRAW_TRANSFORM_USE_MATRIX   0
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE  (256 * 1024)
#define LV_DRAW_LAYER_MAX_MEMORY       0
#define LV_DRAW_THREAD_STACK_SIZE      (16 * 1024)
#define LV_DRAW_SW_SUPPORT_I1          1
#define LV_DRAW_SW_I1_LUM_THRESHOLD   127
#define LV_DRAW_SW_DRAW_UNIT_CNT      1
#define LV_DRAW_SW_SHADOW_CACHE_SIZE   0
#define LV_ENABLE_GLOBAL_CUSTOM 1
#define LV_USE_PRIVATE_API 1
// LVGL logging MUST stay off in production: LV_LOG_PRINTF writes to stdout,
// which hangs the UI forever when the host launches it with a piped/undrained
// stdout (bridged UIs, CI captures). Every lv_* call would block in WriteFile.
#define LV_USE_LOG 0
#define LV_USE_ASSERT_OBJ 0
#define LV_CACHE_DEF_SIZE       0
#define LV_IMAGE_HEADER_CACHE_DEF_CNT 0
#define LV_GRADIENT_MAX_STOPS   2
#define LV_COLOR_MIX_ROUND_OFS  0
#define LV_OBJ_STYLE_CACHE     0
#define LV_USE_OBJ_ID          0
#define LV_USE_OBJ_NAME        0
#define LV_USE_FLOAT  1
#define LV_USE_MATRIX 1
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS " ,.;:-_)]}"
#define LV_TXT_LINE_BREAK_LONG_LEN 0
#define LV_USE_FLEX 1
#define LV_USE_ARC 1
#define LV_USE_BAR 1
#define LV_USE_BUTTON 1
#define LV_USE_CHART 1
#define LV_USE_LABEL 1
#define LV_USE_ROLLER 1
#define LV_USE_SLIDER 1
#define LV_USE_TABVIEW 1
#define LV_USE_GRID 0
#define LV_USE_ANIMIMG 0
#define LV_USE_ARCLABEL 0
#define LV_USE_BUTTONMATRIX 1
#define LV_USE_CALENDAR 0
#define LV_USE_CANVAS 1
#define LV_USE_CHECKBOX 1
#define LV_USE_DROPDOWN 1
#define LV_USE_IMAGE 1
#define LV_USE_IMAGEBUTTON 1
#define LV_USE_KEYBOARD 1
#define LV_USE_LED 1
#define LV_USE_LINE 1
#define LV_USE_LIST 1
#define LV_USE_MENU 1
#define LV_USE_MSGBOX 1
#define LV_USE_SCALE 1
#define LV_USE_SPAN 1
#define LV_USE_SPINBOX 1
#define LV_USE_SPINNER 1
#define LV_USE_SWITCH 1
#define LV_USE_TABLE 1
#define LV_USE_TEXTAREA 1
#define LV_USE_TILEVIEW 1
#define LV_USE_WIN 1
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 1
#define LV_USE_THEME_SIMPLE 0
#define LV_FS_DEFAULT_DRIVER_LETTER '\0'
#define LV_USE_LODEPNG 0
#define LV_USE_LIBPNG 0
#define LV_USE_BMP 0
#define LV_USE_TJPGD 0
#define LV_USE_LIBJPEG_TURBO 0
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0
#define LV_USE_WINDOWS   0
#define LV_USE_X11       0
#define LV_USE_EVDEV     0
#define LV_USE_LIBINPUT  0
#define LV_USE_LINUX_DRM 0
#define LV_USE_SDL       0
#define LV_USE_SNAPSHOT 0
#define LV_USE_MONKEY 0
#define LV_USE_GRIDNAV 0
#define LV_USE_FRAGMENT 0
#define LV_USE_IMGFONT 0
#define LV_USE_OBSERVER 0
#define LV_USE_VECTOR_GRAPHIC 0
#define LV_USE_THORVG_INTERNAL 0
#define LV_USE_THORVG_EXTERNAL 0
#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_FONT_PLACEHOLDER 1
#define LV_ATTRIBUTE_TICK_INC
#define LV_ATTRIBUTE_TIMER_HANDLER
#define LV_ATTRIBUTE_FLUSH_READY
#define LV_ATTRIBUTE_MEM_ALIGN_SIZE 1
#define LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_LARGE_CONST
#define LV_ATTRIBUTE_LARGE_RAM_ARRAY
#define LV_ATTRIBUTE_FAST_MEM
#define LV_EXPORT_CONST_INT(int_value) struct _silence_gcc_warning
#define LV_ATTRIBUTE_EXTERN_DATA
#define LV_USE_ARABIC_PERSIAN_CHARS 0
#define LV_USE_DRAW_OPENGLES 0
#define LV_BIN_DECODER_RAM_LOAD 0
#define LV_USE_RLE 0
#define LV_USE_QRCODE 0
#define LV_USE_BARCODE 0
#define LV_USE_RLOTTIE 0
#define LV_USE_LZ4_INTERNAL  0
#define LV_USE_LZ4_EXTERNAL  0
#define LV_USE_XML    0
#define LV_USE_TRANSLATION 0
#define LV_USE_COLOR_FILTER 0
#define LV_USE_REFR_DEBUG 0
#define LV_USE_LAYER_DEBUG 0
#define LV_USE_PARALLEL_DRAW_DEBUG 0
#define LV_USE_OBJ_PROPERTY 0
#define LV_USE_OBJ_PROPERTY_NAME 0
#define LV_USE_SVG_DEBUG 0
#define LV_USE_GESTURE_RECOGNITION 0
#define LV_BIG_ENDIAN_SYSTEM 0
#endif
