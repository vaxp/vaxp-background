/*
 * wallpaper.c
 * Static-image wallpaper rendered via OpenGL + GStreamer image decoding.
 * Transition effects implemented in software (alpha blending via GL).
 *
 * Image loading pipeline:
 *   filesrc location=<path> ! decodebin ! videoconvert !
 *   video/x-raw,format=RGBA,width=W,height=H ! appsink
 *
 * No GTK, no Cairo, no GdkPixbuf.
 */

#include "wallpaper.h"
#include "desktop_config.h"
#include "video_wallpaper.h"
#include "layer_manager.h"

/* Forward declaration of the canvas type & API from vaxp_canvas_opengl.c */
/* We only use the minimal subset needed here. */
#include <GL/gl.h>
#include <GL/glx.h>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

/* ── Screen geometry ────────────────────────────────────────────────────────── */

int screen_w = 0;
int screen_h = 0;

/* ── GL function pointers needed for texture ops (loaded externally) ─────────
 * We use the subset already loaded by vaxp_canvas_opengl.c via glXGetProcAddr.
 * Rather than re-loading them here, we declare them as extern weak so the
 * linker resolves them from the canvas translation unit.
 *
 * Alternatively (simpler): just call raw GL 1.x calls that don't need
 * extension loading — glGenTextures, glBindTexture, glTexImage2D etc.
 * are available directly in libGL on all Linux systems.
 * ─────────────────────────────────────────────────────────────────────────── */

/* ── Wallpaper state ─────────────────────────────────────────────────────── */

/* Current wallpaper texture */
static GLuint g_tex_current   = 0;
static int    g_tex_cur_w     = 0;
static int    g_tex_cur_h     = 0;

/* Previous wallpaper texture (held during transition) */
static GLuint g_tex_prev      = 0;
static int    g_tex_prev_w    = 0;
static int    g_tex_prev_h    = 0;

static char*  g_current_path  = NULL;
static int    g_anim_type     = 0;        /* 0..9 transition style */

/* Transition timing */
static double g_transition_start = 0.0;  /* wall-clock seconds */
static bool   g_in_transition    = false;
#define TRANSITION_DURATION 0.8           /* seconds */

/* inotify: watch the parent DIRECTORY so atomic rename() doesn't kill the watch */
static int    g_inotify_fd    = -1;
static int    g_inotify_wd    = -1;
static char   g_watch_filename[256] = {0}; /* basename of config file to filter */

/* ── Time helper ────────────────────────────────────────────────────────────── */

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ── GL shader IDs for blending (simple full-screen quad) ─────────────────── */

/* These function pointer types are already loaded by vaxp_canvas_opengl.c.
 * We expose a minimal interface via the canvas_* inline wrappers declared
 * there; however for the full-screen wallpaper quad we drive GL directly
 * to avoid coupling to the canvas struct. */

/* Simple full-screen quad using fixed-function path isn't available in Core
 * profile.  We declare our own tiny shader program here. */

typedef unsigned int GLuint_t;  /* avoid redeclaration conflict */

/* GLSL sources for the fullscreen textured quad */
static const char* VS_FULLSCREEN =
    "#version 330 core\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    vec2 pos[4] = vec2[4](\n"
    "        vec2(-1,-1), vec2(1,-1), vec2(-1,1), vec2(1,1));\n"
    "    vec2 uvs[4] = vec2[4](\n"
    "        vec2(0,1), vec2(1,1), vec2(0,0), vec2(1,0));\n"
    "    vUV = uvs[gl_VertexID];\n"
    "    gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);\n"
    "}\n";

