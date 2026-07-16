/*
 * desktop_config.c
 * Desktop configuration — standalone INI parser, no GLib dependency.
 *
 * INI format:
 *   [Section]
 *   Key=Value
 *
 * All public functions are thread-safe for reading; writes are non-atomic
 * (acceptable for a single-process desktop daemon).
 */

#include "desktop_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* ============================================================================
 * PATH HELPERS
 * ============================================================================ */

/* Returns $XDG_CONFIG_HOME or $HOME/.config */
static const char* xdg_config_home(void) {
    const char* p = getenv("XDG_CONFIG_HOME");
    if (p && p[0]) return p;
    return NULL; /* fallback handled in callers */
}

static const char* home_dir(void) {
    const char* h = getenv("HOME");
    return h ? h : "/tmp";
}

/* Build: $XDG_CONFIG_HOME/vaxp/desktop/<filename>  or
 *         $HOME/.config/vaxp/desktop/<filename>      */
char* get_vaxp_config_path(const char* filename) {
    char buf[4096];
    const char* cfg = xdg_config_home();
    if (cfg)
        snprintf(buf, sizeof(buf), "%s/vaxp/desktop/%s", cfg, filename);
    else
        snprintf(buf, sizeof(buf), "%s/.config/vaxp/desktop/%s", home_dir(), filename);
    return strdup(buf);
}

char* get_vaxp_main_config_path(void) {
    return get_vaxp_config_path("desktop.vaxp");
}

char* get_vaxp_cache_path(const char* filename) {
    char buf[4096];
    const char* cache = getenv("XDG_CACHE_HOME");
    if (cache && cache[0])
        snprintf(buf, sizeof(buf), "%s/vaxp-thumbnails/%s", cache, filename);
    else
        snprintf(buf, sizeof(buf), "%s/.cache/vaxp-thumbnails/%s", home_dir(), filename);
    return strdup(buf);
}

void ensure_config_dir(void) {
    char buf[4096];
    const char* cfg = xdg_config_home();
    if (cfg)
        snprintf(buf, sizeof(buf), "%s/vaxp/desktop", cfg);
    else
        snprintf(buf, sizeof(buf), "%s/.config/vaxp/desktop", home_dir());

    /* mkdir -p equivalent */
    for (char* p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

/* ============================================================================
 * MINIMAL INI PARSER
 * ============================================================================ */

/*
 * ini_lookup: search for [section] / key=value in path.
 * Returns a heap-allocated trimmed value string on success, NULL otherwise.
 * Caller must free() the result.
 */
static char* ini_lookup(const char* path, const char* section, const char* key) {
    FILE* f = fopen(path, "r");
    if (!f) return NULL;

    char line[1024];
    char cur_section[256] = "";
    int  in_section = 0;
    char* result = NULL;

    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline / CR */
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln-1] == '\n' || line[ln-1] == '\r'))
            line[--ln] = '\0';

        /* Skip blank lines and comments */
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == ';' || *p == '#') continue;

        /* Section header */
        if (*p == '[') {
            char* end = strchr(p, ']');
            if (end) {
                *end = '\0';
                snprintf(cur_section, sizeof(cur_section), "%s", p + 1);
                in_section = (strcmp(cur_section, section) == 0);
            }
            continue;
        }

        if (!in_section) continue;

        /* key=value */
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';

        /* Trim key */
        char* kend = eq - 1;
        while (kend >= p && (*kend == ' ' || *kend == '\t')) *kend-- = '\0';

        if (strcmp(p, key) != 0) continue;

        /* Trim value */
        char* val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;
        result = strdup(val);
        break;
    }

    fclose(f);
    return result;
}

/*
 * ini_set: write [section] key=value to path.
 * Reads the existing file, replaces or appends the key, rewrites.
 */
