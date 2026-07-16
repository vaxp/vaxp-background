/*
 * main.c
 * vaxp-background — desktop wallpaper daemon.
 *
 * Supports Wayland (wlr-layer-shell + EGL) and X11 (GLX).
 * Auto-detects which display server is available at runtime.
 *
 * Performance design:
 *   - eglSwapInterval(0): eglSwapBuffers returns immediately (no compositor
 *     callback wait). The Wayland compositor picks the latest buffer at each
 *     VBlank — no tearing, and the background layer is no longer throttled
 *     to the video frame rate.
 *   - Frame pacing: clock_nanosleep(TIMER_ABSTIME) targets the monitor
 *     refresh rate (TARGET_FPS). The CPU sleeps between frames instead of
 *     busy-spinning. Without this, the loop would run at unbounded speed
 *     (thousands of fps) burning 100% of a CPU core for no benefit.
 *   - GStreamer decodes video asynchronously via appsink (try_pull_sample
 *     timeout=0). When a new frame is ready we upload it; otherwise the
 *     last frame is re-presented at the full display refresh rate.
 */

#include "backend.h"
#include "wallpaper.h"
#include "video_wallpaper.h"
#include "layer_manager.h"
#include "desktop_config.h"

#include <gst/gst.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

/* ── Target frame rate ───────────────────────────────────────────────────────
 * Set to your monitor's refresh rate. The render loop sleeps between frames
 * via clock_nanosleep so the CPU is idle except during actual rendering.
 * For video-only content this could be the video's frame rate (e.g., 30),
 * but for future 2D/3D overlays we want the full display refresh rate.
 * ─────────────────────────────────────────────────────────────────────────── */
#ifndef TARGET_FPS
#define TARGET_FPS 100
#endif
#define FRAME_NS (1000000000L / TARGET_FPS)   /* nanoseconds per frame */

/* ── Signal handling ─────────────────────────────────────────────────────── */

static volatile bool g_running = true;

static void on_signal(int sig) {
    (void)sig;
    g_running = false;
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(int argc, char* argv[]) {
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* GStreamer init */
    gst_init(&argc, &argv);

    /* Create display backend (auto-detects Wayland vs X11) */
    DisplayBackend* backend = backend_create_auto();
    if (!backend) {
        fprintf(stderr, "[Main] No display backend available\n");
        return 1;
    }

    /* Initialise rendering subsystems */
    layer_manager_init();
    wallpaper_init();
    wallpaper_resize(backend->screen_w, backend->screen_h);
    video_wallpaper_init(backend->egl_display, backend->egl_context);

    /* Monitor config file for live wallpaper changes (inotify) */
    init_wallpaper_monitor();

    /* Load initial wallpaper from config */
    load_saved_wallpaper();

    /* ── Main loop ────────────────────────────────────────────────────────── */

    int inotify_fd = wallpaper_monitor_fd();

    /* FPS tracking */
    struct timespec fps_start;
    clock_gettime(CLOCK_MONOTONIC, &fps_start);
    int frame_count = 0;

    /* Frame deadline — absolute monotonic timestamp for the NEXT frame.
     * We use TIMER_ABSTIME so accumulated drift is impossible: each sleep
     * wakes up at an exact wall-clock time, not "N ms from now". */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);

    while (g_running) {
        /* ── Non-blocking event poll ── */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(backend->event_fd, &rfds);
        int max_fd = backend->event_fd;

        if (inotify_fd >= 0) {
            FD_SET(inotify_fd, &rfds);
            if (inotify_fd > max_fd) max_fd = inotify_fd;
        }

        struct timeval tv = { 0, 0 };
        select(max_fd + 1, &rfds, NULL, NULL, &tv);

        /* Dispatch display events */
        backend->dispatch(backend);

        /* Config file changed? Reload wallpaper */
        if (inotify_fd >= 0 && FD_ISSET(inotify_fd, &rfds)) {
            if (wallpaper_monitor_fd_ready())
                load_saved_wallpaper();
        }

        /* ── Render & present ── */
        wallpaper_render(0.0f);
        if (backend->flush) backend->flush(backend);

        /* ── FPS counter (prints once per second) ── */
        frame_count++;
        struct timespec fps_now;
        clock_gettime(CLOCK_MONOTONIC, &fps_now);
        double elapsed = (fps_now.tv_sec  - fps_start.tv_sec) +
                         (fps_now.tv_nsec - fps_start.tv_nsec) / 1e9;
        if (elapsed >= 1.0) {
            printf("[Performance] FPS: %.1f\n", frame_count / elapsed);
            fps_start  = fps_now;
            frame_count = 0;
        }

        /* ── Frame pacing: sleep until next deadline ──────────────────────
         * Advance deadline by one frame period and sleep until that moment.
         * TIMER_ABSTIME guarantees that late wakeups don't cause snowballing
         * debt — the next deadline is always [prev_deadline + FRAME_NS],
         * not [now + FRAME_NS].
         * ──────────────────────────────────────────────────────────────── */
        deadline.tv_nsec += FRAME_NS;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    }

    /* ── Cleanup ── */
    printf("[Main] Shutting down\n");
    if (video_wallpaper_is_active()) video_wallpaper_stop();
    wallpaper_destroy();
    backend->destroy(backend);
    gst_deinit();
    return 0;
}
