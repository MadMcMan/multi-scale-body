#include "src/lv_conf_internal.h"
#include "src/core/lv_global.h"
lv_global_t* lv_global_default(void){ static lv_global_t g; return &g; }