static const char* FS_FULLSCREEN =
    "#version 330 core\n"
    "in  vec2 vUV;\n"
    "out vec4 FragColor;\n"
    "uniform sampler2D uTexY;\n"
    "uniform sampler2D uTexUV;\n"
    "uniform float     uAlpha;\n"
    "uniform int       uIsNV12;\n"
    "void main() {\n"
    "    if (uIsNV12 == 1) {\n"
    "        float y = texture(uTexY, vUV).r - (16.0 / 255.0);\n"
    "        float u = texture(uTexUV, vUV).r - 0.5;\n"
    "        float v = texture(uTexUV, vUV).g - 0.5;\n"
    "        \n"
    "        // BT.709 Limited Range to Full Range RGB\n"
    "        float r = 1.16438 * y + 1.79274 * v;\n"
    "        float g = 1.16438 * y - 0.21325 * u - 0.53291 * v;\n"
    "        float b = 1.16438 * y + 2.11240 * u;\n"
    "        \n"
    "        FragColor = vec4(r, g, b, uAlpha);\n"
    "    } else {\n"
    "        vec4 c = texture(uTexY, vUV);\n"
    "        FragColor = vec4(c.rgb, c.a * uAlpha);\n"
    "    }\n"
    "}\n";

/* GL 3.3 function pointers we need (loaded lazily from libGL via glXGetProcAddress) */
typedef GLuint (*PFN_glCreateShader)(GLenum);
typedef void   (*PFN_glShaderSource)(GLuint, GLsizei, const char**, const GLint*);
typedef void   (*PFN_glCompileShader)(GLuint);
typedef GLuint (*PFN_glCreateProgram)(void);
typedef void   (*PFN_glAttachShader)(GLuint, GLuint);
typedef void   (*PFN_glLinkProgram)(GLuint);
typedef void   (*PFN_glUseProgram)(GLuint);
typedef GLint  (*PFN_glGetUniformLocation)(GLuint, const char*);
typedef void   (*PFN_glUniform1f)(GLint, float);
typedef void   (*PFN_glUniform1i)(GLint, int);
typedef void   (*PFN_glDeleteShader)(GLuint);
typedef void   (*PFN_glDeleteProgram)(GLuint);
typedef void   (*PFN_glGenVertexArrays)(GLsizei, GLuint*);
typedef void   (*PFN_glBindVertexArray)(GLuint);
typedef void   (*PFN_glDeleteVertexArrays)(GLsizei, const GLuint*);

static PFN_glCreateShader        p_glCreateShader;
static PFN_glShaderSource        p_glShaderSource;
static PFN_glCompileShader       p_glCompileShader;
static PFN_glCreateProgram       p_glCreateProgram;
static PFN_glAttachShader        p_glAttachShader;
static PFN_glLinkProgram         p_glLinkProgram;
static PFN_glUseProgram          p_glUseProgram;
static PFN_glGetUniformLocation  p_glGetUniformLocation;
static PFN_glUniform1f           p_glUniform1f;
static PFN_glUniform1i           p_glUniform1i;
static PFN_glDeleteShader        p_glDeleteShader;
static PFN_glDeleteProgram       p_glDeleteProgram;
static PFN_glGenVertexArrays     p_glGenVertexArrays;
static PFN_glBindVertexArray     p_glBindVertexArray;
static PFN_glDeleteVertexArrays  p_glDeleteVertexArrays;

#define LOAD_P(name) p_##name = (PFN_##name)glXGetProcAddress((const GLubyte*)#name)

static GLuint g_prog_quad = 0;
static GLuint g_vao_quad  = 0;  /* empty VAO for vertexless draw */

