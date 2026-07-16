/*
 * backend.h
 * Abstract display backend interface — supports X11/GLX and Wayland/EGL.
 */

#ifndef BACKEND_H
#define BACKEND_H

#include <stdbool.h>

/* Forward declaration */
typedef struct VaxpGLCanvas VaxpGLCanvas;

typedef struct DisplayBackend {
    int screen_w, screen_h;

    /* fd for select() — X11: XConnectionNumber, Wayland: wl_display_get_fd */
    int event_fd;

    /* Fully initialised OpenGL canvas */
    VaxpGLCanvas* canvas;

    /* EGL handles — exposed so subsystems (e.g. video_wallpaper) can share
     * the EGL context with GStreamer GL for zero-copy texture import.
     * NULL/EGL_NO_* on X11/GLX backends. */
    void* egl_display;   /* EGLDisplay */
    void* egl_context;   /* EGLContext */
    void* egl_config;    /* EGLConfig  — needed to create shared contexts */

    /* Process all pending display events (non-blocking). */
    void (*dispatch)(struct DisplayBackend* self);

    /* Swap buffers / flush rendering. */
    void (*flush)(struct DisplayBackend* self);

    /* Cleanup all resources. */
    void (*destroy)(struct DisplayBackend* self);
} DisplayBackend;

/*
 * Auto-detect display server and create appropriate backend.
 * Returns NULL on failure.
 *
 * Detection order:
 *   1. WAYLAND_DISPLAY set  → Wayland backend (EGL + wlr-layer-shell)
 *   2. DISPLAY set          → X11 backend (GLX)
 *   3. Both unset           → error
 */
DisplayBackend* backend_create_auto(void);

/* Backend constructors (prefer backend_create_auto) */
DisplayBackend* backend_x11_create(void);
DisplayBackend* backend_wayland_create(void);

#endif /* BACKEND_H */
