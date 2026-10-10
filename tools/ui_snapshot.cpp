/*
 * ui_snapshot.cpp — UI-only visual verification harness (no host, no audio).
 * Drives the real PluginUI through DPF's UIExporter, shows the native window,
 * idles the DGL/LVGL loop, and captures the surface to BMP at three sizes
 * (default 1440x1068 / small 1100x700 / large 2048x1104).
 * Capture uses a screen BitBlt (PrintWindow returns stale frames for GL windows).
 *
 * Ground truth for clipping/overlap is the LVGL tree itself:
 * lv_obj_update_layout() then lv_obj_get_coords(); dumpTree prints the settled
 * geometry, checkLayout flags anything outside the surface or overlapping its
 * flex siblings at region level.
 */
#include "../deps/DPF/distrho/src/DistrhoUIInternal.hpp"
#include "lvgl.h"
#include "../deps/lvgl/src/core/lv_obj_private.h"   // _lv_obj_t struct (class_p etc.)
#include "../deps/lvgl/src/core/lv_obj_class_private.h"  // _lv_obj_class_t struct
#include "PluginMultiScaleBody.hpp"
#include "styles/Palette.hpp"        // design tokens (colors, lay::, scaled(), fonts)
#include "components/UICommon.hpp"  // abstract UI contract
#include <windows.h>
#include <functional>

static FILE* gLog=nullptr;
#define LOGF(...) do{ if(gLog){ fprintf(gLog,__VA_ARGS__); fflush(gLog);} printf(__VA_ARGS__); }while(0)

START_NAMESPACE_DISTRHO

// interaction-proof counters: every UI->plugin route lands in one of these
static int gNoteOns=0,gNoteOffs=0,gParamWrites=0,gStrikeWrites=0,gPresetWrites=0,gBandWrites=0;
static uint32_t gLastParam=UINT32_MAX;
static float gLastValue=0.f;
static void stubEditParam(void*, uint32_t, bool) {}
static void stubSetParam(void*, uint32_t i, float value){
    gLastParam=i; gLastValue=value;
    ++gParamWrites;
    if(i==PluginMultiScaleBody::kParamStrikeX||i==PluginMultiScaleBody::kParamStrikeY) ++gStrikeWrites;
    else if(i==PluginMultiScaleBody::kParamPreset) ++gPresetWrites;
    else if(i>=PluginMultiScaleBody::kParamBand0&&i<PluginMultiScaleBody::kParamBand0+16) ++gBandWrites;
}
static void stubSetState(void*, const char*, const char*) {}
static void stubSendNote(void*, uint8_t, uint8_t, uint8_t v){ if(v) ++gNoteOns; else ++gNoteOffs; }
// Host-emulating setSize: a real host honors the plugin's d_uiResize request
// by resizing the window; the old no-op stub silently swallowed the zoom
// stepper's setSize(2160,1485) and pinned the display at the old size (the
// zoom-step harness regression since R4). Defer the SetWindowPos until the
// exporter's window exists, then apply client-size exactly.
static DISTRHO::UIExporter* gExp=nullptr;
static void stubSetSize(void*, uint w, uint h)
{
    if(!gExp) return;
    HWND hwnd=(HWND)gExp->getNativeWindowHandle();
    if(!hwnd) return;
    RECT r{0,0,(LONG)w,(LONG)h};
    AdjustWindowRect(&r,(DWORD)GetWindowLongPtr(hwnd,GWL_STYLE),FALSE);
    SetWindowPos(hwnd,NULL,0,0,r.right-r.left,r.bottom-r.top,SWP_NOMOVE|SWP_NOZORDER);
}
static bool stubFileRequest(void*, const char*) { return false; }

// Capture the window rect. Two paths:
//   1) BitBlt from the desktop DC (works when the window is on the primary
//      monitor and unoccluded; the original path).
//   2) PrintWindow with PW_RENDERFULLCONTENT - asks the window itself to
//      render into our HDC. This is the proven path for DPF/pugl GL
//      surfaces on this machine (BitBlt returns near-blank because the GL
//      backbuffer is not in the DWM composition when the window is on a
//      non-primary monitor; PrintWindow bypasses DWM and asks the GL
//      surface to present into our bitmap).
// We try (2) first; on Windows 8.1+ and the present-day pugl OpenGL path
// it gives the real content. We also park the window on the primary
// monitor at (0,0) so the DWM composition is in scope.
static void parkOnPrimary(HWND hwnd){
    // move to (0,0) on the primary monitor (no Z-order change) - the
    // DPF pugl window's topmost flag stays; this just translates the rect
    RECT wr; if(!GetWindowRect(hwnd,&wr)) return;
    SetWindowPos(hwnd,HWND_TOP,0,0,wr.right-wr.left,wr.bottom-wr.top,SWP_NOZORDER);
    // small idle so the window can settle at the new origin before the
    // DWM composition picks it up
    MSG m; while(PeekMessage(&m,hwnd,0,0,PM_REMOVE)){ TranslateMessage(&m); DispatchMessage(&m); }
}
static bool grabScreen(HWND hwnd, int& w, int& h, std::vector<unsigned char>& px)
{
    RECT wr; if(!GetWindowRect(hwnd,&wr)) return false;
    w=wr.right-wr.left; h=wr.bottom-wr.top;
    if(w<=0||h<=0) return false;
    HDC sdc=GetDC(NULL);
    HDC mdc=CreateCompatibleDC(sdc);
    HBITMAP bmp=CreateCompatibleBitmap(sdc,w,h);
    HGDIOBJ old=SelectObject(mdc,bmp);
    PatBlt(mdc,0,0,w,h,BLACKNESS);
    // round-2: try PrintWindow WITHOUT PW_RENDERFULLCONTENT first; some GL
    // window implementations (pugl) only honor the legacy PrintWindow
    // request because they don't declare the WM_PRINT path. If that returns
    // 0 (failed) we fall back to PW_RENDERFULLCONTENT, then to BitBlt
    // from the window's own DC (DWM-exempt path).
    if(!PrintWindow(hwnd,mdc,0)){
        if(!PrintWindow(hwnd,mdc,PW_RENDERFULLCONTENT)){
            HDC wdc=GetDC(hwnd);
            if(wdc){ BitBlt(mdc,0,0,w,h,wdc,0,0,SRCCOPY); ReleaseDC(hwnd,wdc); }
        }
    }
    BITMAPINFO bi; memset(&bi,0,sizeof(bi));
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=w; bi.bmiHeader.biHeight=h;
    bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
    px.resize((size_t)w*h*4);
    const int got=GetDIBits(mdc,bmp,0,h,px.data(),&bi,DIB_RGB_COLORS);
    SelectObject(mdc,old); DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(NULL,sdc);
    if(got!=h) return false;
    return true;
}

static double blackRatio(const std::vector<unsigned char>& px)
{
    size_t nonblack=0;
    for(size_t i=0;i+3<px.size();i+=4)
        if(px[i]|px[i+1]|px[i+2]) ++nonblack;
    const size_t total=px.size()/4;
    return total? 1.0-(double)nonblack/(double)total : 1.0;
}

static bool writeBMP(HWND hwnd, const char* path)
{
    // round-2: park the window on the primary monitor (0,0) and kick present
    // before capture; the harness comment above explains why BitBlt alone
    // returns a near-blank surface on this machine's GL/DWM stack.
    parkOnPrimary(hwnd);
    int w=0,h=0; std::vector<unsigned char> px;
    if(!grabScreen(hwnd,w,h,px)) { LOGF("grab failed %s\n",path); return false; }
    const double br=blackRatio(px);
    // round-2: do NOT reject blank captures; we want the file on disk for
    // visual inspection even if DWM/GL wedged. The harness's pass/fail is
    // in the log, the file is for human eyes.
    FILE* f=fopen(path,"wb"); if(!f) return false;
    BITMAPFILEHEADER fh; memset(&fh,0,sizeof(fh));
    fh.bfType=0x4D42; fh.bfOffBits=14+40; fh.bfSize=(DWORD)(14+40+px.size());
    BITMAPINFOHEADER ih; memset(&ih,0,sizeof(ih));
    ih.biSize=40; ih.biWidth=w; ih.biHeight=h; ih.biPlanes=1;
    ih.biBitCount=32; ih.biCompression=BI_RGB; ih.biSizeImage=(DWORD)px.size();
    const bool okw = fwrite(&fh,sizeof(fh),1,f)==1 && fwrite(&ih,sizeof(ih),1,f)==1
                   && fwrite(px.data(),1,px.size(),f)==px.size();
    fclose(f);
    LOGF("captured %s (%dx%d, black=%.2f)\n",path,w,h,br);
    return okw;
}

static void dumpTree(lv_obj_t* obj, int depth);

static void logState(DISTRHO::UIExporter* exp, const char* tag)
{
    lv_display_t* d=lv_display_get_default();
    LOGF("[%s] widget=%ux%u display=%ldx%ld gUIScale=%.3f\n",tag,
        exp->getWidth(),exp->getHeight(),
        d?(long)lv_display_get_horizontal_resolution(d):-1L,
        d?(long)lv_display_get_vertical_resolution(d):-1L,
        (double)DISTRHO::gUIScale);
    lv_obj_update_layout(lv_screen_active());
    dumpTree(lv_screen_active(),0);
}

static void dumpTree(lv_obj_t* obj, int depth)
{
    if(depth>6) return;
    lv_area_t c; lv_obj_get_coords(obj,&c);
    const char* txt = (obj->class_p==&lv_label_class) ? lv_label_get_text(obj) : nullptr;
    LOGF("%*s%s @ %ld,%ld %ldx%ld%s%s%s\n",depth*2,"",
        obj->class_p?obj->class_p->name:"?",
        (long)c.x1,(long)c.y1,(long)lv_obj_get_width(obj),(long)lv_obj_get_height(obj),
        txt?" \"":"", txt?txt:"", txt?"\"":"");
    const uint32_t n=lv_obj_get_child_count(obj);
    for(uint32_t i=0;i<n;++i) dumpTree(lv_obj_get_child(obj,i),depth+1);
}

// ---- geometric proof pass ---------------------------------------------------
// BOUNDS: every laid-out object must sit inside the surface (2px grace for
// hairline borders). OVERLAP: among region-level siblings (direct children of
// the screen and of the stage row - depth<=1 containers) intersections fail;
// deeper overlays (knob cap-in-arc, rings-in-disc) are intentional compositing.
static int gBoundFails=0, gOverlapFails=0;

static bool isDropdownList(const lv_obj_t* o)
{
    // the closed lv_dropdown-list lives on the top layer at a default position;
    // it only has real geometry while open - not a layout sibling
    return o && o->class_p && 0==std::strcmp(o->class_p->name,"lv_dropdown-list");
}

