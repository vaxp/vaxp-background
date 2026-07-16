/*
 * desktop_config.h
 * Shared desktop configuration — no GLib dependency.
 */

#ifndef DESKTOP_CONFIG_H
#define DESKTOP_CONFIG_H

#include <stdbool.h>

typedef enum {
    MODE_NORMAL,
    MODE_WORK,
    MODE_WIDGETS
} DesktopMode;

typedef enum {
    SORT_MANUAL,
    SORT_NAME,
    SORT_TYPE,
    SORT_DATE_MODIFIED,
    SORT_SIZE
} DesktopSortMode;

#define ICON_SIZE   56
#define ITEM_WIDTH  120
#define ITEM_HEIGHT 128
#define GRID_X      20
#define GRID_Y      20

/* Config path helpers */
char* get_vaxp_config_path(const char* filename);   /* caller must free() */
char* get_vaxp_main_config_path(void);              /* caller must free() */
char* get_vaxp_cache_path(const char* filename);    /* caller must free() */

void ensure_config_dir(void);

/* Desktop / sort mode */
DesktopMode     get_current_desktop_mode(void);
void            set_current_desktop_mode(DesktopMode mode);

DesktopSortMode get_current_sort_mode(void);
void            set_current_sort_mode(DesktopSortMode mode);

/* Desktop path */
char* get_current_desktop_path(void);  /* caller must free() */

/* Icon positions */
void    save_item_position(const char* filename, int x, int y);
bool    get_item_position(const char* filename, int* x, int* y);

/* Wallpaper / animation / video volume */
char*   config_get_string(const char* section, const char* key, const char* fallback); /* caller must free() */
int     config_get_int(const char* section, const char* key, int fallback);
void    config_set_string(const char* section, const char* key, const char* value);
void    config_set_int(const char* section, const char* key, int value);

#endif /* DESKTOP_CONFIG_H */
