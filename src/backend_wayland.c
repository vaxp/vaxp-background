/*
 * backend_wayland.c
 * Wayland + EGL + wlr-layer-shell display backend.
 *
 * Creates a background-layer surface using the zwlr_layer_shell_v1 protocol,
 * sets up an EGL context on top of it, and returns a DisplayBackend with an
 * initialised VaxpGLCanvas.
 *
 * No GTK, no GLib, no libmpv.
 */

#include "backend.h"

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

/* Auto-generated from wlr-layer-shell-unstable-v1.xml by wayland-scanner */
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

/* Forward declaration of EGL canvas API */
typedef struct VaxpGLCanvas VaxpGLCanvas;
extern VaxpGLCanvas* vaxp_canvas_create_from_current_context(
        unsigned width, unsigned height,
        void* (*proc_loader)(const char*),
        void  (*swap_fn)(void*),
        void*  swap_opaque);

/* ── Private state struct ──────────────────────────────────────────────────── */

typedef struct {
    DisplayBackend base;

    /* Wayland objects */
    struct wl_display*            wl_display;
    struct wl_registry*           registry;
    struct wl_compositor*         compositor;
    struct wl_surface*            surface;
    struct zwlr_layer_shell_v1*   layer_shell;
    struct zwlr_layer_surface_v1* layer_surface;
    struct wl_output*             wl_output;

    /* EGL objects */
    EGLDisplay  egl_dpy;
    EGLSurface  egl_surf;
    EGLContext  egl_ctx;
    struct wl_egl_window* egl_window;

    /* Configure state */
    bool    configured;
    int     cfg_w, cfg_h;

    /* EGL config stored so callers can create shared contexts */
    EGLConfig egl_cfg;
} WaylandBackend;

/* ── EGL swap callback (called by VaxpGLCanvas on flush) ──────────────────── */

static void wayland_egl_swap(void* opaque) {
    WaylandBackend* wb = (WaylandBackend*)opaque;
    eglSwapBuffers(wb->egl_dpy, wb->egl_surf);
    wl_display_flush(wb->wl_display);
}

/* ── wlr-layer-surface listener ────────────────────────────────────────────── */

static void layer_surface_configure(void* data,
                                     struct zwlr_layer_surface_v1* surf,
                                     uint32_t serial, uint32_t w, uint32_t h) {
    WaylandBackend* wb = (WaylandBackend*)data;
    if (wb->cfg_w != (int)w || wb->cfg_h != (int)h || !wb->configured) {
        printf("[WaylandBackend] Configured: %ux%u\n", w, h);
    }
    
    wb->cfg_w = (int)w;
    wb->cfg_h = (int)h;
    zwlr_layer_surface_v1_ack_configure(surf, serial);
    wb->configured = true;

    /* Resize the EGL window to match */
    if (wb->egl_window)
        wl_egl_window_resize(wb->egl_window, (int)w, (int)h, 0, 0);

    /* Update base screen dimensions */
    wb->base.screen_w = (int)w;
    wb->base.screen_h = (int)h;
}

static void layer_surface_closed(void* data,
                                  struct zwlr_layer_surface_v1* surf) {
    (void)surf;
    WaylandBackend* wb = (WaylandBackend*)data;
    fprintf(stderr, "[WaylandBackend] Layer surface closed by compositor\n");
    /* Signal the main loop to exit gracefully */
    (void)wb;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed    = layer_surface_closed,
};

/* ── Wayland registry listener ─────────────────────────────────────────────── */

static void registry_global(void* data, struct wl_registry* reg,
                              uint32_t name, const char* interface,
                              uint32_t version) {
    WaylandBackend* wb = (WaylandBackend*)data;
    (void)version;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        wb->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        wb->layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0 && !wb->wl_output) {
        /* Bind first available output (primary monitor) */
        wb->wl_output = wl_registry_bind(reg, name, &wl_output_interface, 2);
    }
}

