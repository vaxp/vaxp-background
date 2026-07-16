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

/* Gets the currently playing texture(s) without advancing the frame */
bool video_wallpaper_get_texture(GLuint* tex_y_out, GLuint* tex_uv_out, int* w, int* h);

/* Pulls the next frame from the video stream if available.
 * Sets tex_y_out and tex_uv_out.
 * Returns true if a new frame was loaded, false otherwise. */
bool video_wallpaper_update_texture(GLuint* tex_y_out, GLuint* tex_uv_out, int* w, int* h);

#endif /* VIDEO_WALLPAPER_H */