static void init_fullscreen_shader(void) {
    LOAD_P(glCreateShader);
    LOAD_P(glShaderSource);
    LOAD_P(glCompileShader);
    LOAD_P(glCreateProgram);
    LOAD_P(glAttachShader);
    LOAD_P(glLinkProgram);
    LOAD_P(glUseProgram);
    LOAD_P(glGetUniformLocation);
    LOAD_P(glUniform1f);
    LOAD_P(glUniform1i);
    LOAD_P(glDeleteShader);
    LOAD_P(glDeleteProgram);
    LOAD_P(glGenVertexArrays);
    LOAD_P(glBindVertexArray);
    LOAD_P(glDeleteVertexArrays);

    if (!p_glCreateShader) { fprintf(stderr, "[Wallpaper] GL3 functions unavailable\n"); return; }

    GLuint vs = p_glCreateShader(GL_VERTEX_SHADER);
    p_glShaderSource(vs, 1, &VS_FULLSCREEN, NULL);
    p_glCompileShader(vs);

    GLuint fs = p_glCreateShader(GL_FRAGMENT_SHADER);
    p_glShaderSource(fs, 1, &FS_FULLSCREEN, NULL);
    p_glCompileShader(fs);

    g_prog_quad = p_glCreateProgram();
    p_glAttachShader(g_prog_quad, vs);
    p_glAttachShader(g_prog_quad, fs);
    p_glLinkProgram(g_prog_quad);

    p_glDeleteShader(vs);
    p_glDeleteShader(fs);

    /* Empty VAO — draw 4 vertices with gl_VertexID trick */
    p_glGenVertexArrays(1, &g_vao_quad);
}

/* ── Draw a texture as a full-screen quad with alpha ────────────────────── */

static void draw_fullscreen_tex(GLuint tex_y, GLuint tex_uv, bool is_nv12, float alpha) {
    if (!g_prog_quad || !tex_y) return;
    p_glUseProgram(g_prog_quad);
    
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_y);
    p_glUniform1i(p_glGetUniformLocation(g_prog_quad, "uTexY"), 0);
    
    if (is_nv12 && tex_uv) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, tex_uv);
        p_glUniform1i(p_glGetUniformLocation(g_prog_quad, "uTexUV"), 1);
    }
    
    p_glUniform1i(p_glGetUniformLocation(g_prog_quad, "uIsNV12"), is_nv12 ? 1 : 0);
    p_glUniform1f(p_glGetUniformLocation(g_prog_quad, "uAlpha"), alpha);
    
    p_glBindVertexArray(g_vao_quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    
    /* Reset active texture just in case */
    glActiveTexture(GL_TEXTURE0);
}

/* ── Load an image file into a GL texture via GStreamer ──────────────────── */

static GLuint load_image_texture(const char* path, int* out_w, int* out_h) {
    char desc[4096];
    snprintf(desc, sizeof(desc),
        "filesrc location=\"%s\" "
        "! decodebin "
        "! videoconvert "
        "! video/x-raw,format=RGBA "
        "! appsink name=sink max-buffers=1 sync=false emit-signals=false",
        path);

    GError*    err  = NULL;
    GstElement* pipe = gst_parse_launch(desc, &err);
    if (!pipe || err) {
        fprintf(stderr, "[Wallpaper] Cannot load image '%s': %s\n",
                path, err ? err->message : "?");
        if (err) g_error_free(err);
        return 0;
    }

    GstAppSink* sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipe), "sink"));
    if (!sink) { gst_object_unref(pipe); return 0; }

    gst_element_set_state(pipe, GST_STATE_PLAYING);

    /* Pull the first (and only) sample — block up to 5 s */
    GstSample* sample = gst_app_sink_pull_sample(sink);
    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(pipe);

    if (!sample) { fprintf(stderr, "[Wallpaper] No sample from '%s'\n", path); return 0; }

    GstCaps*      caps = gst_sample_get_caps(sample);
    GstStructure* st   = gst_caps_get_structure(caps, 0);
    int w = 0, h = 0;
    gst_structure_get_int(st, "width",  &w);
    gst_structure_get_int(st, "height", &h);

    GstBuffer* buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return 0;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, map.data);

    gst_buffer_unmap(buf, &map);
    gst_sample_unref(sample);

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    printf("[Wallpaper] Loaded image '%s' (%dx%d) → texture %u\n", path, w, h, tex);
    return tex;
}

/* ── Load wallpaper (internal) ─────────────────────────────────────────────── */

