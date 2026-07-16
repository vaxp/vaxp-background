/*
 * backend_x11.c
 * X11 + GLX display backend.
 * Refactored from main.c — no GTK dependency.
 */

#include "backend.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>
#include <GL/glx.h>
#include <GL/gl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration of canvas API */
typedef struct VaxpGLCanvas VaxpGLCanvas;
extern VaxpGLCanvas* vaxp_canvas_create_opengl(Display*, Window, unsigned, unsigned);

/* ── Private state ─────────────────────────────────────────────────────────── */

typedef struct {
    DisplayBackend base;
    Display*       dpy;
    Window         win;
    Colormap       cm;
} X11Backend;

/* ── EWMH helpers ──────────────────────────────────────────────────────────── */

static void set_window_type_desktop(Display* dpy, Window win) {
    Atom t = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE",         False);
    Atom v = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    XChangeProperty(dpy, win, t, XA_ATOM, 32, PropModeReplace, (unsigned char*)&v, 1);
}

static void set_window_state_below(Display* dpy, Window win) {
    Atom s = XInternAtom(dpy, "_NET_WM_STATE",       False);
    Atom v = XInternAtom(dpy, "_NET_WM_STATE_BELOW", False);
    XChangeProperty(dpy, win, s, XA_ATOM, 32, PropModeReplace, (unsigned char*)&v, 1);
}

static void set_no_decorations(Display* dpy, Window win) {
    Atom motif = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    if (motif == None) return;
    unsigned long hints[5] = { 2, 0, 0, 0, 0 };
    XChangeProperty(dpy, win, motif, motif, 32, PropModeReplace, (unsigned char*)hints, 5);
}

static void set_skip_taskbar(Display* dpy, Window win) {
    Atom s  = XInternAtom(dpy, "_NET_WM_STATE",              False);
    Atom tb = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    Atom pg = XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER",   False);
    Atom st = XInternAtom(dpy, "_NET_WM_STATE_STICKY",       False);
    Atom states[3] = { tb, pg, st };
    XChangeProperty(dpy, win, s, XA_ATOM, 32, PropModeReplace, (unsigned char*)states, 3);
}

/* ── Monitor geometry ─────────────────────────────────────────────────────── */

static bool get_primary_geometry(Display* dpy, int* x, int* y, int* w, int* h) {
    Window root = DefaultRootWindow(dpy);
    XRRScreenResources* res = XRRGetScreenResources(dpy, root);
    if (!res) goto fallback;

    RROutput primary = XRRGetOutputPrimary(dpy, root);
    bool found = false;

    for (int i = 0; i < res->noutput && !found; i++) {
        XRROutputInfo* oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (!oi) continue;
        if (oi->connection == RR_Connected && oi->crtc != None) {
            if (res->outputs[i] == primary || (!found && i == 0)) {
                XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
                if (ci) {
                    *x = ci->x;  *y = ci->y;
                    *w = (int)ci->width; *h = (int)ci->height;
                    found = true;
                    XRRFreeCrtcInfo(ci);
                }
            }
        }
        XRRFreeOutputInfo(oi);
    }
    XRRFreeScreenResources(res);
    if (found) return true;

fallback:
    *x = 0; *y = 0;
    *w = DisplayWidth(dpy, DefaultScreen(dpy));
    *h = DisplayHeight(dpy, DefaultScreen(dpy));
    return true;
}

/* ── GLXFBConfig selection ────────────────────────────────────────────────── */

static GLXFBConfig choose_fbconfig(Display* dpy, XVisualInfo** vi_out) {
    static int attr[] = {
        GLX_X_RENDERABLE,  True,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE,   GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
        GLX_RED_SIZE,   8, GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE,  8, GLX_ALPHA_SIZE, 8,
        GLX_DEPTH_SIZE, 24,
        GLX_DOUBLEBUFFER, True,
        None
    };
    int n = 0;
    GLXFBConfig* cfgs = glXChooseFBConfig(dpy, DefaultScreen(dpy), attr, &n);
    if (!cfgs || n == 0) return NULL;
    GLXFBConfig best = cfgs[0];
    *vi_out = glXGetVisualFromFBConfig(dpy, best);
    XFree(cfgs);
    return best;
}