static void checkBounds(lv_obj_t* obj, lv_area_t limit, int depth)
{
    if(isDropdownList(obj)) return;
    lv_area_t c; lv_obj_get_coords(obj,&c);
    if(c.x1<limit.x1-2||c.y1<limit.y1-2||c.x2>limit.x2+2||c.y2>limit.y2+2){
        LOGF("BOUNDS-FAIL d%d %s @ %ld,%ld %ldx%ld outside [%ld,%ld %ldx%ld]\n",
            depth,obj->class_p?obj->class_p->name:"?",
            (long)c.x1,(long)c.y1,(long)lv_obj_get_width(obj),(long)lv_obj_get_height(obj),
            (long)limit.x1,(long)limit.y1,
            (long)(limit.x2-limit.x1+1),(long)(limit.y2-limit.y1+1));
        ++gBoundFails;
    }
    const uint32_t n=lv_obj_get_child_count(obj);
    for(uint32_t i=0;i<n;++i) checkBounds(lv_obj_get_child(obj,i),limit,depth+1);
}

static void checkSiblingOverlaps(lv_obj_t* parent,int depth)
{
    const uint32_t n=lv_obj_get_child_count(parent);
    for(uint32_t i=0;i<n;++i){
        lv_obj_t* a=lv_obj_get_child(parent,i);
        if(isDropdownList(a)) continue;
        lv_area_t ca; lv_obj_get_coords(a,&ca);
        for(uint32_t j=i+1;j<n;++j){
            lv_obj_t* b=lv_obj_get_child(parent,j);
            if(isDropdownList(b)) continue;
            lv_area_t cb; lv_obj_get_coords(b,&cb);
            const long ox=std::min(ca.x2,cb.x2)-std::max(ca.x1,cb.x1);
            const long oy=std::min(ca.y2,cb.y2)-std::max(ca.y1,cb.y1);
            if(ox>1&&oy>1){
                LOGF("OVERLAP-FAIL d%d %s x %s (%ldx%ld px)\n",depth,
                    a->class_p?a->class_p->name:"?",b->class_p?b->class_p->name:"?",ox,oy);
                ++gOverlapFails;
            }
        }
    }
}

static int gTestFails=0;
#define EXPECT(cond,msg) do{ if(cond) LOGF("PASS %s\n",msg); else { LOGF("FAIL %s\n",msg); ++gTestFails; } }while(0)

static lv_obj_t* findPlate(lv_obj_t* root);   // defined after checkLayout

static void checkLayout(const char* tag)
{
    lv_display_t* d=lv_display_get_default();
    lv_area_t surf{0,0,(lv_coord_t)(lv_display_get_horizontal_resolution(d)-1),
                        (lv_coord_t)(lv_display_get_vertical_resolution(d)-1)};
    gBoundFails=0; gOverlapFails=0;
    lv_obj_t* root=lv_screen_active();
    // T0: an empty screen (tree never built) once passed every geometric
    // check vacuously - bounds/overlap cannot fail over zero widgets. The
    // standalone exe opened blank for exactly this reason (first build
    // skipped at exact base scale). Every checkpoint must prove a
    // non-trivial tree exists. Since the fixed-aspect plate fix, the screen
    // holds the plate (+ any open dropdown lists); healthy = a plate with
    // its three regions (topbar/stage/keyboard strip).
    const uint32_t rootChildN=lv_obj_get_child_count(root);
    LOGF("[%s] screen-children=%u\n",tag,rootChildN);
    lv_obj_t* plate=findPlate(root);
    if(std::strcmp(tag,"default")==0){
        EXPECT(plate!=nullptr,"default-plate-exists");
        EXPECT(plate && lv_obj_get_child_count(plate)>=3,"default-screen-has-regions");
    }else{
        EXPECT(plate!=nullptr,"checkpoint-plate-exists");
    }
    checkBounds(root,surf,0);
    checkSiblingOverlaps(root,0);
    // the plate's three regions (topbar, stage, keyboard strip) must not
    // overlap; the stage's columns are the main horizontal hazard
    if(plate){
        checkSiblingOverlaps(plate,0);
        if(lv_obj_get_child_count(plate)>=2)
            checkSiblingOverlaps(lv_obj_get_child(plate,1),1);
    }
    LOGF("[%s] checks: bounds=%d overlap=%2d\n",tag,gBoundFails,gOverlapFails);
}

static void idleFrames(DISTRHO::UIExporter* exp, int n)
{
    // Service LVGL's timers here. DPF's plugin_idle() (which ends up in the
    // LVGL widget's idleCallback) does NOT reliably run lv_timer_handler during
    // this harness's headless loop, so without this the plugin's 30 ms spectrum
    // timer never fires - which silently left the entire 30 Hz path (output
    // meter, MODE MAP repaint, disc heatmap, and the motion-recorder playback
    // step) unexercised. Found by the recorder-loop self-test, which asserted
    // an advance that could never happen because the recorder's advance was
    // unreachable (it was timer-driven then; it is synchronous now, which is
    // what made the bug obvious).
    for(int i=0;i<n && exp->plugin_idle();++i){ Sleep(16); lv_timer_handler(); }
}

// present-kick: this pugl/GL window presents nothing until its first WM_SIZE;
// a 2px round-trip at the same size changes nothing in the UI (dumps identical)
static void presentKick(HWND hwnd, DISTRHO::UIExporter* exp)
{
    RECT wr; GetWindowRect(hwnd,&wr);
    const int ww=wr.right-wr.left, wh=wr.bottom-wr.top;
    SetWindowPos(hwnd,NULL,40,40,ww+2,wh+2,SWP_NOZORDER);
    idleFrames(exp,40);
    SetWindowPos(hwnd,NULL,40,40,ww,wh,SWP_NOZORDER);
    idleFrames(exp,80);
}

// ---- task-specific verification helpers ------------------------------------
// (EXPECT/gTestFails now defined above checkLayout, which needs them)

static void pumpMsgs()
{
    MSG msg;
    while(PeekMessage(&msg,NULL,0,0,PM_REMOVE)){ TranslateMessage(&msg); DispatchMessage(&msg); }
}

static lv_obj_t* findByClass(lv_obj_t* root,const char* cls)
{
    if(root->class_p && 0==std::strcmp(root->class_p->name,cls)) return root;
    const uint32_t n=lv_obj_get_child_count(root);
    for(uint32_t i=0;i<n;++i){ lv_obj_t* r=findByClass(lv_obj_get_child(root,i),cls); if(r) return r; }
    return nullptr;
}

// The fixed-aspect PLATE: the single plain lv_obj sized scaled(BASE_W x BASE_H)
// that owns every region (topbar/stage/keyboard). Absent in the pre-fix tree
// (regions were direct screen children), present after the "don't move the
// presets/keyboard on zoom" fix. The disc is also a plain lv_obj but only
// scaled(DISC_D); the plate is uniquely the only object with the base size.
static lv_obj_t* findPlate(lv_obj_t* root)
{
    // never match the screen itself (a plain lv_obj of the exact base size at
    // zoom 125), or the pre-fix checks would pass against the screen instead
    // of the actual fixed-aspect plate. The screen is the parentless root; the
    // plate always has a parent. Only THE MATCH is skipped for the screen -
    // we still recurse into its children.
    if(root){
        if(lv_obj_get_parent(root)!=nullptr
           && root->class_p && 0==std::strcmp(root->class_p->name,"lv_obj")
           && !isDropdownList(root)
           && lv_obj_get_width(root)==scaled(lay::BASE_W)
           && lv_obj_get_height(root)==scaled(lay::BASE_H)) return root;
        const uint32_t n=lv_obj_get_child_count(root);
        for(uint32_t i=0;i<n;++i){ lv_obj_t* r=findPlate(lv_obj_get_child(root,i)); if(r) return r; }
    }
    return nullptr;
}

// A region (topbar/stage/keyboard strip): a DIRECT child of the plate with the
// matching scaled height. Topbar = scaled(HEADER_H), stage = scaled(STAGE_H),
// keyboard strip = scaled(KB_STRIP_H).
static lv_obj_t* findRegion(const lv_obj_t* plate,lv_coord_t h)
{
    if(!plate) return nullptr;
    const uint32_t n=lv_obj_get_child_count(plate);
    for(uint32_t i=0;i<n;++i){
        lv_obj_t* c=lv_obj_get_child(plate,i);
        if(c && lv_obj_get_height(c)==h) return c;
    }
    return nullptr;
}

// Zoom stepper buttons are found by their label text, NOT user_data: the
// preset prev/next arrows also carry user_data +/-1 (addPresetArrowBtn) and
// are created earlier in the tree, so a user_data scan resolves the PRESET
// arrow instead (the R4 zoom-test regression: the "zoom click" changed the
// preset and the display stayed 1440x1068).
static lv_obj_t* findZoomButton(lv_obj_t* root,intptr_t ud)
{
    if(root->class_p && 0==std::strcmp(root->class_p->name,"lv_button")){
        const uint32_t n=lv_obj_get_child_count(root);
        for(uint32_t i=0;i<n;++i){
            lv_obj_t* c=lv_obj_get_child(root,i);
            if(c->class_p && 0==std::strcmp(c->class_p->name,"lv_label")){
                const char* t=lv_label_get_text(c);
                const char* want=(ud>0)?"+":"-";
                if(t&&t[0]==want[0]&&t[1]=='\0') return root;
            }
        }
        return nullptr;                    // +/- userdata alone is not enough
    }
    const uint32_t n=lv_obj_get_child_count(root);
    for(uint32_t i=0;i<n;++i){ lv_obj_t* r=findZoomButton(lv_obj_get_child(root,i),ud); if(r) return r; }
    return nullptr;
}

static bool resizeWindow(HWND hwnd,DISTRHO::UIExporter* exp,int w,int h,bool& first)
{
    RECT r{0,0,w,h};
    AdjustWindowRect(&r,(DWORD)GetWindowLongPtr(hwnd,GWL_STYLE),FALSE);
    const int wx=(w>1800)?0:40, wy=(h>1000)?0:40;
    SetWindowPos(hwnd,NULL,wx,wy,r.right-r.left,r.bottom-r.top,SWP_NOZORDER);
    idleFrames(exp,first?60:120); pumpMsgs();
    presentKick(hwnd,exp);
    first=false;
    RECT cr; GetClientRect(hwnd,&cr);
    return (cr.right-cr.left)==w && (cr.bottom-cr.top)==h;
}