static void load_wallpaper(const char* path) {
    if (!path || !path[0]) return;

    /* Avoid redundant reload */
    if (g_current_path && strcmp(g_current_path, path) == 0) return;

    /* Load new texture */
    int  new_w = 0, new_h = 0;
    GLuint new_tex = load_image_texture(path, &new_w, &new_h);
    if (!new_tex) return;

    /* Discard old "previous" texture */
    if (g_tex_prev) {
        glDeleteTextures(1, &g_tex_prev);
        g_tex_prev = 0;
    }

    /* Move current → previous */
    if (g_tex_current) {
        g_tex_prev   = g_tex_current;
        g_tex_prev_w = g_tex_cur_w;
        g_tex_prev_h = g_tex_cur_h;

        /* Start transition */
        g_in_transition      = true;
        g_transition_start   = now_sec();
        g_anim_type          = config_get_int("Desktop", "WallpaperAnim", 0);
    }

    g_tex_current  = new_tex;
    g_tex_cur_w    = new_w;
    g_tex_cur_h    = new_h;

    free(g_current_path);
    g_current_path = strdup(path);
}

/* ── Public API ─────────────────────────────────────────────────────────────── */

void wallpaper_init(void) {
    init_fullscreen_shader();
    printf("[Wallpaper] Initialised\n");
}

void wallpaper_destroy(void) {
    if (g_tex_current) glDeleteTextures(1, &g_tex_current);
    if (g_tex_prev)    glDeleteTextures(1, &g_tex_prev);
    if (g_prog_quad)   p_glDeleteProgram(g_prog_quad);
    if (g_vao_quad)    p_glDeleteVertexArrays(1, &g_vao_quad);
    free(g_current_path);
    g_current_path = NULL;

    if (g_inotify_fd >= 0) { close(g_inotify_fd); g_inotify_fd = -1; }
}

void wallpaper_resize(int w, int h) {
    screen_w = w;
    screen_h = h;
}

/* inotify monitoring — watch the DIRECTORY so atomic rename() doesn't break us */
void init_wallpaper_monitor(void) {
    char* file_path = get_vaxp_main_config_path();
    if (!file_path) return;

    /* Split into directory + basename */
    char dir_path[4096];
    snprintf(dir_path, sizeof(dir_path), "%s", file_path);
    char* slash = strrchr(dir_path, '/');
    if (slash) {
        /* Save just the filename for filtering events */
        snprintf(g_watch_filename, sizeof(g_watch_filename), "%s", slash + 1);
        *slash = '\0'; /* dir_path is now the parent directory */
    } else {
        snprintf(g_watch_filename, sizeof(g_watch_filename), "%s", dir_path);
        snprintf(dir_path, sizeof(dir_path), ".");
    }

    g_inotify_fd = inotify_init1(IN_NONBLOCK);
    if (g_inotify_fd >= 0) {
        /* Watch the directory: survives atomic rename() unlike file watches */
        g_inotify_wd = inotify_add_watch(g_inotify_fd, dir_path,
                                          IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO);
        if (g_inotify_wd < 0)
            fprintf(stderr, "[Wallpaper] inotify_add_watch failed for dir %s\n", dir_path);
    }
    free(file_path);
}

int wallpaper_monitor_fd(void) { return g_inotify_fd; }