/* ── Backend callbacks ────────────────────────────────────────────────────── */

static void x11_dispatch(DisplayBackend* self) {
    X11Backend* b = (X11Backend*)self;
    while (XPending(b->dpy)) {
        XEvent ev;
        XNextEvent(b->dpy, &ev);
        if (ev.type == ConfigureNotify) {
            XConfigureEvent* ce = &ev.xconfigure;
            b->base.screen_w = ce->width;
            b->base.screen_h = ce->height;
        }
    }
}

static void x11_flush(DisplayBackend* self) {
    X11Backend* b = (X11Backend*)self;
    glXSwapBuffers(b->dpy, b->win);
}

static void x11_destroy(DisplayBackend* self) {
    X11Backend* b = (X11Backend*)self;
    glXMakeCurrent(b->dpy, None, NULL);
    XDestroyWindow(b->dpy, b->win);
    XFreeColormap(b->dpy, b->cm);
    XCloseDisplay(b->dpy);
    free(b);
}

/* ── Public constructor ───────────────────────────────────────────────────── */

DisplayBackend* backend_x11_create(void) {
    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[X11Backend] Cannot open display\n");
        return NULL;
    }

    int wx, wy, ww, wh;
    if (!get_primary_geometry(dpy, &wx, &wy, &ww, &wh)) {
        XCloseDisplay(dpy);
        return NULL;
    }
    printf("[X11Backend] Monitor: %dx%d+%d+%d\n", ww, wh, wx, wy);

    XVisualInfo* vi = NULL;
    GLXFBConfig fbc = choose_fbconfig(dpy, &vi);
    if (!fbc || !vi) {
        fprintf(stderr, "[X11Backend] No suitable GLXFBConfig\n");
        XCloseDisplay(dpy);
        return NULL;
    }

    /* Create window */
    Window root = DefaultRootWindow(dpy);
    XSetWindowAttributes swa;
    swa.colormap         = XCreateColormap(dpy, root, vi->visual, AllocNone);
    swa.border_pixel     = 0;
    swa.override_redirect= False;
    swa.event_mask       = StructureNotifyMask | ExposureMask;

    Window win = XCreateWindow(dpy, root, wx, wy, (unsigned)ww, (unsigned)wh,
                               0, vi->depth, InputOutput, vi->visual,
                               CWColormap | CWBorderPixel | CWEventMask, &swa);
    Colormap cm = swa.colormap;
    XFree(vi);

    set_window_type_desktop(dpy, win);
    set_window_state_below(dpy, win);
    set_no_decorations(dpy, win);
    set_skip_taskbar(dpy, win);

    XClassHint* ch = XAllocClassHint();
    ch->res_name  = (char*)"vaxp-background";
    ch->res_class = (char*)"VaxpBackground";
    XSetClassHint(dpy, win, ch);
    XFree(ch);
    XStoreName(dpy, win, "vaxp-background");
    XMapWindow(dpy, win);
    XLowerWindow(dpy, win);
    XFlush(dpy);

    /* Create OpenGL canvas (creates GL 3.3 Core context) */
    VaxpGLCanvas* canvas = vaxp_canvas_create_opengl(dpy, win,
                                                       (unsigned)ww, (unsigned)wh);
    if (!canvas) {
        fprintf(stderr, "[X11Backend] Failed to create GL canvas\n");
        XDestroyWindow(dpy, win);
        XFreeColormap(dpy, cm);
        XCloseDisplay(dpy);
        return NULL;
    }

    X11Backend* b = (X11Backend*)calloc(1, sizeof(X11Backend));
    if (!b) { XCloseDisplay(dpy); return NULL; }

    b->base.screen_w = ww;
    b->base.screen_h = wh;
    b->base.event_fd = ConnectionNumber(dpy);
    b->base.canvas   = canvas;
    b->base.dispatch = x11_dispatch;
    b->base.flush    = x11_flush;
    b->base.destroy  = x11_destroy;
    b->dpy = dpy;
    b->win = win;
    b->cm  = cm;

    printf("[X11Backend] Ready (%dx%d)\n", ww, wh);
    return &b->base;
}