static void registry_global_remove(void* data, struct wl_registry* reg,
                                    uint32_t name) {
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

/* ── EGL context creation ──────────────────────────────────────────────────── */

static bool create_egl_context(WaylandBackend* wb) {
    wb->egl_dpy = eglGetDisplay((EGLNativeDisplayType)wb->wl_display);
    if (wb->egl_dpy == EGL_NO_DISPLAY) {
        fprintf(stderr, "[WaylandBackend] eglGetDisplay failed\n");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(wb->egl_dpy, &major, &minor)) {
        fprintf(stderr, "[WaylandBackend] eglInitialize failed\n");
        return false;
    }
    printf("[WaylandBackend] EGL %d.%d initialised\n", major, minor);

    /* We want OpenGL (not OpenGL ES) */
    if (!eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "[WaylandBackend] eglBindAPI(OpenGL) failed\n");
        return false;
    }

    /* Choose EGL config */
    static const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      24,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint n_cfg = 0;
    if (!eglChooseConfig(wb->egl_dpy, cfg_attribs, &cfg, 1, &n_cfg) || n_cfg == 0) {
        fprintf(stderr, "[WaylandBackend] No suitable EGL config\n");
        return false;
    }
    wb->egl_cfg = cfg;   /* store for shared-context creation */

    /* Create OpenGL 3.3 Core context */
    static const EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION,             3,
        EGL_CONTEXT_MINOR_VERSION,             3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK,       EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE, EGL_TRUE,
        EGL_NONE
    };
    wb->egl_ctx = eglCreateContext(wb->egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (wb->egl_ctx == EGL_NO_CONTEXT) {
        /* Fallback: try without forward-compatible flag */
        static const EGLint ctx_attribs2[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3,
            EGL_CONTEXT_MINOR_VERSION, 3,
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
            EGL_NONE
        };
        wb->egl_ctx = eglCreateContext(wb->egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attribs2);
    }
    if (wb->egl_ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "[WaylandBackend] eglCreateContext failed\n");
        return false;
    }

    /* Create EGL window surface (size is a placeholder; configure event resizes it) */
    wb->egl_window = wl_egl_window_create(wb->surface, 256, 256);
    if (!wb->egl_window) {
        fprintf(stderr, "[WaylandBackend] wl_egl_window_create failed\n");
        return false;
    }

    wb->egl_surf = eglCreateWindowSurface(wb->egl_dpy, cfg,
                                           (EGLNativeWindowType)wb->egl_window, NULL);
    if (wb->egl_surf == EGL_NO_SURFACE) {
        fprintf(stderr, "[WaylandBackend] eglCreateWindowSurface failed: 0x%x\n",
                eglGetError());
        return false;
    }

    if (!eglMakeCurrent(wb->egl_dpy, wb->egl_surf, wb->egl_surf, wb->egl_ctx)) {
        fprintf(stderr, "[WaylandBackend] eglMakeCurrent failed\n");
        return false;
    }

    /* VSync policy: eglSwapInterval(0) — do NOT wait for compositor callbacks.
     *
     * With interval=1, Mesa's EGL Wayland backend inserts a wl_surface_frame
     * request before each commit and blocks in wl_display_dispatch() until the
     * compositor sends the callback. For a BACKGROUND layer the compositor only
     * sends that callback when the content changes — which for a 30fps video is
     * just 30 times/second → the entire render loop is throttled to 30fps even
     * on a 100Hz monitor.
     *
     * With interval=0, eglSwapBuffers commits the buffer and returns immediately.
     * The compositor handles scanout at its own VBlank rate — we get no tearing
     * on Wayland because the compositor always picks the latest committed buffer
     * at each VBlank. The render loop now runs at the full monitor refresh rate.
     */
    eglSwapInterval(wb->egl_dpy, 0);

    return true;
}

/* ── Backend callbacks ─────────────────────────────────────────────────────── */

static void wayland_dispatch(DisplayBackend* self) {
    WaylandBackend* wb = (WaylandBackend*)self;
    /* Simple dispatch: flush pending events without blocking.
     * The select() in main.c already handles the blocking wait. */
    if (wl_display_prepare_read(wb->wl_display) == 0) {
        wl_display_read_events(wb->wl_display);
    }
    wl_display_dispatch_pending(wb->wl_display);
    wl_display_flush(wb->wl_display);
}

