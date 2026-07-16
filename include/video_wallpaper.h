/*
 * video_wallpaper.h
 * Video wallpaper via GStreamer — no GTK, no libmpv.
 *
 * Zero-copy path: GStreamer GL Memory (GPU Decode → GL Texture, no CPU).
 * Fallback path:  VA-API + PBO double-buffered upload.
 */

#ifndef VIDEO_WALLPAPER_H
#define VIDEO_WALLPAPER_H

#include <stdbool.h>
#include <GL/gl.h>

/*
 * Initialise the video wallpaper subsystem.
 *
 * Pass the EGL display and context from the display backend to enable
 * GStreamer GL context sharing (zero-copy path). On X11/GLX backends
 * both pointers will be NULL — the system falls back to PBO upload.
 *
 * Call AFTER gst_init() and AFTER the GL context is current.
 */
void video_wallpaper_init(void* egl_display, void* egl_context);

/* Load and loop a video file as wallpaper. */
void video_wallpaper_load(const char* path);

/* Stop playback and release pipeline resources. */
void video_wallpaper_stop(void);

/* True if a video is currently playing. */
bool video_wallpaper_is_active(void);

/* Set playback volume (0–100). */
void video_wallpaper_set_volume(int volume);

/* Returns true if path has a recognised video extension. */
bool is_video_file(const char* path);

/*
 * video_wallpaper_update_texture:
 *   Poll the appsink for new frames.
 *   GL Memory path: returns a GStreamer-owned texture — do NOT delete it.
 *   PBO path:       returns our own texture — managed internally.
 *   Returns true if tex_out contains a NEW frame.
 *   Returns false if no new frame; tex_out/w_out/h_out are unchanged
 *   (caller should keep using its previous texture).
 */
bool video_wallpaper_update_texture(GLuint* tex_out, int* w_out, int* h_out);

/* Get the last known texture without polling GStreamer.
 * Returns false if no texture is available yet. */
bool video_wallpaper_get_texture(GLuint* tex_out, int* w_out, int* h_out);

#endif /* VIDEO_WALLPAPER_H */