static void ini_set(const char* path, const char* section, const char* key, const char* value) {
    /* Read existing content */
    FILE* f = fopen(path, "r");
    size_t capacity = 65536;
    char*  buf = (char*)malloc(capacity);
    if (!buf) return;
    size_t len = 0;
    if (f) {
        size_t n;
        while ((n = fread(buf + len, 1, capacity - len - 1, f)) > 0) {
            len += n;
            if (len + 1 >= capacity) {
                capacity *= 2;
                char* nb = (char*)realloc(buf, capacity);
                if (!nb) { free(buf); fclose(f); return; }
                buf = nb;
            }
        }
        buf[len] = '\0';
        fclose(f);
    } else {
        buf[0] = '\0';
    }

    /* Build new content line-by-line */
    size_t out_cap = capacity + strlen(section) + strlen(key) + strlen(value) + 64;
    char*  out = (char*)malloc(out_cap);
    if (!out) { free(buf); return; }
    size_t out_len = 0;

    #define APPEND(s) do { \
        size_t sl = strlen(s); \
        if (out_len + sl + 1 >= out_cap) { \
            out_cap = (out_cap + sl) * 2; \
            char* no = (char*)realloc(out, out_cap); \
            if (!no) goto done; \
            out = no; \
        } \
        memcpy(out + out_len, s, sl); out_len += sl; \
    } while(0)

    int in_section = 0;
    int key_written = 0;
    char line[1024];
    size_t pos = 0;

    /* Parse buf line by line */
    while (pos < len) {
        size_t start = pos;
        while (pos < len && buf[pos] != '\n') pos++;
        size_t end = pos;
        if (pos < len) pos++; /* skip '\n' */

        /* Copy line into line[] */
        size_t ll = end - start;
        if (ll >= sizeof(line) - 1) ll = sizeof(line) - 2;
        memcpy(line, buf + start, ll);
        line[ll] = '\0';
        /* strip CR */
        if (ll > 0 && line[ll-1] == '\r') line[--ll] = '\0';

        char* p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (*p == '[') {
            /* If leaving our section and key not yet written, write it now */
            if (in_section && !key_written) {
                char tmp[512];
                snprintf(tmp, sizeof(tmp), "%s=%s\n", key, value);
                APPEND(tmp);
                key_written = 1;
            }
            char* end_b = strchr(p, ']');
            if (end_b) {
                *end_b = '\0';
                in_section = (strcmp(p + 1, section) == 0);
                *end_b = ']';
            }
            APPEND(line); APPEND("\n");
            continue;
        }

        if (in_section && !(*p == '\0' || *p == ';' || *p == '#')) {
            char* eq = strchr(p, '=');
            if (eq) {
                char* kend = eq - 1;
                while (kend >= p && (*kend == ' ' || *kend == '\t')) *kend-- = '\0';
                if (strcmp(p, key) == 0) {
                    /* Replace */
                    char tmp[512];
                    snprintf(tmp, sizeof(tmp), "%s=%s\n", key, value);
                    APPEND(tmp);
                    key_written = 1;
                    continue;
                }
            }
        }

        APPEND(line); APPEND("\n");
    }

    /* If section was never encountered, append it */
    if (!key_written) {
        if (out_len > 0 && out[out_len-1] != '\n') APPEND("\n");
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "[%s]\n%s=%s\n", section, key, value);
        APPEND(tmp);
    }

done:
    out[out_len] = '\0';
    free(buf);

    /* Write atomically via temp file */
    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE* wf = fopen(tmp_path, "w");
    if (wf) {
        fwrite(out, 1, out_len, wf);
        fclose(wf);
        rename(tmp_path, path);
    }
    free(out);

    #undef APPEND
}

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

char* config_get_string(const char* section, const char* key, const char* fallback) {
    char* path = get_vaxp_main_config_path();
    char* val  = ini_lookup(path, section, key);
    free(path);
    if (val) return val;
    return fallback ? strdup(fallback) : NULL;
}

int config_get_int(const char* section, const char* key, int fallback) {
    char* s = config_get_string(section, key, NULL);
    if (!s) return fallback;
    int v = atoi(s);
    free(s);
    return v;
}

void config_set_string(const char* section, const char* key, const char* value) {
    ensure_config_dir();
    char* path = get_vaxp_main_config_path();
    ini_set(path, section, key, value);
    free(path);
}

void config_set_int(const char* section, const char* key, int value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    config_set_string(section, key, buf);
}