bool wallpaper_monitor_fd_ready(void) {
    if (g_inotify_fd < 0) return false;

    /* Read inotify events — must use proper struct layout (variable-length name) */
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool got = false;
    ssize_t len;
    while ((len = read(g_inotify_fd, buf, sizeof(buf))) > 0) {
        const char* p = buf;
        while (p < buf + len) {
            const struct inotify_event* ev = (const struct inotify_event*)p;
            /* Only trigger a reload if the event is for our config file */
            if (ev->len > 0 && strcmp(ev->name, g_watch_filename) == 0)
                got = true;
            else if (ev->len == 0)
                got = true; /* no name = direct file watch (shouldn't happen, but safe) */
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
    return got;
}

void load_saved_wallpaper(void) {
    char* path = config_get_string("Desktop", "Wallpaper", NULL);
    if (!path) return;

    /* Validate: must start with '/' */
    if (path[0] != '/') { free(path); return; }

    if (is_video_file(path)) {
        /* Video wallpaper */
        if (g_tex_current) { glDeleteTextures(1, &g_tex_current); g_tex_current = 0; }
        if (g_tex_prev)    { glDeleteTextures(1, &g_tex_prev);    g_tex_prev    = 0; }
        layer_manager_show_video();
        video_wallpaper_load(path);
    } else {
        /* Static image — stop any running video first */
        if (video_wallpaper_is_active()) video_wallpaper_stop();
        /* Clear cached path so load_wallpaper always reloads after a video */
        if (!g_tex_current) { free(g_current_path); g_current_path = NULL; }
        layer_manager_show_image();
        load_wallpaper(path);
    }

    free(path);
}

/* ── Render ─────────────────────────────────────────────────────────────────── */

void wallpaper_render(float elapsed_sec) {
    (void)elapsed_sec;  /* kept for API compat; we use internal clock */

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glViewport(0, 0, screen_w, screen_h);

    WallpaperLayer layer = layer_manager_current();

    if (layer == LAYER_VIDEO) {
        /* ── Video layer ── */
        GLuint vtex_y = 0, vtex_uv = 0; int vw = 0, vh = 0;
        bool new_frame = video_wallpaper_update_texture(&vtex_y, &vtex_uv, &vw, &vh);

        /* If no new frame but we have a cached texture, still present it */
        if (!new_frame && g_tex_prev == 0) {
            /* Use the video module's last known texture without pulling */
            video_wallpaper_get_texture(&vtex_y, &vtex_uv, &vw, &vh);
        }
        if (!vtex_y) return;  /* nothing to draw yet */

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        draw_fullscreen_tex(vtex_y, vtex_uv, true, 1.0f);
        return;
    }

    /* ── Static image layer ── */
    if (!g_tex_current) return;  /* no wallpaper loaded yet */

    /* Calculate transition progress */
    double progress = 1.0;
    if (g_in_transition && g_tex_prev) {
        double elapsed = now_sec() - g_transition_start;
        progress = elapsed / TRANSITION_DURATION;
        if (progress >= 1.0) {
            progress      = 1.0;
            g_in_transition = false;
            glDeleteTextures(1, &g_tex_prev);
            g_tex_prev = 0;
        }
    }

    /* Draw current wallpaper */
    draw_fullscreen_tex(g_tex_current, 0, false, 1.0f);

    /* Draw outgoing (previous) wallpaper on top with decreasing alpha */
    if (g_in_transition && g_tex_prev) {
        float prev_alpha = (float)(1.0 - progress);

        switch (g_anim_type) {
            case 2:  /* Crossfade */
            default:
                draw_fullscreen_tex(g_tex_prev, 0, false, prev_alpha);
                break;

            /* For more complex transitions (wipe, zoom, etc.) we'd need
             * additional clip/scissor or transform logic.  For now they
             * all fall back to a clean crossfade — easy to extend later. */
            case 0:  /* Sliding Doors → crossfade fallback */
            case 1:  /* Circle Reveal → crossfade fallback */
            case 3:  /* Wipe Right    → crossfade fallback */
            case 4:  /* Zoom Out      → crossfade fallback */
            case 5:  /* Blinds        → crossfade fallback */
            case 6:  /* Swipe Up      → crossfade fallback */
            case 7:  /* Grid/Mosaic   → crossfade fallback */
            case 8:  /* Diagonal Wipe → crossfade fallback */
            case 9:  /* Spin & Fade   → crossfade fallback */
                draw_fullscreen_tex(g_tex_prev, 0, false, prev_alpha);
                break;
        }
    }
}