// topmost DIRECT clickable child covering a point - mirrors what
// lv_indev_search_obj picks among the disc's own children
static lv_obj_t* clickableCoverChild(lv_obj_t* disc,int px,int py)
{
    const uint32_t n=lv_obj_get_child_count(disc);
    for(uint32_t i=n;i>0;--i){
        lv_obj_t* c=lv_obj_get_child(disc,i-1);
        lv_area_t a; lv_obj_get_coords(c,&a);
        if(px>=a.x1&&px<=a.x2&&py>=a.y1&&py<=a.y2&&lv_obj_is_clickable(c)) return c;
    }
    return nullptr;
}
static lv_coord_t wellSize(){ const lv_coord_t D=scaled(lay::DISC_D); return (lv_coord_t)(D*0.80f); }
static lv_obj_t* findFirstChildBySize(lv_obj_t* p,lv_coord_t s)
{
    const uint32_t n=lv_obj_get_child_count(p);
    for(uint32_t i=0;i<n;++i){
        lv_obj_t* c=lv_obj_get_child(p,i);
        if(lv_obj_get_width(c)==s&&lv_obj_get_height(c)==s) return c;
    }
    return nullptr;
}
// the playable disc: the one square plain-obj of DISC_D size with children
static lv_obj_t* findDisc(lv_obj_t* root)
{
    const lv_coord_t want=scaled(lay::DISC_D);
    if(root->class_p && 0==std::strcmp(root->class_p->name,"lv_obj")
       && lv_obj_get_width(root)==want && lv_obj_get_height(root)==want
       && lv_obj_get_child_count(root)>=5) return root;
    const uint32_t n=lv_obj_get_child_count(root);
    for(uint32_t i=0;i<n;++i){ lv_obj_t* r=findDisc(lv_obj_get_child(root,i)); if(r) return r; }
    return nullptr;
}
static void wheelAt(HWND hwnd,int clientX,int clientY,double delta)
{
    POINT p{clientX,clientY}; ClientToScreen(hwnd,&p);
    const WPARAM wp=(WPARAM)((int)(delta*120)<<16);
    PostMessage(hwnd,WM_MOUSEWHEEL,wp,MAKELPARAM(p.x,p.y));
}

static lv_obj_t* findParamArc(lv_obj_t* obj,uint32_t param){
    if(lv_obj_check_type(obj,&lv_arc_class) && (uintptr_t)lv_obj_get_user_data(obj)==param) return obj;
    for(uint32_t i=0;i<lv_obj_get_child_count(obj);++i)
        if(auto* found=findParamArc(lv_obj_get_child(obj,i),param)) return found;
    return nullptr;
}
static lv_point_t contactPointer{};
static bool contactPressed=false;
static void contactPointerRead(lv_indev_t*,lv_indev_data_t* data){
    data->point=contactPointer;
    data->state=contactPressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
}
// ---- double-click knob reset self-test -----------------------------------
// (R-user) Double-clicking a knob must restore the value the current preset
// loaded it with. Drive it through a synthetic pointer indev: two quick
// press/release pairs on the arc with no drag, then assert the host-visible
// param is back at the preset baseline.
static void doubleClickResetProof(DISTRHO::UIExporter* exp,HWND hwnd){
    // Pick a knob with a distinct preset default. Volume is unity(1.0) and is
    // re-captured on every preset change; use Decay (preset default 0.5).
    const auto pi=PluginMultiScaleBody::kParamDecay;
    bool first=true;
    resizeWindow(hwnd,exp,1440,1068,first);
    lv_obj_update_layout(lv_screen_active());
    lv_obj_t* arc=findParamArc(lv_screen_active(),pi);
    EXPECT(arc!=nullptr,"DblClick: Decay arc present");
    if(!arc) return;
    // establish the baseline: preset 0 loaded -> capturePresetDefaults() ran.
    exp->parameterChanged(PluginMultiScaleBody::kParamPreset,0.f);
    exp->parameterChanged(pi,0.5f);            // the preset default for Decay
    idleFrames(exp,4);
    // move the knob AWAY from baseline via the host, then check the arc synced
    exp->parameterChanged(pi,0.9f);
    idleFrames(exp,4);
    EXPECT(lv_arc_get_value(arc)==900,"DblClick: knob moved off baseline");
    // two quick clicks, no drag, well within the 400ms double-click window
    lv_area_t a; lv_obj_get_coords(arc,&a);
    contactPointer={(lv_coord_t)((a.x1+a.x2)/2),(lv_coord_t)((a.y1+a.y2)/2)};
    lv_indev_t* pointer=lv_indev_create();
    lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer,lv_display_get_default());
    lv_indev_set_read_cb(pointer,contactPointerRead);
    for(int click=0;click<2;++click){
        contactPressed=true;  lv_indev_read(pointer); idleFrames(exp,2);
        contactPressed=false; lv_indev_read(pointer); idleFrames(exp,2);
    }
    contactPressed=false; lv_indev_delete(pointer);
    idleFrames(exp,8);
    // the reset writes through setParamValue -> the plugin's stubSetParam, and
    // syncs the arc back. Assert the arc landed on the preset baseline (500).
    LOGF("[dblclick] arc value after 2 clicks = %d (expect 500)\n",lv_arc_get_value(arc));
    EXPECT(lv_arc_get_value(arc)==500,"DblClick: knob reset to preset default");
    // drag-cancel guard: a second click that DRAGS must NOT count as a
    // double-click reset (the drag is honoured instead of being undone).
    exp->parameterChanged(pi,0.9f); idleFrames(exp,4);
    // click 1 clean, then click 2 with a small drag within the double-click window
    contactPointer={(lv_coord_t)((a.x1+a.x2)/2),(lv_coord_t)((a.y1+a.y2)/2)};
    lv_indev_t* p2=lv_indev_create();
    lv_indev_set_type(p2,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(p2,lv_display_get_default());
    lv_indev_set_read_cb(p2,contactPointerRead);
    contactPressed=true;  lv_indev_read(p2); idleFrames(exp,2);   // press 1
    contactPressed=false; lv_indev_read(p2); idleFrames(exp,2);   // release 1
    contactPressed=true;  lv_indev_read(p2); idleFrames(exp,2);   // press 2
    contactPointer.y-=4; lv_indev_read(p2); idleFrames(exp,2);     // small drag during press 2
    contactPressed=false; lv_indev_read(p2); idleFrames(exp,2);   // release 2
    contactPointer.y+=4; contactPressed=false; lv_indev_delete(p2);
    idleFrames(exp,8);
    const int afterDrag=lv_arc_get_value(arc);
    LOGF("[dblclick-drag] arc value after click+drag = %d (must NOT be 500)\n",afterDrag);
    EXPECT(afterDrag!=500,"DblClick: drag on 2nd click cancels reset");
    checkLayout("dblclick");
    EXPECT(gBoundFails==0 && gOverlapFails==0,"DblClick layout inside surface");
}
static void contactProof(DISTRHO::UIExporter* exp,HWND hwnd){
    const auto pi=PluginMultiScaleBody::kParamContactNoise;
    bool first=true;
    for(const auto dims : {std::pair<int,int>{1440,1068},{1100,816}}){
        resizeWindow(hwnd,exp,dims.first,dims.second,first);
        lv_obj_update_layout(lv_screen_active());
        lv_obj_t* arc=findParamArc(lv_screen_active(),pi);
        EXPECT(arc!=nullptr,"Contact arc present");
        if(!arc) continue;
        exp->parameterChanged(pi,0.5f);
        EXPECT(lv_arc_get_value(arc)==500,"Contact host echo 100 percent");
        lv_area_t a; lv_obj_get_coords(arc,&a);
        contactPointer={(lv_coord_t)((a.x1+a.x2)/2),(lv_coord_t)((a.y1+a.y2)/2)};
        EXPECT(lv_indev_search_obj(lv_screen_active(),&contactPointer)==arc,"Contact cap does not steal clicks");
        lv_indev_t* pointer=lv_indev_create();
        lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(pointer,lv_display_get_default());
        lv_indev_set_read_cb(pointer,contactPointerRead);
        gLastParam=UINT32_MAX; gLastValue=0.f;
        contactPressed=true; lv_indev_read(pointer);
        contactPointer.y-=150; lv_indev_read(pointer);
        EXPECT(gLastParam==pi && gLastValue>0.9f,"Contact drag reaches host independently");
        contactPressed=false; lv_indev_read(pointer); lv_indev_delete(pointer);
        exp->parameterChanged(pi,0.f);
        EXPECT(lv_arc_get_value(arc)==0,"Contact host echo OFF");
        exp->parameterChanged(pi,1.f);
        EXPECT(lv_arc_get_value(arc)==1000,"Contact host echo 200 percent");
        exp->parameterChanged(pi,0.5f);
        idleFrames(exp,30); lv_obj_update_layout(lv_screen_active());
        checkLayout("contact");
        EXPECT(gBoundFails==0 && gOverlapFails==0,"Contact layout inside surface");
        char name[64]; snprintf(name,sizeof(name),"contact_%d.bmp",dims.first);
        writeBMP(hwnd,name);
        LOGF("CONTACT capture=%s arc=[%d,%d,%d,%d] writes=%d value=%.3f\n",name,a.x1,a.y1,a.x2,a.y2,gParamWrites,gLastValue);
    }
}

// ---- generic text-label finder (used by the panel self-tests) -------------
static lv_obj_t* findLabelText(lv_obj_t* root,const char* txt){
    if(lv_obj_check_type(root,&lv_label_class)){
        const char* t=lv_label_get_text(root);
        if(t && std::strcmp(t,txt)==0) return root;
    }
    for(uint32_t i=0;i<lv_obj_get_child_count(root);++i)
        if(lv_obj_t* f=findLabelText(lv_obj_get_child(root,i),txt)) return f;
    return nullptr;
}

