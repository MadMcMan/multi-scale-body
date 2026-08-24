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
#include "ui/UICommon.hpp"
#include <cstdio>
#include <cstring>
#include <vector>
#include <windows.h>

static FILE* gLog=nullptr;
#define LOGF(...) do{ if(gLog){ fprintf(gLog,__VA_ARGS__); fflush(gLog);} printf(__VA_ARGS__); }while(0)

START_NAMESPACE_DISTRHO

static void stubEditParam(void*, uint32_t, bool) {}
static void stubSetParam(void*, uint32_t, float) {}
static void stubSetState(void*, const char*, const char*) {}
static void stubSendNote(void*, uint8_t, uint8_t, uint8_t) {}
static void stubSetSize(void*, uint, uint) {}
static bool stubFileRequest(void*, const char*) { return false; }

// copy the window rect from the desktop (window is TOPMOST => unoccluded)
static bool grabScreen(HWND hwnd, int& w, int& h, std::vector<unsigned char>& px)
{
    RECT wr; if(!GetWindowRect(hwnd,&wr)) return false;
    w=wr.right-wr.left; h=wr.bottom-wr.top;
    if(w<=0||h<=0) return false;
    HDC sdc=GetDC(NULL);
    HDC mdc=CreateCompatibleDC(sdc);
    HBITMAP bmp=CreateCompatibleBitmap(sdc,w,h);
    HGDIOBJ old=SelectObject(mdc,bmp);
    BitBlt(mdc,0,0,w,h,sdc,wr.left,wr.top,SRCCOPY);
    BITMAPINFO bi; memset(&bi,0,sizeof(bi));
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=w; bi.bmiHeader.biHeight=h;
    bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
    px.resize((size_t)w*h*4);
    const int got=GetDIBits(mdc,bmp,0,h,px.data(),&bi,DIB_RGB_COLORS);
    SelectObject(mdc,old); DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(NULL,sdc);
    return got==h;
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
    int w=0,h=0; std::vector<unsigned char> px;
    if(!grabScreen(hwnd,w,h,px)) { LOGF("grab failed %s\n",path); return false; }
    const double br=blackRatio(px);
    if(br>0.98) { LOGF("blank capture %s (%.2f)\n",path,br); return false; }
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

END_NAMESPACE_DISTRHO

int main()
{
    SetProcessDPIAware(); // 1:1 physical pixels: no DWM scaling in screen blits
    gLog=fopen("ui_verify_log.txt","w");
    DISTRHO::UIExporter* exp=new DISTRHO::UIExporter(
        nullptr, 0, 48000.0,
        &DISTRHO::stubEditParam, &DISTRHO::stubSetParam, &DISTRHO::stubSetState,
        &DISTRHO::stubSendNote,  &DISTRHO::stubSetSize,  &DISTRHO::stubFileRequest);

    HWND hwnd=(HWND)exp->getNativeWindowHandle();
    if(!hwnd){ LOGF("no native handle\n"); return 1; }

    ShowWindow(hwnd,SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    SetWindowPos(hwnd,HWND_TOPMOST,40,40,0,0,SWP_NOSIZE);

    idleFrames(exp,180);
    presentKick(hwnd,exp);

    struct Sz{ int w,h; const char* bmp; const char* tag; };
    const Sz sizes[]={{1440,860,"ui_remake_1440.bmp","default"},
                      {1100,700,"ui_remake_1100.bmp","small"},
                      {2028,1104,"ui_remake_2048.bmp","large"}}; // screen 2048x1152: largest client that fits incl. frame
    bool first=true;
    for(const Sz& s:sizes){
        RECT r{0,0,s.w,s.h};
        AdjustWindowRect(&r,(DWORD)GetWindowLongPtr(hwnd,GWL_STYLE),FALSE);
        const int wx=(s.w>1800)?0:40, wy=(s.w>1800)?0:40; // large pins to origin so the blit never leaves the desktop
        SetWindowPos(hwnd,NULL,wx,wy,r.right-r.left,r.bottom-r.top,SWP_NOZORDER);
        idleFrames(exp,first?60:240);
        presentKick(hwnd,exp); // GL surface needs a WM_SIZE nudge to present the resized frame
        first=false;
        LOGF("=== %s ===\n",s.tag);
        logState(exp,s.tag);
        checkLayout(s.tag);
        LOGF("=== %s raw (no update_layout) ===\n",s.tag);
        dumpTree(lv_screen_active(),2); // raw coords as the renderer sees them
        idleFrames(exp,90);             // render the settled layout before grabbing
        writeBMP(hwnd,s.bmp);
    }

    exp->quit();
    delete exp;
    if(gLog) fclose(gLog);
    printf("done\n");
    return 0;
}
