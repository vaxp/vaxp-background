/*
 * wallpaper.h
 * Static image wallpaper via GStreamer + OpenGL — no GTK.
 */

#ifndef WALLPAPER_H
#define WALLPAPER_H

#include <stdbool.h>

/* Screen geometry (set after X11 display query) */
extern int screen_w;
extern int screen_h;

/* Initialise wallpaper subsystem (call once after OpenGL context is ready). */
void wallpaper_init(void);

/* Monitor config file for changes. */
void init_wallpaper_monitor(void);

/* Load the wallpaper saved in desktop.vaxp and display it. */
void load_saved_wallpaper(void);

/* Render the current wallpaper to the OpenGL canvas.
 * elapsed_sec: seconds since the transition started (0 = no transition). */
void wallpaper_render(float elapsed_sec);

/* Notify that the X11 window was resized. */
void wallpaper_resize(int w, int h);

/* Cleanup resources. */
void wallpaper_destroy(void);

/* True if the wallpaper monitor file descriptor is ready to read. */
bool wallpaper_monitor_fd_ready(void);

/* Return the inotify fd (or -1 if unavailable). */
int wallpaper_monitor_fd(void);

#endif /* WALLPAPER_H */