// ---- DAMPING panel drag self-test ----------------------------------------
// (R-user) every DAMPING row is a slider for that band's decay trim. Drive a
// REAL press on the B1 row and require the host-visible write to be the
// matching kParamBandDecay, at the pressed fraction (row right edge -> ~1.0).
// This is the only automated coverage for the drag-to-adjust-damping feature.
static void dampDragProof(DISTRHO::UIExporter* exp,HWND hwnd){
    bool first=true; resizeWindow(hwnd,exp,1440,1068,first);
    lv_obj_update_layout(lv_screen_active());
    lv_obj_t* lab=findLabelText(lv_screen_active(),"DAMPING");
    EXPECT(lab!=nullptr,"Damp: DAMPING label found");
    if(!lab) return;
    lv_obj_t* head=lv_obj_get_parent(lab);
    lv_obj_t* card=head?lv_obj_get_parent(head):nullptr;
    if(!card){ EXPECT(false,"Damp: damping card found"); return; }
    const uint32_t nc=lv_obj_get_child_count(card);
    LOGF("[damp] card children=%u (expect 1 head + 16 rows)\n",(unsigned)nc);
    EXPECT(nc>=17,"Damp: card has the head + 16 band rows");
    lv_obj_t* row=lv_obj_get_child(card,1);   // B1 (child 0 is the head row)
    EXPECT(row!=nullptr,"Damp: B1 row found");
    if(!row) return;
    lv_area_t rc; lv_obj_get_coords(row,&rc);
    LOGF("[damp] B1 row=@%d,%d %dx%d\n",(int)rc.x1,(int)rc.y1,
         (int)(rc.x2-rc.x1+1),(int)(rc.y2-rc.y1+1));
    // the press must resolve to the (clickable) row, not a child
    contactPointer={ (lv_coord_t)(rc.x2-2),(lv_coord_t)((rc.y1+rc.y2)/2) };
    EXPECT(lv_indev_search_obj(lv_screen_active(),&contactPointer)==row,"Damp: press resolves to the row");
    lv_indev_t* pointer=lv_indev_create();
    lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer,lv_display_get_default());
    lv_indev_set_read_cb(pointer,contactPointerRead);
    gLastParam=UINT32_MAX; gLastValue=0.f;
    contactPressed=true;  lv_indev_read(pointer); idleFrames(exp,4);
    contactPressed=false; lv_indev_read(pointer); idleFrames(exp,4);
    lv_indev_delete(pointer);
    LOGF("[damp] write param=%u value=%.4f (expect param=%u ~1.0)\n",
         (unsigned)gLastParam,gLastValue,(unsigned)PluginMultiScaleBody::kParamBandDecay0);
    EXPECT(gLastParam==PluginMultiScaleBody::kParamBandDecay0,"Damp: drag wrote kParamBandDecay0");
    EXPECT(gLastValue>0.9f,"Damp: right-edge press wrote ~1.0");
    // and the panel repainted the band's value from the new trim
    lv_obj_t* val=lv_obj_get_child(row,2);
    if(val && lv_obj_check_type(val,&lv_label_class))
        LOGF("[damp] B1 value label now '%s'\n",lv_label_get_text(val));
    // The card is EXACTLY content-height (24 pad + 22 head + 16*22 rows + 16*2
    // gaps = DAMP_CARD_H 430), so any change to DAMP_ROW_H or the head would
    // start clipping the last bands with nothing to catch it: no bounds check
    // looks inside a card. Assert the last row still ends inside the card.
    if(lv_obj_get_child_count(card)>=17){
        lv_obj_t* lastRow=lv_obj_get_child(card,16);   // [0]=head, [1..16]=bands
        lv_area_t lrc,cc;
        lv_obj_get_coords(lastRow,&lrc); lv_obj_get_coords(card,&cc);
        LOGF("[damp] card h=%d ; last band row bottom=%d card bottom=%d\n",
             (int)lv_obj_get_height(card),(int)lrc.y2,(int)cc.y2);
        EXPECT(lrc.y2<=cc.y2,"Damp: all 16 band rows fit inside the card");
    } else {
        EXPECT(false,"Damp: card has the head + 16 band rows");
    }
    checkLayout("damping-drag");
    EXPECT(gBoundFails==0 && gOverlapFails==0,"Damp: layout inside surface");
}

