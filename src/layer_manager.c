/*
 * layer_manager.c
 * Simple active-layer tracker — no GTK dependency.
 */

#include "layer_manager.h"
#include "video_wallpaper.h"
#include <stdio.h>

static WallpaperLayer g_active_layer = LAYER_IMAGE;

void layer_manager_init(void) {
    g_active_layer = LAYER_IMAGE;
    printf("[LayerManager] Initialised\n");
}

void layer_manager_show_image(void) {
    if (g_active_layer == LAYER_VIDEO) {
        /* Stop video before switching away */
        video_wallpaper_stop();
    }
    g_active_layer = LAYER_IMAGE;
    printf("[LayerManager] Active layer: IMAGE\n");
}

void layer_manager_show_video(void) {
    g_active_layer = LAYER_VIDEO;
    printf("[LayerManager] Active layer: VIDEO\n");
}

WallpaperLayer layer_manager_current(void) {
    return g_active_layer;
}
