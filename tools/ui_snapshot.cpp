/*
 * ui_snapshot.cpp — UI-only visual verification harness (no host, no audio).
 * Drives the real PluginUI through DPF's UIExporter, shows the native window,
 * idles the DGL/LVGL loop, and captures the surface to BMP at three sizes
 * (default 1440x860 / small 1100x700 / large 2048x1200).
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
#include "ui/UICommon.hpp"
#include <windows.h>

static FILE* gLog=nullptr;
#define LOGF(...) do{ if(gLog){ fprintf(gLog,__VA_ARGS__); fflush(gLog);} printf(__VA_ARGS__); }while(0)

START_NAMESPACE_DISTRHO

// interaction-proof counters: every UI->plugin route lands in one of these
static int gNoteOns=0,gNoteOffs=0,gParamWrites=0,gStrikeWrites=0,gPresetWrites=0,gBandWrites=0;
static void stubEditParam(void*, uint32_t, bool) {}
static void stubSetParam(void*, uint32_t i, float){
    ++gParamWrites;
    if(i==PluginMultiScaleBody::kParamStrikeX||i==PluginMultiScaleBody::kParamStrikeY) ++gStrikeWrites;
    else if(i==PluginMultiScaleBody::kParamPreset) ++gPresetWrites;
    else if(i>=PluginMultiScaleBody::kParamBand0&&i<PluginMultiScaleBody::kParamBand0+16) ++gBandWrites;
}
static void stubSetState(void*, const char*, const char*) {}
static void stubSendNote(void*, uint8_t, uint8_t, uint8_t v){ if(v) ++gNoteOns; else ++gNoteOffs; }
static void stubSetSize(void*, uint, uint) {}
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
    LOGF("%*s%s @ %ld,%ld %ldx%ld\n",depth*2,"",
        obj->class_p?obj->class_p->name:"?",
        (long)c.x1,(long)c.y1,(long)lv_obj_get_width(obj),(long)lv_obj_get_height(obj));
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

static void checkLayout(const char* tag)
{
    lv_display_t* d=lv_display_get_default();
    lv_area_t surf{0,0,(lv_coord_t)(lv_display_get_horizontal_resolution(d)-1),
                        (lv_coord_t)(lv_display_get_vertical_resolution(d)-1)};
    gBoundFails=0; gOverlapFails=0;
    lv_obj_t* root=lv_screen_active();
    checkBounds(root,surf,0);
    checkSiblingOverlaps(root,0);
    // stage = second child of root (header, stage, keyboard strip): its three
    // columns are the main overlap hazard
    if(lv_obj_get_child_count(root)>=2)
        checkSiblingOverlaps(lv_obj_get_child(root,1),1);
    LOGF("[%s] checks: bounds=%d overlap=%d\n",tag,gBoundFails,gOverlapFails);
}

static void idleFrames(DISTRHO::UIExporter* exp, int n)
{
    for(int i=0;i<n && exp->plugin_idle();++i) Sleep(16);
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
static int gTestFails=0;
#define EXPECT(cond,msg) do{ if(cond) LOGF("PASS %s\n",msg); else { LOGF("FAIL %s\n",msg); ++gTestFails; } }while(0)

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

static lv_obj_t* findButtonByUserData(lv_obj_t* root,intptr_t ud)
{
    if(root->class_p && 0==std::strcmp(root->class_p->name,"lv_button")
       && (intptr_t)lv_obj_get_user_data(root)==ud) return root;
    const uint32_t n=lv_obj_get_child_count(root);
    for(uint32_t i=0;i<n;++i){ lv_obj_t* r=findButtonByUserData(lv_obj_get_child(root,i),ud); if(r) return r; }
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


static DISTRHO::UIExporter* gExp=nullptr;

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
    const Sz sizes[]={{1440,860,"ui_zoom_1440.bmp","default"},
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

    // ---- T1b: exercise one zoom step (125%) and prove aspect-locked rescale -
    LOGF("=== zoom-step test ===\n");
    first=false;
    EXPECT(resizeWindow(hwnd,exp,1440,860,first),"back-to-base-1440x860");
    lv_obj_t* zPlus=findButtonByUserData(lv_screen_active(),1);
    lv_obj_t* zMinus=findButtonByUserData(lv_screen_active(),-1);
    EXPECT(zPlus!=nullptr,"zoom-plus-found"); EXPECT(zMinus!=nullptr,"zoom-minus-found");
    if(zPlus){
        lv_obj_send_event(zPlus,LV_EVENT_CLICKED,nullptr);   // 100% -> 125%
        idleFrames(exp,120); pumpMsgs(); presentKick(hwnd,exp); idleFrames(exp,90);
        lv_display_t* d=lv_display_get_default();
        const long dw=lv_display_get_horizontal_resolution(d), dh=lv_display_get_vertical_resolution(d);
        LOGF("[zoom125] display=%ldx%ld gUIScale=%.3f\n",dw,dh,(double)DISTRHO::gUIScale);
        EXPECT(dw==1800&&dh==1075,"display-exactly-1800x1075");
        EXPECT(dw*860==dh*1440,"aspect-ratio-exact");
        checkLayout("zoom125");
        presentKick(hwnd,exp); idleFrames(exp,90); writeBMP(hwnd,"ui_zoom_step_125.bmp");
        // drift/stability: + then - twice must return to the EXACT base size
        lv_obj_t* zm=findButtonByUserData(lv_screen_active(),-1);
        if(zm){ lv_obj_send_event(zm,LV_EVENT_CLICKED,nullptr); }              // -> 100%
        idleFrames(exp,120); pumpMsgs(); presentKick(hwnd,exp); idleFrames(exp,90);
        d=lv_display_get_default();
        const long dw2=lv_display_get_horizontal_resolution(d), dh2=lv_display_get_vertical_resolution(d);
        LOGF("[zoom-back] display=%ldx%ld gUIScale=%.3f\n",dw2,dh2,(double)DISTRHO::gUIScale);
        presentKick(hwnd,exp); idleFrames(exp,90); writeBMP(hwnd,"ui_zoom_back_100.bmp");
        EXPECT(DISTRHO::gUIScale>0.995f&&DISTRHO::gUIScale<1.005f,"scale-no-drift");
        checkLayout("zoom-back");
    }

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
    EXPECT(resizeWindow(hwnd,exp,1440,860,first),"base-size-for-click-test");
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
    LOGF("=== RESULT fails=%d bounds/overlap reported above ===\n",gTestFails);

    exp->quit();
    delete exp;
    if(gLog) fclose(gLog);
    printf("done\n");
    return 0;
}