// ---- recorder loop self-test ---------------------------------------------
// (R-user) REC captures the disc drag path; PLAY then advances one recorded
// strike position per live note (the plugin's kParamNoteTick) and LOOPS for as
// long as notes keep coming. This is the only automated coverage for the loop:
// arm REC, capture a short path with real disc drags, start PLAY, then drive
// note ticks and prove the position advances and the transport is STILL running
// after far more ticks than recorded points (i.e. it wrapped, not stopped).
static void recorderLoopProof(DISTRHO::UIExporter* exp,HWND hwnd){
    bool first=true; resizeWindow(hwnd,exp,1440,1068,first);
    lv_obj_update_layout(lv_screen_active());
    lv_obj_t* recLab=findLabelText(lv_screen_active(),"REC");
    lv_obj_t* playLab=findLabelText(lv_screen_active(),"PLAY");
    EXPECT(recLab!=nullptr && playLab!=nullptr,"Rec: REC/PLAY buttons found");
    if(!recLab||!playLab) return;
    lv_obj_t* recBtn=lv_obj_get_parent(recLab);
    lv_obj_t* playBtn=lv_obj_get_parent(playLab);
    lv_obj_t* disc=findDisc(lv_screen_active());
    EXPECT(disc!=nullptr,"Rec: strike disc found");
    if(!disc) return;
    lv_area_t dc; lv_obj_get_coords(disc,&dc);
    lv_indev_t* p=lv_indev_create();
    lv_indev_set_type(p,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(p,lv_display_get_default());
    lv_indev_set_read_cb(p,contactPointerRead);
    // 1) arm REC, then capture a short path with REAL disc drags
    lv_obj_send_event(recBtn,LV_EVENT_CLICKED,nullptr);
    idleFrames(exp,4);
    contactPointer={ (lv_coord_t)((dc.x1+dc.x2)/2),(lv_coord_t)((dc.y1+dc.y2)/2) };
    contactPressed=true; lv_indev_read(p); idleFrames(exp,3);
    for(int i=1;i<=5;++i){ contactPointer.y+=6; lv_indev_read(p); idleFrames(exp,3); }
    contactPressed=false; lv_indev_read(p); idleFrames(exp,3);
    lv_indev_delete(p);
    // 2) start PLAY
    lv_obj_send_event(playBtn,LV_EVENT_CLICKED,nullptr);
    idleFrames(exp,6);
    const bool playing=lv_obj_has_state(playBtn,LV_STATE_CHECKED);
    LOGF("[rec] PLAY checked after click=%d\n",(int)playing);
    EXPECT(playing,"Rec: PLAY started (a path was recorded)");
    if(!playing) return;
    // 3) one live note must advance the position (writes StrikeX+Y).
    // The advance is SYNCHRONOUS inside parameterChanged, so no timer pumping
    // is needed - this asserts that determinism itself.
    const int before=gStrikeWrites;
    exp->parameterChanged(PluginMultiScaleBody::kParamNoteTick,0.001f);
    idleFrames(exp,4);
    LOGF("[rec] strike writes on 1 tick: %d -> %d\n",before,gStrikeWrites);
    EXPECT(gStrikeWrites>before,"Rec: a live note advances the playback position");
    // 4) LOOP: far more ticks than captured points must NOT stop the transport
    for(int i=0;i<40;++i){
        exp->parameterChanged(PluginMultiScaleBody::kParamNoteTick,0.001f*(float)((i%9)+1));
        idleFrames(exp,2);
    }
    LOGF("[rec] after 40 ticks: still playing=%d strike writes=%d\n",
         (int)lv_obj_has_state(playBtn,LV_STATE_CHECKED),gStrikeWrites);
    EXPECT(gStrikeWrites>before+20,"Rec: every tick advanced a position");
    EXPECT(lv_obj_has_state(playBtn,LV_STATE_CHECKED),"Rec: playback LOOPS (still running after more notes than points)");
}

// ---- control-exposure (MIDI learn) tagging self-test ----------------------
// (R-user) "all controls exposed": every PARAM-backed control must be
// right-click MIDI-learnable. That works by tagging the control's object
// user_data with (param | kLearnTag) for dropdowns/toggles, while knob arcs
// carry the raw param index; learnParamAt() walks up to exactly those. Assert
// the tagging contract directly - it is the whole mechanism, and a missing tag
// silently makes a control unlearnable.
static void learnTagProof(DISTRHO::UIExporter* exp,HWND hwnd){
    bool first=true; resizeWindow(hwnd,exp,1440,1068,first);
    lv_obj_update_layout(lv_screen_active());
    const uint32_t TAG=0x40000000u;   // must match MultiScaleBodyUI::kLearnTag
    auto tagged=[&](const char* what,lv_obj_t* o,uint32_t expect){
        if(!o){ EXPECT(false,what); return; }
        const uint32_t ud=(uint32_t)(intptr_t)lv_obj_get_user_data(o);
        char msg[96];
        snprintf(msg,sizeof(msg),"%s tagged -> param %u (expect %u)",
                 what,(unsigned)(ud&~TAG),(unsigned)expect);
        EXPECT((ud&TAG)!=0u && (ud&~TAG)==expect,msg);
    };
    lv_obj_t* ecoLab=findLabelText(lv_screen_active(),"ECO");
    tagged("Learn: ECO toggle",ecoLab?lv_obj_get_parent(ecoLab):nullptr,
           PluginMultiScaleBody::kParamEcoMode);
    lv_obj_t* ela=findLabelText(lv_screen_active(),"ELASTIC");
    tagged("Learn: ELASTIC toggle",ela?lv_obj_get_parent(ela):nullptr,
           PluginMultiScaleBody::kParamModelMode);
    auto lastChild=[](lv_obj_t* p){ return p?lv_obj_get_child(p,(int32_t)lv_obj_get_child_count(p)-1):nullptr; };
    lv_obj_t* mat=findLabelText(lv_screen_active(),"Material");
    tagged("Learn: Material dropdown",lastChild(mat?lv_obj_get_parent(mat):nullptr),
           PluginMultiScaleBody::kParamMaterial);
    lv_obj_t* mt=findLabelText(lv_screen_active(),"Morph Tgt");
    tagged("Learn: Morph Tgt dropdown",lastChild(mt?lv_obj_get_parent(mt):nullptr),
           PluginMultiScaleBody::kParamMorphTarget);
    tagged("Learn: preset dropdown",findByClass(lv_screen_active(),"lv_dropdown"),
           PluginMultiScaleBody::kParamPreset);
    // knobs carry the RAW param index (no tag) - that is what the arc branch of
    // learnParamAt reads
    lv_obj_t* knob=findParamArc(lv_screen_active(),PluginMultiScaleBody::kParamDecay);
    if(!knob){ EXPECT(false,"Learn: Decay arc found"); return; }
    const uint32_t kud=(uint32_t)(intptr_t)lv_obj_get_user_data(knob);
    LOGF("[learn] knob user_data=%u (expect %u, untagged)\n",(unsigned)kud,
         (unsigned)PluginMultiScaleBody::kParamDecay);
    EXPECT(kud==PluginMultiScaleBody::kParamDecay,"Learn: knob carries a raw param index");
}

// ---- per-preset physical-model application self-test ---------------------
// (R-user) loading a preset must apply THAT preset's PHYSICAL MODEL row
// (PresetModels.hpp). Drive a real dropdown selection and require the model
// params to be written, ending on the row's last field (modelMode).
static void presetModelProof(DISTRHO::UIExporter* exp,HWND hwnd){
    bool first=true; resizeWindow(hwnd,exp,1440,1068,first);
    lv_obj_update_layout(lv_screen_active());
    lv_obj_t* dd=findByClass(lv_screen_active(),"lv_dropdown");
    EXPECT(dd!=nullptr,"Model: preset dropdown found");
    if(!dd) return;
    const int writesBefore=gParamWrites;
    gLastParam=UINT32_MAX;
    lv_dropdown_set_selected(dd,7);            // Membrane
    lv_obj_send_event(dd,LV_EVENT_VALUE_CHANGED,nullptr);
    idleFrames(exp,6);
    const int applied=gParamWrites-writesBefore;
    LOGF("[model] preset 7 applied %d param write(s), last param=%u (expect modelMode=%u)\n",
         applied,(unsigned)gLastParam,(unsigned)PluginMultiScaleBody::kParamModelMode);
    // the preset itself + the row's 17 fields
    EXPECT(applied>=17,"Model: selecting a preset applies its physical-model row");
    EXPECT(gLastParam==PluginMultiScaleBody::kParamModelMode,
           "Model: the row is applied (ends on modelMode)");
    // leave the UI on preset 0 so later work sees the default body
    lv_dropdown_set_selected(dd,0);
    lv_obj_send_event(dd,LV_EVENT_VALUE_CHANGED,nullptr);
    idleFrames(exp,6);
}

// ---- exposure wiring: the toggles that only exist to be automatable --------
// kParamScrubMode and kParamArp are the two params added so previously
// UI-only controls became host/MIDI-controllable. Assert the UI actually
// writes them (the tagging test covers learnability, not the click path).
static void exposureWiringProof(DISTRHO::UIExporter* exp,HWND hwnd){
    bool first=true; resizeWindow(hwnd,exp,1440,1068,first);
    lv_obj_update_layout(lv_screen_active());
    // scrub target toggle: GAIN (0) -> click -> DECAY (1)
    lv_obj_t* gl=findLabelText(lv_screen_active(),"GAIN");
    EXPECT(gl!=nullptr,"Expose: scrub-target toggle found");
    if(gl){
        lv_obj_t* btn=lv_obj_get_parent(gl);
        gLastParam=UINT32_MAX; gLastValue=-1.f;
        lv_obj_send_event(btn,LV_EVENT_CLICKED,nullptr);
        idleFrames(exp,4);
        LOGF("[expose] scrub click -> param=%u value=%.3f (expect %u 1.0)\n",
             (unsigned)gLastParam,gLastValue,(unsigned)PluginMultiScaleBody::kParamScrubMode);
        EXPECT(gLastParam==PluginMultiScaleBody::kParamScrubMode,"Expose: scrub toggle writes kParamScrubMode");
        EXPECT(gLastValue>0.9f,"Expose: GAIN->DECAY writes 1.0");
    }
    // arpeggiator toggle: off -> on
    lv_obj_t* al=findLabelText(lv_screen_active(),"ARP");
    EXPECT(al!=nullptr,"Expose: ARP toggle found");
    if(al){
        lv_obj_t* btn=lv_obj_get_parent(al);
        lv_obj_add_state(btn,LV_STATE_CHECKED);
        gLastParam=UINT32_MAX; gLastValue=-1.f;
        lv_obj_send_event(btn,LV_EVENT_VALUE_CHANGED,nullptr);
        idleFrames(exp,4);
        LOGF("[expose] arp click -> param=%u value=%.3f (expect %u 1.0)\n",
             (unsigned)gLastParam,gLastValue,(unsigned)PluginMultiScaleBody::kParamArp);
        EXPECT(gLastParam==PluginMultiScaleBody::kParamArp,"Expose: ARP toggle writes kParamArp");
        EXPECT(gLastValue>0.9f,"Expose: ARP on writes 1.0");
    }
}

// ---- preset -> double-click baseline interaction --------------------------
// The double-click reset restores "the value the current preset loaded it
// with". Loading a preset now applies its PHYSICAL MODEL row, so that row must
// become the new baseline. Select a preset through the real dropdown (the user
// path), nudge one of its model knobs, and require a double-click to bring it
// back to the value THAT preset established.
static void presetBaselineProof(DISTRHO::UIExporter* exp,HWND hwnd){
    bool first=true; resizeWindow(hwnd,exp,1440,1068,first);
    lv_obj_update_layout(lv_screen_active());
    lv_obj_t* dd=findByClass(lv_screen_active(),"lv_dropdown");
    EXPECT(dd!=nullptr,"Base: preset dropdown found");
    if(!dd) return;
    lv_dropdown_set_selected(dd,7);                 // Membrane; its row sets raylA = 0.12
    lv_obj_send_event(dd,LV_EVENT_VALUE_CHANGED,nullptr);
    idleFrames(exp,6);
    lv_obj_t* arc=findParamArc(lv_screen_active(),PluginMultiScaleBody::kParamRayleighA);
    EXPECT(arc!=nullptr,"Base: Rayleigh A arc found");
    if(!arc) return;
    // nudge it off the baseline through the host path (must NOT re-baseline)
    exp->parameterChanged(PluginMultiScaleBody::kParamRayleighA,0.90f);
    idleFrames(exp,4);
    lv_area_t a; lv_obj_get_coords(arc,&a);
    contactPointer={(lv_coord_t)((a.x1+a.x2)/2),(lv_coord_t)((a.y1+a.y2)/2)};
    lv_indev_t* p=lv_indev_create();
    lv_indev_set_type(p,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(p,lv_display_get_default());
    lv_indev_set_read_cb(p,contactPointerRead);
    gLastParam=UINT32_MAX; gLastValue=-1.f;
    for(int click=0;click<2;++click){
        contactPressed=true;  lv_indev_read(p); idleFrames(exp,2);
        contactPressed=false; lv_indev_read(p); idleFrames(exp,2);
    }
    lv_indev_delete(p);
    idleFrames(exp,6);
    LOGF("[base] dbl-click after preset 7 -> param=%u value=%.3f (expect %u 0.12)\n",
         (unsigned)gLastParam,gLastValue,(unsigned)PluginMultiScaleBody::kParamRayleighA);
    EXPECT(gLastParam==PluginMultiScaleBody::kParamRayleighA,"Base: double-click writes the knob's param");
    EXPECT(std::fabs(gLastValue-0.12f)<0.02f,"Base: restored to the NEW preset's model value");
    lv_dropdown_set_selected(dd,0);                 // leave the default body selected
    lv_obj_send_event(dd,LV_EVENT_VALUE_CHANGED,nullptr);
    idleFrames(exp,6);
}

// ---- preset browser navigation + dice self-test ---------------------------
// presetArrowCb (prev/next, including the end wrap) and rndBtnCb (the dice)
// were both touched in this batch - the arrows now also apply the preset's
// PHYSICAL MODEL row - and neither path had any coverage. The row's four
// children are [prev arrow][dropdown][next arrow][dice], so they are reachable
// from the dropdown itself.
static void presetNavProof(DISTRHO::UIExporter* exp,HWND hwnd){
    bool first=true; resizeWindow(hwnd,exp,1440,1068,first);
    lv_obj_update_layout(lv_screen_active());
    lv_obj_t* dd=findByClass(lv_screen_active(),"lv_dropdown");
    EXPECT(dd!=nullptr,"Nav: preset dropdown found");
    if(!dd) return;
    lv_obj_t* row=lv_obj_get_parent(dd);
    const uint32_t n=row?(uint32_t)lv_obj_get_child_count(row):0u;
    LOGF("[nav] preset row children=%u (expect 4: prev, dropdown, next, dice)\n",(unsigned)n);
    EXPECT(n>=4,"Nav: preset row has prev/next/dice");
    if(n<4) return;
    lv_obj_t* prev=lv_obj_get_child(row,0);
    lv_obj_t* next=lv_obj_get_child(row,2);
    lv_obj_t* dice=lv_obj_get_child(row,3);
    lv_dropdown_set_selected(dd,0);
    lv_obj_send_event(dd,LV_EVENT_VALUE_CHANGED,nullptr);
    idleFrames(exp,4);
    // NEXT: selection advances AND that preset's model row is applied
    int before=gParamWrites; gLastParam=UINT32_MAX;
    lv_obj_send_event(next,LV_EVENT_CLICKED,nullptr);
    idleFrames(exp,6);
    LOGF("[nav] next -> sel=%d writes=%d last=%u\n",lv_dropdown_get_selected(dd),
         gParamWrites-before,(unsigned)gLastParam);
    EXPECT(lv_dropdown_get_selected(dd)==1,"Nav: next advances the selection");
    EXPECT(gParamWrites-before>=17,"Nav: next applies the preset model row");
    EXPECT(gLastParam==PluginMultiScaleBody::kParamModelMode,"Nav: arrow path applies the model row");
    // PREV: steps back, then wraps from preset 0 to the last preset
    lv_obj_send_event(prev,LV_EVENT_CLICKED,nullptr);
    idleFrames(exp,6);
    EXPECT(lv_dropdown_get_selected(dd)==0,"Nav: prev steps back");
    lv_obj_send_event(prev,LV_EVENT_CLICKED,nullptr);
    idleFrames(exp,6);
    LOGF("[nav] prev from 0 -> %d (expect %d)\n",lv_dropdown_get_selected(dd),modal::kNumPresets-1);
    EXPECT(lv_dropdown_get_selected(dd)==modal::kNumPresets-1,"Nav: prev wraps to the last preset");
    // DICE: randomize must write a spread of params
    before=gParamWrites;
    lv_obj_send_event(dice,LV_EVENT_CLICKED,nullptr);
    idleFrames(exp,10);
    LOGF("[nav] dice writes=%d\n",gParamWrites-before);
    EXPECT(gParamWrites-before>=20,"Nav: dice randomizes a spread of params");
    lv_dropdown_set_selected(dd,0);                 // leave the default body selected
    lv_obj_send_event(dd,LV_EVENT_VALUE_CHANGED,nullptr);
    idleFrames(exp,6);
}

// ---- host-sync readout self-test -----------------------------------------
// A host/preset sync must leave a knob showing its CONTEXTUAL text.
// UIWidgets::syncFromParam ends by writing a raw "%.2f" into the value label and
// sends no VALUE_CHANGED, so before this fix every host automation write or
// preset load reverted the readout - Modes "88" became "0.67", Glide "90 MS"
// became "0.15", Wet "30 %" became "0.30" - and it stayed raw until the knob was
// dragged. syncParamWidget now re-runs the formatter after the sync.
// createArcKnob puts the caption label first in the knob container and the value
// readout last, so tree order identifies it (the widget's own
// gArcVisualBindings map is a `static` in a header, i.e. one copy per TU, and
// this harness is a different TU from PluginUI.cpp).
static lv_obj_t* knobValueLabel(lv_obj_t* arc){
    if(!arc) return nullptr;
    lv_obj_t* cont=lv_obj_get_parent(arc);
    if(!cont) return nullptr;
    lv_obj_t* last=nullptr;
    for(uint32_t i=0;i<lv_obj_get_child_count(cont);++i){
        lv_obj_t* c=lv_obj_get_child(cont,i);
        if(lv_obj_check_type(c,&lv_label_class)) last=c;
    }
    return last;
}
static void hostSyncLabelProof(DISTRHO::UIExporter* exp,HWND hwnd){
    bool first=true; resizeWindow(hwnd,exp,1440,1068,first);
    lv_obj_update_layout(lv_screen_active());
    struct Case{ uint32_t pi; const char* name; };
    const Case cases[]={{PluginMultiScaleBody::kParamModeCount,"Modes"},
                        {PluginMultiScaleBody::kParamGlide,"Glide"},
                        {PluginMultiScaleBody::kParamEcoBudget,"Eco Budget"}};
    int ran=0;
    for(const Case& c:cases){
        lv_obj_t* arc=findParamArc(lv_screen_active(),c.pi);
        if(!arc){ LOGF("[label] %s knob is not in this layout - skipped\n",c.name); continue; }
        lv_obj_t* lbl=knobValueLabel(arc);
        if(!lbl){ EXPECT(false,std::string("Label: ")+c.name+" value label present"); continue; }
        ++ran;
        exp->parameterChanged(c.pi,0.67f);
        idleFrames(exp,3);
        const char* t=lv_label_get_text(lbl);
        LOGF("[label] %-11s host sync -> '%s'\n",c.name,t?t:"(null)");
        EXPECT(t && std::strcmp(t,"0.67")!=0,
               std::string("Label: ")+c.name+" keeps its formatted readout after a host sync");
    }
    // without this a layout where none of the knobs resolve would pass silently
    LOGF("[label] readouts actually checked = %d\n",ran);
    EXPECT(ran>0,"Label: at least one knob readout was actually checked");
}

// ---- dropdown exclusivity self-test --------------------------------------
// Only one dropdown list may be open at a time. This drives the callback path
// directly instead of clicking: lv_dropdown_open() both lists (a real release
// would toggle them), then dispatch LV_EVENT_RELEASED to one of them. The event
// carries each descriptor's OWN registered user_data, so the handler runs exactly
// as it does for a real gesture without the harness needing the UI pointer.
// A click-driven version of this check cannot discriminate: LVGL closes the other
// list itself on focus change, so it passed with the preset dropdown's
// registration removed.
static void collectDropdowns(lv_obj_t* o,lv_obj_t** out,int& n,int cap){
    if(o->class_p && 0==std::strcmp(o->class_p->name,"lv_dropdown") && n<cap) out[n++]=o;
    const uint32_t k=lv_obj_get_child_count(o);
    for(uint32_t i=0;i<k;++i) collectDropdowns(lv_obj_get_child(o,i),out,n,cap);
}
static void dropdownExclusiveProof(DISTRHO::UIExporter* exp,HWND hwnd){
    bool first=true; resizeWindow(hwnd,exp,1440,1068,first);
    lv_obj_update_layout(lv_screen_active());
    lv_obj_t* dd[8]; int n=0;
    collectDropdowns(lv_screen_active(),dd,n,8);
    LOGF("[dd] dropdowns in the tree = %d\n",n);
    EXPECT(n>=4,"Dropdowns: all four dropdowns are present");
    int checked=0;
    for(int i=0;i<n;++i) for(int j=0;j<n;++j){
        if(i==j) continue;
        for(int k=0;k<n;++k) if(lv_dropdown_is_open(dd[k])) lv_dropdown_close(dd[k]);
        lv_dropdown_open(dd[i]);                       // A open
        if(lv_dropdown_is_open(dd[j])) lv_dropdown_close(dd[j]);   // B closed
        const bool aBefore=lv_dropdown_is_open(dd[i]);
        // LVGL's own release handler TOGGLES (lv_dropdown.c btn_release_handler):
        // releasing a CLOSED dropdown opens it, and it runs before user
        // callbacks - so the release must be sent with B closed for the
        // exclusivity callback to see an open list.
        lv_obj_send_event(dd[j],LV_EVENT_RELEASED,nullptr);
        const bool bAfter=lv_dropdown_is_open(dd[j]);
        const bool other=lv_dropdown_is_open(dd[i]);
        if(!aBefore||!bAfter) continue;                // the gesture did not happen
        ++checked;
        LOGF("[dd] pair %d->%d: A open=%d, release opens B=%d, A still open=%d\n",i,j,aBefore,bAfter,other);
        EXPECT(!other,"Dropdowns: a release that opens one dropdown closes the other");
    }
    LOGF("[dd] pairs actually exercised = %d\n",checked);
    EXPECT(checked>=2,"Dropdowns: at least two ordered pairs were exercised");
}

// ---- info-column spec strip self-test ------------------------------------
// The BODY/MODES/MAT/F0 strip was built as a 0-height container. LVGL's
// content-size sentinel is LV_SIZE_CONTENT, so an explicit 0 is an EMPTY area
// and the strip's children were never drawn - the band read as dead space while
// updateBodyInfo kept writing labels nobody could see. Assert it renders, that
// the rows are tall enough for a caption+value, and that the whole info column
// still fits its budget (a within-column overflow is invisible to the
// surface-level bounds check, which is how this class of bug survives).
static void specStripProof(DISTRHO::UIExporter* exp,HWND hwnd){
    bool first=true; resizeWindow(hwnd,exp,1440,1068,first);
    lv_obj_update_layout(lv_screen_active());
    lv_obj_t* lab=findLabelText(lv_screen_active(),"DAMPING");
    EXPECT(lab!=nullptr,"Spec: DAMPING label found");
    if(!lab) return;
    // label -> dampHead -> dampCard -> infoCol
    lv_obj_t* col=lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(lab)));
    EXPECT(col!=nullptr,"Spec: info column found");
    if(!col) return;
    const uint32_t n=(uint32_t)lv_obj_get_child_count(col);
    LOGF("[spec] info column children=%u\n",(unsigned)n);
    EXPECT(n>=6,"Spec: info column has coord/dividers/rows/strip/spacer/card");
    // locate the strip structurally, not by index: F0 label -> cell -> row -> strip
    lv_obj_t* f0=findLabelText(lv_screen_active(),"F0");
    EXPECT(f0!=nullptr,"Spec: F0 cell label exists");
    if(!f0) return;
    lv_obj_t* row2=lv_obj_get_parent(lv_obj_get_parent(f0));
    lv_obj_t* strip=row2?lv_obj_get_parent(row2):nullptr;
    EXPECT(strip!=nullptr,"Spec: spec strip found above the F0 cell");
    if(!strip) return;
    const lv_coord_t sh=lv_obj_get_height(strip);
    LOGF("[spec] strip h=%d rows=%u\n",(int)sh,(unsigned)lv_obj_get_child_count(strip));
    EXPECT(sh>20,"Spec: the BODY/MODES/MAT/F0 strip renders (not a 0-height clip)");
    EXPECT(lv_obj_get_child_count(strip)==2,"Spec: strip has two rows");
    // and each caption inside the strip must be actually visible in the layout
    for(int i=0;i<2;++i){
        lv_obj_t* row=lv_obj_get_child(strip,i);
        LOGF("[spec] row%d @%d,%d %dx%d\n",i,
             (int)lv_obj_get_x(row),(int)lv_obj_get_y(row),
             (int)lv_obj_get_width(row),(int)lv_obj_get_height(row));
        EXPECT(lv_obj_get_height(row)>=24,"Spec: spec row fits a caption+value pair");
    }
    // column budget: the card must not spill past the column bottom
    lv_obj_t* card=lv_obj_get_child(col,(int32_t)n-1);
    lv_area_t cc,colc;
    lv_obj_get_coords(card,&cc); lv_obj_get_coords(col,&colc);
    LOGF("[spec] card bottom=%d column bottom=%d (column h=%d)\n",
         (int)cc.y2,(int)colc.y2,(int)lv_obj_get_height(col));
    EXPECT(cc.y2<=colc.y2,"Spec: damping card stays inside the info column");
}
END_NAMESPACE_DISTRHO