static void wayland_destroy(DisplayBackend* self) {
    WaylandBackend* wb = (WaylandBackend*)self;

    eglMakeCurrent(wb->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (wb->egl_surf)   eglDestroySurface(wb->egl_dpy, wb->egl_surf);
    if (wb->egl_ctx)    eglDestroyContext(wb->egl_dpy, wb->egl_ctx);
    if (wb->egl_window) wl_egl_window_destroy(wb->egl_window);
    if (wb->egl_dpy != EGL_NO_DISPLAY) eglTerminate(wb->egl_dpy);

    if (wb->layer_surface) zwlr_layer_surface_v1_destroy(wb->layer_surface);
    if (wb->layer_shell)   zwlr_layer_shell_v1_destroy(wb->layer_shell);
    if (wb->surface)       wl_surface_destroy(wb->surface);
    if (wb->compositor)    wl_compositor_destroy(wb->compositor);
    if (wb->wl_output)     wl_output_destroy(wb->wl_output);
    if (wb->registry)      wl_registry_destroy(wb->registry);
    if (wb->wl_display)    wl_display_disconnect(wb->wl_display);

    free(wb);
}

/* ── Public constructor ────────────────────────────────────────────────────── */

DisplayBackend* backend_wayland_create(void) {
    WaylandBackend* wb = (WaylandBackend*)calloc(1, sizeof(WaylandBackend));
    if (!wb) return NULL;

    wb->egl_dpy = EGL_NO_DISPLAY;
    wb->egl_surf = EGL_NO_SURFACE;
    wb->egl_ctx  = EGL_NO_CONTEXT;

    /* ── Connect to Wayland display ── */
    wb->wl_display = wl_display_connect(NULL);
    if (!wb->wl_display) {
        fprintf(stderr, "[WaylandBackend] Cannot connect to Wayland display\n");
        free(wb);
        return NULL;
    }
    printf("[WaylandBackend] Connected to Wayland compositor\n");

    /* ── Get globals via registry ── */
    wb->registry = wl_display_get_registry(wb->wl_display);
    wl_registry_add_listener(wb->registry, &registry_listener, wb);
    wl_display_roundtrip(wb->wl_display);   /* wait for all globals */

    if (!wb->compositor) {
        fprintf(stderr, "[WaylandBackend] wl_compositor not found\n");
        goto fail;
    }
    if (!wb->layer_shell) {
        fprintf(stderr, "[WaylandBackend] zwlr_layer_shell_v1 not found — "
                        "is your compositor wlroots-based (Sway/Hyprland)?\n");
        goto fail;
    }

    /* ── Create Wayland surface ── */
    wb->surface = wl_compositor_create_surface(wb->compositor);
    if (!wb->surface) {
        fprintf(stderr, "[WaylandBackend] Cannot create wl_surface\n");
        goto fail;
    }

    /* ── Create layer surface (background layer) ── */
    wb->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        wb->layer_shell,
        wb->surface,
        wb->wl_output,           /* NULL = let compositor pick output */
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
        "vaxp-background");

    if (!wb->layer_surface) {
        fprintf(stderr, "[WaylandBackend] Cannot create layer surface\n");
        goto fail;
    }

    zwlr_layer_surface_v1_add_listener(wb->layer_surface, &layer_surface_listener, wb);

    /* Anchor to all edges = full-screen stretch */
    zwlr_layer_surface_v1_set_anchor(wb->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP    |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT   |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);

    /* exclusive_zone = -1: don't push other windows, extend to screen edges */
    zwlr_layer_surface_v1_set_exclusive_zone(wb->layer_surface, -1);

    /* size (0,0) = let compositor tell us via configure event */
    zwlr_layer_surface_v1_set_size(wb->layer_surface, 0, 0);

    /* Keyboard interactivity: none (it's a wallpaper) */
    zwlr_layer_surface_v1_set_keyboard_interactivity(wb->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

    /* Commit the surface to trigger configure event */
    wl_surface_commit(wb->surface);
    wl_display_roundtrip(wb->wl_display);  /* receive configure */

    if (!wb->configured) {
        fprintf(stderr, "[WaylandBackend] No configure event received\n");
        goto fail;
    }
    printf("[WaylandBackend] Screen: %dx%d\n", wb->cfg_w, wb->cfg_h);

    /* ── Create EGL context ── */
    if (!create_egl_context(wb)) goto fail;

    /* Now resize the EGL window to the actual screen size */
    wl_egl_window_resize(wb->egl_window, wb->cfg_w, wb->cfg_h, 0, 0);

    /* ── Create canvas using the current EGL context ── */
    VaxpGLCanvas* canvas = vaxp_canvas_create_from_current_context(
        (unsigned)wb->cfg_w, (unsigned)wb->cfg_h,
        (void* (*)(const char*))eglGetProcAddress,
        wayland_egl_swap,
        wb);

    if (!canvas) {
        fprintf(stderr, "[WaylandBackend] Cannot create GL canvas\n");
        goto fail;
    }

    /* Initial surface commit to show the window */
    wl_surface_commit(wb->surface);
    wl_display_flush(wb->wl_display);

    /* ── Populate base struct ── */
    wb->base.screen_w    = wb->cfg_w;
    wb->base.screen_h    = wb->cfg_h;
    wb->base.event_fd    = wl_display_get_fd(wb->wl_display);
    wb->base.canvas      = canvas;
    wb->base.egl_display = (void*)wb->egl_dpy;
    wb->base.egl_context = (void*)wb->egl_ctx;
    wb->base.egl_config  = (void*)wb->egl_cfg;
    wb->base.dispatch    = wayland_dispatch;
    wb->base.flush       = (void (*)(DisplayBackend*))wayland_egl_swap;
    wb->base.destroy     = wayland_destroy;

    printf("[WaylandBackend] Ready (%dx%d)\n", wb->cfg_w, wb->cfg_h);
    return &wb->base;

fail:
    wayland_destroy(&wb->base);
    return NULL;
}

/* ── Auto-detect ─────────────────────────────────────────────────────────── */

DisplayBackend* backend_create_auto(void) {
    const char* wayland = getenv("WAYLAND_DISPLAY");
    const char* x11     = getenv("DISPLAY");

    if (wayland && wayland[0]) {
        printf("[Backend] Detected Wayland (%s)\n", wayland);
        DisplayBackend* b = backend_wayland_create();
        if (b) return b;
        fprintf(stderr, "[Backend] Wayland failed, trying X11 fallback...\n");
    }

    if (x11 && x11[0]) {
        printf("[Backend] Using X11 (%s)\n", x11);
        return backend_x11_create();
    }

    fprintf(stderr, "[Backend] No display available (set WAYLAND_DISPLAY or DISPLAY)\n");
    return NULL;
}