/* ============================================================================
 * DESKTOP / SORT MODE
 * ============================================================================ */

DesktopMode get_current_desktop_mode(void) {
    char* s = config_get_string("Desktop", "Mode", "normal");
    DesktopMode m = MODE_NORMAL;
    if (s) {
        if (strncmp(s, "work", 4) == 0)    m = MODE_WORK;
        else if (strncmp(s, "widgets", 7) == 0) m = MODE_WIDGETS;
        free(s);
    }
    return m;
}

void set_current_desktop_mode(DesktopMode mode) {
    const char* str = "normal";
    if (mode == MODE_WORK)    str = "work";
    else if (mode == MODE_WIDGETS) str = "widgets";
    config_set_string("Desktop", "Mode", str);
}

DesktopSortMode get_current_sort_mode(void) {
    char* path = get_vaxp_config_path("desktop-sort");
    FILE* f = fopen(path, "r");
    free(path);
    DesktopSortMode mode = SORT_MANUAL;
    if (f) {
        char buf[64] = "";
        if (fgets(buf, sizeof(buf), f)) {
            /* strip whitespace */
            size_t l = strlen(buf);
            while (l > 0 && (buf[l-1] == '\n' || buf[l-1] == '\r' || buf[l-1] == ' '))
                buf[--l] = '\0';
            if (strcmp(buf, "name") == 0)          mode = SORT_NAME;
            else if (strcmp(buf, "type") == 0)     mode = SORT_TYPE;
            else if (strcmp(buf, "date-modified") == 0) mode = SORT_DATE_MODIFIED;
            else if (strcmp(buf, "size") == 0)     mode = SORT_SIZE;
        }
        fclose(f);
    }
    return mode;
}

void set_current_sort_mode(DesktopSortMode mode) {
    const char* str = "manual";
    if (mode == SORT_NAME)          str = "name";
    else if (mode == SORT_TYPE)     str = "type";
    else if (mode == SORT_DATE_MODIFIED) str = "date-modified";
    else if (mode == SORT_SIZE)     str = "size";

    ensure_config_dir();
    char* path = get_vaxp_config_path("desktop-sort");
    FILE* f = fopen(path, "w");
    if (f) { fputs(str, f); fclose(f); }
    free(path);
}

char* get_current_desktop_path(void) {
    DesktopMode mode = get_current_desktop_mode();
    const char* home = home_dir();

    if (mode == MODE_WIDGETS) return NULL;

    char buf[4096];
    if (mode == MODE_WORK) {
        snprintf(buf, sizeof(buf), "%s/Work", home);
        mkdir(buf, 0755);
        return strdup(buf);
    }
    snprintf(buf, sizeof(buf), "%s/Desktop", home);
    return strdup(buf);
}

/* ============================================================================
 * ICON POSITIONS  (stored in desktop-items.ini  [Positions] key=x  key_y=y)
 * ============================================================================ */

void save_item_position(const char* filename, int x, int y) {
    ensure_config_dir();
    char* path = get_vaxp_config_path("desktop-items.ini");

    char key_x[512], key_y[512];
    snprintf(key_x, sizeof(key_x), "%s",    filename);
    snprintf(key_y, sizeof(key_y), "%s_y",  filename);

    char vx[16], vy[16];
    snprintf(vx, sizeof(vx), "%d", x);
    snprintf(vy, sizeof(vy), "%d", y);

    ini_set(path, "Positions", key_x, vx);
    ini_set(path, "Positions", key_y, vy);
    free(path);
}

bool get_item_position(const char* filename, int* x, int* y) {
    char* path = get_vaxp_config_path("desktop-items.ini");

    char key_x[512], key_y[512];
    snprintf(key_x, sizeof(key_x), "%s",   filename);
    snprintf(key_y, sizeof(key_y), "%s_y", filename);

    char* sx = ini_lookup(path, "Positions", key_x);
    char* sy = ini_lookup(path, "Positions", key_y);
    free(path);

    if (!sx || !sy) { free(sx); free(sy); return false; }
    *x = atoi(sx);
    *y = atoi(sy);
    free(sx); free(sy);
    return true;
}