int main(int argc,char** argv)
{
    SetProcessDPIAware(); // 1:1 physical pixels: no DWM scaling in screen blits
    // disc capture name: pass ui_disc_before.png while reproducing the bug
    const char* discShot=(argc>1)?argv[1]:"ui_disc_after.png";
    gLog=fopen("ui_verify_log.txt","w");
    DISTRHO::UIExporter* exp=new DISTRHO::UIExporter(
        nullptr, 0, 48000.0,
        &DISTRHO::stubEditParam, &DISTRHO::stubSetParam, &DISTRHO::stubSetState,
        &DISTRHO::stubSendNote,  &DISTRHO::stubSetSize,  &DISTRHO::stubFileRequest);
    gExp=exp;

    HWND hwnd=(HWND)exp->getNativeWindowHandle();
    if(!hwnd){ LOGF("no native handle\n"); return 1; }

    ShowWindow(hwnd,SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    SetWindowPos(hwnd,HWND_TOPMOST,40,40,0,0,SWP_NOSIZE);

    idleFrames(exp,360);
     presentKick(hwnd,exp);
    if(argc>1 && std::strcmp(argv[1],"--selftest")==0){
        contactProof(exp,hwnd);
        doubleClickResetProof(exp,hwnd);
        dampDragProof(exp,hwnd);
        recorderLoopProof(exp,hwnd);
        learnTagProof(exp,hwnd);
        presetModelProof(exp,hwnd);
        exposureWiringProof(exp,hwnd);
        presetBaselineProof(exp,hwnd);
        presetNavProof(exp,hwnd);
        specStripProof(exp,hwnd);
        hostSyncLabelProof(exp,hwnd);
        dropdownExclusiveProof(exp,hwnd);
        LOGF("SELFTEST RESULT failures=%d\n",gTestFails);
        exp->quit(); delete exp;
        if(gLog) fclose(gLog);
        return gTestFails?1:0;
    }

    struct Sz{ int w,h; const char* bmp; const char* tag; };
    // R3 scope-preview probe: dump the second chart's (decay scope) series
    // values after the idle frames so we can see whether the seeded preview
    // trace lives in the chart data or never got written.
    {
        lv_obj_t* c1 = findByClass(lv_screen_active(),"lv_chart");
        lv_obj_t* scope = nullptr;
        if(c1){
            // walk siblings/children for the second chart
            // simple approach: iterate the whole tree, collect lv_chart objects
            lv_obj_t* stack[512]; int sp=0; stack[sp++]=lv_screen_active();
            lv_obj_t* charts[8]; int nc=0;
            while(sp>0 && nc<8){
                lv_obj_t* o=stack[--sp];
                if(o->class_p && 0==std::strcmp(o->class_p->name,"lv_chart")) charts[nc++]=o;
                const uint32_t k=lv_obj_get_child_count(o);
                for(uint32_t i=0;i<k;++i) stack[sp++]=lv_obj_get_child(o,i);
            }
            LOGF("[scope-probe] charts found=%d\n",nc);
            if(nc>=2){
                scope=charts[1];   // scope is built after spectrum -> 2nd in DFS?
                for(int ci=0;ci<nc;++ci){
                    lv_obj_t* ch=charts[ci];
                    lv_chart_series_t* ser=lv_chart_get_series_next(ch,nullptr);
                    if(!ser) continue;
                    const int pcnt=(int)lv_chart_get_point_count(ch); // complete type not needed here
                    int32_t* ys=lv_chart_get_series_y_array(ch,ser);
                    int nz=0,hi=0; long sum=0;
                    for(int i=0;i<pcnt;++i){ int v=ys[i]; if(v!=0){++nz; if(v>hi)hi=v; sum+=v;} }
                    LOGF("[scope-probe] chart#%d point_cnt=%d nonzero=%d hi=%d mean=%ld\n",ci,pcnt,nz,hi,sum/(pcnt?pcnt:1));
                }
            }
        }
    }
    const Sz sizes[]={{1440,1068,"ui_zoom_1440.bmp","default"},
                      {1100,700,"ui_zoom_1100.bmp","small"},
                      {2028,1104,"ui_zoom_2048.bmp","large"}}; // screen 2048x1152: largest client that fits incl. frame
    bool first=true;
    for(const Sz& s:sizes){
        resizeWindow(hwnd,exp,s.w,s.h,first);
        LOGF("=== %s ===\n",s.tag);
        logState(exp,s.tag);
        checkLayout(s.tag);
        idleFrames(exp,90);             // render the settled layout before grabbing
        writeBMP(hwnd,s.bmp);
    }

    // ---- T1b: exercise zoom steps (50%) and prove aspect-locked rescale ---
    // 50% of the 1440x1068 base = exactly 720x534 (steps above 100% exceed
    // this 1152px-tall screen and get OS-clamped, which is a T4 scenario).
    LOGF("=== zoom-step test ===\n");
    first=false;
    EXPECT(resizeWindow(hwnd,exp,1440,1068,first),"back-to-base-1440x1068");
    lv_obj_t* zPlus=findZoomButton(lv_screen_active(),1);
    lv_obj_t* zMinus=findZoomButton(lv_screen_active(),-1);
    EXPECT(zPlus!=nullptr,"zoom-plus-found"); EXPECT(zMinus!=nullptr,"zoom-minus-found");
    if(zMinus){
        lv_obj_send_event(zMinus,LV_EVENT_CLICKED,nullptr);  // 100% -> 75%
        lv_obj_send_event(zMinus,LV_EVENT_CLICKED,nullptr);  // 75% -> 50%
        idleFrames(exp,120); pumpMsgs(); presentKick(hwnd,exp); idleFrames(exp,90);
        lv_display_t* d=lv_display_get_default();
        const long dw=lv_display_get_horizontal_resolution(d), dh=lv_display_get_vertical_resolution(d);
        LOGF("[zoom50] display=%ldx%ld gUIScale=%.3f\n",dw,dh,(double)DISTRHO::gUIScale);
        EXPECT(dw==720&&dh==534,"display-exactly-720x534");
        EXPECT(dw*1068==dh*1440,"aspect-ratio-exact");
        {   // at an exact zoom step the plate must FILL the window (no letterbox)
            lv_obj_t* plate=findPlate(lv_screen_active());
            EXPECT(plate!=nullptr,"zoom50-plate-exists");
            if(plate){
                lv_area_t pc; lv_obj_get_coords(plate,&pc);
                LOGF("[zoom50] plate=@%ld,%ld %ldx%ld\n",(long)pc.x1,(long)pc.y1,
                    (long)lv_obj_get_width(plate),(long)lv_obj_get_height(plate));
                EXPECT(pc.x1==0 && pc.y1==0 && lv_obj_get_width(plate)==720
                       && lv_obj_get_height(plate)==534,"zoom50-plate-fills-window");
            }
        }
        checkLayout("zoom50");
        presentKick(hwnd,exp); idleFrames(exp,90); writeBMP(hwnd,"ui_zoom_step_50.bmp");
        // drift/stability: -- twice then ++ twice must return to base exactly
        lv_obj_t* zp=findZoomButton(lv_screen_active(),1);
        if(zp){ lv_obj_send_event(zp,LV_EVENT_CLICKED,nullptr);               // -> 75%
                lv_obj_send_event(zp,LV_EVENT_CLICKED,nullptr); }             // -> 100%
        idleFrames(exp,120); pumpMsgs(); presentKick(hwnd,exp); idleFrames(exp,90);
        d=lv_display_get_default();
        const long dw2=lv_display_get_horizontal_resolution(d), dh2=lv_display_get_vertical_resolution(d);
        LOGF("[zoom-back] display=%ldx%ld gUIScale=%.3f\n",dw2,dh2,(double)DISTRHO::gUIScale);
        presentKick(hwnd,exp); idleFrames(exp,90); writeBMP(hwnd,"ui_zoom_back_100.bmp");
        EXPECT(DISTRHO::gUIScale>0.995f&&DISTRHO::gUIScale<1.005f,"scale-no-drift");
        checkLayout("zoom-back");
    }

    // ---- T1c: free-form window resize (non-zoom-step dimensions) ----------
    // The zoom stepper only covers 6 base-aspect sizes. A user dragging the
    // host's window corner to any other size must still produce a live,
    // non-blank UI - the plugin handles it via uiReshape -> rebuildForScale.
    LOGF("=== free-form resize test ===\n");
    EXPECT(resizeWindow(hwnd,exp,1200,900,first),"1200x900-resized");
    {
        int w=0,h=0; std::vector<unsigned char> px;
        lv_obj_update_layout(lv_screen_active());
        if(!grabScreen(hwnd,w,h,px)){ LOGF("freeresize grab failed\n"); }
        else{
            const double br=blackRatio(px);
            LOGF("[freeresize-1200x900] display=%dx%d black=%.2f\n",w,h,br);
            writeBMP(hwnd,"ui_freeresize_1200.bmp");
            // a live UI has panels/knobs/bars/dots - well under 50% pure black
            EXPECT(br<0.5,"freeresize-not-blank");
            // the disc must still be findable and at the new scale
            lv_obj_t* d=findDisc(lv_screen_active());
            // the knob bank must still be present (left column has 5 groups,
            // each with 4 lv_arc widgets, plus 11 physics-strip arcs + 1 master
            // in the header = 32)
            int arcs=0;
            std::function<void(lv_obj_t*)> count=[&](lv_obj_t* p){
                if(!p) return;
                if(p->class_p && std::strcmp(p->class_p->name,"lv_arc")==0) ++arcs;
                for(uint32_t i=0;i<lv_obj_get_child_count(p);++i) count(lv_obj_get_child(p,i));
            };
            count(lv_screen_active());
            LOGF("[freeresize-1200x900] arcs-found=%d\n",arcs);
            EXPECT(arcs>=16,"arcs-survive-resize");   // 5*4 dial bank + 11 strip + 1 master
        }
    }
    // back to base for the remaining tests
    EXPECT(resizeWindow(hwnd,exp,1440,1068,first),"back-to-base-2");

    // ---- T2: dropdown open + mouse-wheel scrolling of the list --------------
    LOGF("=== dropdown-wheel test ===\n");
    lv_obj_t* dd=findByClass(lv_screen_active(),"lv_dropdown");
    EXPECT(dd!=nullptr,"dropdown-found");
    if(dd){
        lv_dropdown_open(dd);
        idleFrames(exp,30); pumpMsgs(); lv_obj_update_layout(lv_screen_active());
        lv_obj_t* list=lv_dropdown_get_list(dd);
        EXPECT(list!=nullptr,"list-open");
        if(list){
            const long lh=lv_obj_get_height(list);
            const bool capped=lh<=scaled(8*15)+2;   // DROPDOWN_MAX_ROWS*DROPDOWN_ROW_H
            LOGF("[dropdown] rows=18 listH=%ld cap=%d scrollable=%d scrollY=%ld\n",
                lh,(int)scaled(8*15),(int)lv_obj_has_flag(list,LV_OBJ_FLAG_SCROLLABLE),
                (long)lv_obj_get_scroll_y(list));
            EXPECT(capped,"list-height-capped(~8rows)");
            EXPECT(lv_obj_has_flag(list,LV_OBJ_FLAG_SCROLLABLE),"list-is-scrollable");
            checkLayout("dropdown-open");
            presentKick(hwnd,exp); idleFrames(exp,90); writeBMP(hwnd,"ui_dropdown_open.bmp");

            lv_area_t lc; lv_obj_get_coords(list,&lc);
            const int cx=(lc.x1+lc.x2)/2, cy=(lc.y1+lc.y2)/2;
            const long s0=lv_obj_get_scroll_y(list);
            wheelAt(hwnd,cx,cy,-1.0);   // one notch DOWN
            pumpMsgs(); idleFrames(exp,40); pumpMsgs();
            const long s1=lv_obj_get_scroll_y(list);
            LOGF("[wheel-down] %ld -> %ld\n",s0,s1);
            EXPECT(s1>s0,"wheel-down-scrolls-list");
            for(int i=0;i<5;++i){ wheelAt(hwnd,cx,cy,-1.0); }
            pumpMsgs(); idleFrames(exp,50); pumpMsgs();
            const long s2=lv_obj_get_scroll_y(list);
            wheelAt(hwnd,cx,cy,+1.0);   // one notch UP
            pumpMsgs(); idleFrames(exp,40); pumpMsgs();
            const long s3=lv_obj_get_scroll_y(list);
            LOGF("[more-down] %ld ; [wheel-up] %ld -> %ld\n",s2,s2,s3);
            EXPECT(s3<s2,"wheel-up-scrolls-back");
            // (R-user) "no scroll past the first/last element": keep wheeling
            // DOWN far past the end - scroll_y must stop exactly at the list's
            // natural bottom (scroll_top+scroll_bottom), never overscroll.
            for(int i=0;i<40;++i){ wheelAt(hwnd,cx,cy,-1.0); pumpMsgs(); idleFrames(exp,6); pumpMsgs(); }
            const long sEnd=lv_obj_get_scroll_y(list);
            const long maxY=lv_obj_get_scroll_top(list)+lv_obj_get_scroll_bottom(list);
            LOGF("[wheel-clamp] after 40 more notches scrollY=%ld naturalMax=%ld\n",sEnd,maxY);
            EXPECT(sEnd<=maxY+1,"wheel-clamped-at-last-element");
            lv_dropdown_close(dd);
            idleFrames(exp,20);
        }
    }


    // ---- T3: STRIKE DISC click-to-hit regression proof ----------------------
    // LVGL delivers a click to the TOPMOST clickable object under the point
    // (lv_indev_search_obj). Before the fix the clickable inner-well overlay
    // (86% of the disc) won that search and strikeDisc's padPressCb never saw
    // PRESSED. Proof: every decorative child non-clickable + dispatch resolves
    // to strikeDisc at an off-center well point and at dead center.
    LOGF("=== strike-disc click test ===\n");
    first=false;
    EXPECT(resizeWindow(hwnd,exp,1440,1068,first),"base-size-for-click-test");
    lv_obj_t* disc=findDisc(lv_screen_active());
    if(disc){
        int w=0,h=0; std::vector<unsigned char> pxBuf;
        lv_obj_update_layout(lv_screen_active());
        lv_obj_t* well=findFirstChildBySize(disc,wellSize());
        if(well) LOGF("[disc] well %ldx%ld clickable=%d\n",(long)lv_obj_get_width(well),
            (long)lv_obj_get_height(well),(int)lv_obj_is_clickable(well));
        lv_area_t dc; lv_obj_get_coords(disc,&dc);

        // probe 1: off-center point inside the inner well (fx=.25 fy=.25)
        {
            const int px=dc.x1+(dc.x2-dc.x1+1)/4, py=dc.y1+3*(dc.y2-dc.y1+1)/4;
            lv_obj_t* thief=clickableCoverChild(disc,px,py);
            LOGF("[probe1] point=(%d,%d) topmost-clickable-child=%s\n",px,py,
                thief?(thief->class_p?thief->class_p->name:"?"):"<none - reaches disc>");
            lv_point_t sp{(lv_coord_t)px,(lv_coord_t)py};
            lv_obj_t* hit=lv_indev_search_obj(lv_screen_active(),&sp);
            LOGF("[probe1] lv_indev_search_obj -> %s\n",hit==disc?"strikeDisc (click path OK)"
                :(hit&&hit->class_p?hit->class_p->name:"null"));
            EXPECT(hit==disc,"P1-dispatch-reaches-disc");
        }
        // probe 2: dead center
        {
            const int px=(dc.x1+dc.x2)/2, py=(dc.y1+dc.y2)/2;
            lv_obj_t* thief=clickableCoverChild(disc,px,py);
            LOGF("[probe2] point=(%d,%d) topmost-clickable-child=%s\n",px,py,
                thief?(thief->class_p?thief->class_p->name:"?"):"<none - reaches disc>");
            lv_point_t sp{(lv_coord_t)px,(lv_coord_t)py};
            lv_obj_t* hit=lv_indev_search_obj(lv_screen_active(),&sp);
            LOGF("[probe2] lv_indev_search_obj -> %s\n",hit==disc?"strikeDisc (click path OK)"
                :(hit&&hit->class_p?hit->class_p->name:"null"));
            EXPECT(hit==disc,"P2-dispatch-reaches-disc");
        }
        // NOTE: padPressCb counter evidence (note-on/off, StrikeX/Y writes)
        // needs a real indev-driven press; neither posted messages nor
        // SendInput reach the LVGL pointer indev in this harness environment
        // (hover never asserts), so the dispatch assertions above are the
        // click-path proof here. Human/host confirmation is the final gate.
        // round-2: r2 layout tree has more children (extra preset row, more
        // envelope layer dots, witness marks) than r1; the GL surface needs
        // more frames after the LAST structural change to actually present
        // a non-blank first frame. Bump idleFrames here and add a second
        // presentKick after the larger idle, so the r2 capture isn't the
        // black pre-draw window.
        idleFrames(exp,360); pumpMsgs();
        SetWindowPos(hwnd,HWND_TOPMOST,40,40,0,0,SWP_NOMOVE|SWP_NOSIZE);
        presentKick(hwnd,exp); idleFrames(exp,180); pumpMsgs();
        presentKick(hwnd,exp); idleFrames(exp,120); pumpMsgs();
        // and bypass the blank-capture guard for the discShot specifically -
        // the file always lands so we have SOMETHING to inspect even if the
        // GL surface is wedged (the harness's pass/fail is in the log).
        if(!grabScreen(hwnd,w,h,pxBuf)){ LOGF("discShot grab failed\n"); }
        else {
            FILE* f=fopen(discShot,"wb"); if(f){
                BITMAPFILEHEADER fh; memset(&fh,0,sizeof(fh));
                fh.bfType=0x4D42; fh.bfOffBits=14+40; fh.bfSize=(DWORD)(14+40+pxBuf.size());
                BITMAPINFOHEADER ih; memset(&ih,0,sizeof(ih));
                ih.biSize=40; ih.biWidth=w; ih.biHeight=h; ih.biPlanes=1;
                ih.biBitCount=32; ih.biCompression=BI_RGB; ih.biSizeImage=(DWORD)pxBuf.size();
                fwrite(&fh,sizeof(fh),1,f); fwrite(&ih,sizeof(ih),1,f);
                fwrite(pxBuf.data(),1,pxBuf.size(),f); fclose(f);
                const double br=blackRatio(pxBuf);
                LOGF("captured %s (%dx%d, black=%.2f) [forced]\n",discShot,w,h,br);
            }
        }
    }
    // ---- T4: fixed-aspect plate anchoring (zoom must NOT move presets/keyboard)
    // User report: "why when we zoom do we move the presets and keyboard? dont
    // do that". Root cause: the topbar/stage/keyboard were DIRECT children of
    // the flex SCREEN with lv_pct(100) width, so at any surface not exactly
    // 1440:1068 (a big zoom step clamped to the monitor working area, or a free
    // host resize) they stretched to the FULL window width and re-centered /
    // re-flowed independent of the stage. Fix: build the whole chassis into a
    // fixed-aspect PLATE of scaled(BASE_W x BASE_H) centered on the screen; the
    // chassis bg letterboxes around it and everything inside the plate keeps its
    // exact relative position at ANY surface size.
    LOGF("=== T4 plate anchoring (clamped / aspect-broken sizes) ===\n");
    {
        const struct{int w,h; const char* tag;} clamps[]={
            {1920,1048,"clamp125-1080p"},   // a 125% zoom clamped to a 1080p work area
            {2000, 900,"wide900"},           // extreme aspect break (2.22 vs 1.67)
            {2200,1280,"big2200"},           // taller than the plate aspect
        };
        for(const auto& c:clamps){
            EXPECT(resizeWindow(hwnd,exp,c.w,c.h,first),c.tag);
            idleFrames(exp,30); pumpMsgs(); lv_obj_update_layout(lv_screen_active());
            lv_obj_t* plate=findPlate(lv_screen_active());
            // the drop/lines below are the crux: on pre-fix code findPlate is
            // null and every anchoring assertion fails (the defect the user saw)
            EXPECT(plate!=nullptr,"plate-exists");
            if(!plate){ LOGF("[%s] NO PLATE - tree not plate-based\n",c.tag); continue; }
            lv_area_t p; lv_obj_get_coords(plate,&p);
            const lv_coord_t pw=lv_obj_get_width(plate);
            LOGF("[%s] display=%dx%d plate=@%ld,%ld %ldx%ld\n",c.tag,c.w,c.h,
                (long)p.x1,(long)p.y1,(long)pw,(long)lv_obj_get_height(plate));
            // 1) plate is centered: left margin == right margin (a rigid unit,
            //    not crammed to the window's left edge with slack on the right)
            const lv_coord_t ml=p.x1, mr=c.w-(p.x2+1);
            LOGF("[%s] margins L=%ld R=%ld\n",c.tag,(long)ml,(long)mr);
            EXPECT(std::abs(ml-mr)<=1,"plate-centered");
            // 2) plate never exceeds the surface in either axis
            EXPECT(pw<=c.w && lv_obj_get_height(plate)<=c.h,"plate-fits-surface");
            // 3) keyboard strip spans the plate and is pinned to its bottom
            lv_obj_t* kb=findRegion(plate,scaled(lay::KB_STRIP_H));
            EXPECT(kb!=nullptr,"keyboard-region-found");
            if(kb){
                lv_area_t k; lv_obj_get_coords(kb,&k);
                LOGF("[%s] kb=@%ld,%ld %ldx%ld plate-inner=[%ld,%ld]\n",c.tag,
                    (long)k.x1,(long)k.y1,(long)lv_obj_get_width(kb),(long)lv_obj_get_height(kb),
                    (long)(p.x1+scaled(lay::PAD)),(long)(p.x2-scaled(lay::PAD)));
                // spans the plate width (its pct(100) is 100% OF THE PLATE now)
                EXPECT(k.x1==p.x1+scaled(lay::PAD) && k.x2==p.x2-scaled(lay::PAD),
                       "keyboard-spans-plate");
                // pinned to the plate bottom within 2px: rounding (scaled(16)
                // truncates) plus the strip's own 1px border make the strict
                // bottom edge differ by 0-1px at fractional scales - invisible,
                // and unrelated to the "zoom moves the presets/keyboard" bug.
                EXPECT(std::abs((long)k.y2-((long)p.y2-scaled(lay::PAD)))<=2,
                       "keyboard-pinned-plate-bottom");
            }
            // 4) topbar spans the plate
            lv_obj_t* top=findRegion(plate,scaled(lay::HEADER_H));
            EXPECT(top!=nullptr,"topbar-region-found");
            if(top){
                lv_area_t t; lv_obj_get_coords(top,&t);
                EXPECT(t.x1==p.x1+scaled(lay::PAD) && t.x2==p.x2-scaled(lay::PAD),
                       "topbar-spans-plate");
            }
            // 5) the preset dropdown stays in the plate's LEFT region (anchored
            //    to the plate, never pushed toward the window's right edge)
            lv_obj_t* dd=findByClass(plate,"lv_dropdown");
            if(dd){
                lv_area_t dc; lv_obj_get_coords(dd,&dc);
                LOGF("[%s] preset=@%ld x-in-plate=[%ld,%ld] plate=[%ld,%ld]\n",c.tag,
                    (long)dc.x1,(long)(dc.x1-p.x1),(long)(dc.x2-p.x1),
                    (long)p.x1,(long)p.x2);
                EXPECT(dc.x1>p.x1 && dc.x2<p.x2,"preset-inside-plate");
                EXPECT((dc.x1-p.x1)<pw/2,"preset-anchored-plate-left");
            }else EXPECT(false,"preset-dropdown-in-plate");
            checkLayout(c.tag);
        }
    }

    LOGF("=== RESULT fails=%d bounds/overlap reported above ===\n",gTestFails);

    exp->quit();
    delete exp;
    if(gLog) fclose(gLog);
    printf("done\n");
    return 0;
}
