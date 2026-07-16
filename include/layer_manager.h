/*
 * layer_manager.h
 * Simple layer switcher — no GTK dependency.
 *
 * The wallpaper daemon has two logical "layers":
 *   LAYER_IMAGE  — static image rendered via OpenGL
 *   LAYER_VIDEO  — video frames uploaded to a GL texture
 *
 * The active layer determines what gets rendered each frame.
 */

#ifndef LAYER_MANAGER_H
#define LAYER_MANAGER_H

typedef enum {
    LAYER_IMAGE = 0,
    LAYER_VIDEO = 1
} WallpaperLayer;

/* Initialise the layer manager (call once at startup). */
void layer_manager_init(void);

/* Switch the active layer. */
void layer_manager_show_image(void);
void layer_manager_show_video(void);

/* Query current active layer. */
WallpaperLayer layer_manager_current(void);

#endif /* LAYER_MANAGER_H */
