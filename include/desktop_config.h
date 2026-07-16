/*
 * desktop_config.h
 * Shared desktop configuration — no GLib dependency.
 */

#ifndef DESKTOP_CONFIG_H
#define DESKTOP_CONFIG_H

#include <stdbool.h>


/* Config path helpers */
char* get_vaxp_config_path(const char* filename);   /* caller must free() */
char* get_vaxp_main_config_path(void);              /* caller must free() */
char* get_vaxp_cache_path(const char* filename);    /* caller must free() */

void ensure_config_dir(void);


/* Wallpaper / animation / video volume */
char*   config_get_string(const char* section, const char* key, const char* fallback); /* caller must free() */
int     config_get_int(const char* section, const char* key, int fallback);
void    config_set_string(const char* section, const char* key, const char* value);
void    config_set_int(const char* section, const char* key, int value);

#endif /* DESKTOP_CONFIG_H */
