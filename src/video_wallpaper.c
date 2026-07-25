/*
 * video_wallpaper.c
 * Video wallpaper — GStreamer appsink → OpenGL texture.
 *
 * ╔══════════════════════════════════════════════════════════════╗
 * ║  ZERO-COPY PATH  (preferred, when EGL context is available) ║
 * ╠══════════════════════════════════════════════════════════════╣
 * ║                                                              ║
 * ║  GStreamer pipeline:                                         ║
 * ║    filesrc ! parsebin ! vaapidecodebin                       ║
 * ║    ! glupload ! glcolorconvert                               ║
 * ║    ! video/x-raw(memory:GLMemory),format=RGBA ! appsink      ║
 * ║                                                              ║
 * ║  Frame path:  GPU(VA-API decode) → EGLImage → GL Texture     ║
 * ║  CPU involvement: ZERO                                       ║
 * ║  Memory copies:   ZERO                                       ║
 * ║                                                              ║
 * ║  Mechanism:                                                  ║
 * ║   • We share our EGLDisplay+EGLContext with GStreamer GL.    ║
 * ║   • glupload imports the VASurface as an EGLImage and binds  ║
 * ║     it directly to a GL texture (same GPU VRAM, no copy).   ║
 * ║   • glcolorconvert runs a YUV→RGBA GLSL shader on the GPU.  ║
 * ║   • We extract the GL texture ID from GstGLMemory and use   ║
 * ║     it in our renderer — no upload needed.                   ║
 * ║                                                              ║
 * ╠══════════════════════════════════════════════════════════════╣
 * ║  FALLBACK PATH  (PBO double-buffered upload)                 ║
 * ╠══════════════════════════════════════════════════════════════╣
 * ║                                                              ║
 * ║  GStreamer pipeline:                                         ║
 * ║    filesrc ! decodebin ! videoconvert                        ║
 * ║    ! video/x-raw,format=RGBA ! appsink                       ║
 * ║                                                              ║
 * ║  Frame path:  GPU decode → RAM → PBO DMA → VRAM             ║
 * ║                                                              ║
 * ╚══════════════════════════════════════════════════════════════╝
 */

#include "video_wallpaper.h"
#include "desktop_config.h"

/* GStreamer core */
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

/* GStreamer GL — for zero-copy path */
#include <gst/gl/gl.h>
#include <gst/gl/egl/gstgldisplay_egl.h>

/* EGL */
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── GL extension function pointers for PBO (resolved once at init) ─────── */

typedef void  (*PFNGLGENBUFFERSPROC)    (GLsizei, GLuint*);
typedef void  (*PFNGLBINDBUFFERPROC)    (GLenum,  GLuint);
typedef void  (*PFNGLBUFFERDATAPROC)    (GLenum,  GLsizeiptr, const void*, GLenum);
typedef void* (*PFNGLMAPBUFFERPROC)     (GLenum,  GLenum);
typedef GLboolean (*PFNGLUNMAPBUFFERPROC)(GLenum);
typedef void  (*PFNGLDELETEBUFFERSPROC) (GLsizei, const GLuint*);

static PFNGLGENBUFFERSPROC    gl_GenBuffers    = NULL;
static PFNGLBINDBUFFERPROC    gl_BindBuffer    = NULL;
static PFNGLBUFFERDATAPROC    gl_BufferData    = NULL;
static PFNGLMAPBUFFERPROC     gl_MapBuffer     = NULL;
static PFNGLUNMAPBUFFERPROC   gl_UnmapBuffer   = NULL;
static PFNGLDELETEBUFFERSPROC gl_DeleteBuffers = NULL;
static bool g_pbo_available = false;

#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW         0x88E0
#endif
#ifndef GL_WRITE_ONLY
#define GL_WRITE_ONLY          0x88B9
#endif

/* ── Private state ─────────────────────────────────────────────────────────── */

/* GStreamer GL objects (zero-copy path) */
static GstGLDisplay *g_gst_display  = NULL;
static GstGLContext *g_gst_context  = NULL;  /* wrapped: our EGL context */
static bool          g_gl_zero_copy = false;

/* GStreamer pipeline */
static GstElement*  g_pipeline  = NULL;
static GstAppSink*  g_appsink   = NULL;
static bool         g_active    = false;
static char*        g_cur_path  = NULL;

/* ── GL Memory path: current sample (holds texture alive) ── */
static GstSample*   g_gl_sample = NULL;  /* unref when replaced */

/* ── PBO path: owned GL texture ── */
static GLuint       g_tex       = 0;
static int          g_tex_w     = 0;
static int          g_tex_h     = 0;

/* Double-buffered PBOs */
#define PBO_COUNT 2
static GLuint g_pbo[PBO_COUNT]  = {0, 0};
static int    g_pbo_idx         = 0;
static int    g_pbo_size        = 0;

/* ── PBO helpers ──────────────────────────────────────────────────────────── */

static void pbo_ensure(int w, int h) {
    if (!g_pbo_available) return;
    int sz = w * h * 4;
    if (sz == g_pbo_size && g_pbo[0]) return;
    if (g_pbo[0]) { gl_DeleteBuffers(PBO_COUNT, g_pbo); g_pbo[0] = g_pbo[1] = 0; }
    gl_GenBuffers(PBO_COUNT, g_pbo);
    for (int i = 0; i < PBO_COUNT; i++) {
        gl_BindBuffer(GL_PIXEL_UNPACK_BUFFER, g_pbo[i]);
        gl_BufferData(GL_PIXEL_UNPACK_BUFFER, sz, NULL, GL_STREAM_DRAW);
    }
    gl_BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    g_pbo_size = sz;
    g_pbo_idx  = 0;
}

static void pbo_release(void) {
    if (!g_pbo_available || !g_pbo[0]) return;
    gl_BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    gl_DeleteBuffers(PBO_COUNT, g_pbo);
    g_pbo[0] = g_pbo[1] = 0;
    g_pbo_size = 0;
}

static void pbo_upload(int w, int h, const void* pixels) {
    int read_idx  = g_pbo_idx;
    int write_idx = (g_pbo_idx + 1) % PBO_COUNT;
    gl_BindBuffer(GL_PIXEL_UNPACK_BUFFER, g_pbo[write_idx]);
    void* dst = gl_MapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
    if (dst) { memcpy(dst, pixels, (size_t)(w * h * 4)); gl_UnmapBuffer(GL_PIXEL_UNPACK_BUFFER); }
    gl_BindBuffer(GL_PIXEL_UNPACK_BUFFER, g_pbo[read_idx]);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gl_BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    g_pbo_idx = write_idx;
}

/* ── Pipeline helpers ────────────────────────────────────────────────────── */

static void pipeline_destroy(void) {
    if (g_pipeline) {
        gst_element_set_state(g_pipeline, GST_STATE_NULL);
        gst_object_unref(g_pipeline);
        g_pipeline = NULL;
        g_appsink  = NULL;
    }
    /* Release GL Memory sample — GStreamer now owns the texture again */
    if (g_gl_sample) { gst_sample_unref(g_gl_sample); g_gl_sample = NULL; }
}

/* Inject our EGL context into the pipeline so glupload can share it */
static void inject_gl_context(GstElement* pipeline) {
    if (!g_gst_display || !g_gst_context) return;

    GstContext* disp_ctx = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
    gst_structure_set(gst_context_writable_structure(disp_ctx),
        "display", GST_TYPE_GL_DISPLAY, g_gst_display, NULL);
    gst_element_set_context(pipeline, disp_ctx);
    gst_context_unref(disp_ctx);

    GstContext* app_ctx = gst_context_new("gst.gl.app_context", TRUE);
    gst_structure_set(gst_context_writable_structure(app_ctx),
        "context", GST_TYPE_GL_CONTEXT, g_gst_context, NULL);
    gst_element_set_context(pipeline, app_ctx);
    gst_context_unref(app_ctx);
}

/* ── Public API ────────────────────────────────────────────────────────────── */

void video_wallpaper_init(void* egl_display, void* egl_context) {

    /* ── Load PBO extension pointers (fallback path) ── */
    typedef void* (*ProcLoader)(const char*);
    ProcLoader loader = (ProcLoader)eglGetProcAddress;
    if (loader) {
        gl_GenBuffers    = (PFNGLGENBUFFERSPROC)    loader("glGenBuffers");
        gl_BindBuffer    = (PFNGLBINDBUFFERPROC)    loader("glBindBuffer");
        gl_BufferData    = (PFNGLBUFFERDATAPROC)    loader("glBufferData");
        gl_MapBuffer     = (PFNGLMAPBUFFERPROC)     loader("glMapBuffer");
        gl_UnmapBuffer   = (PFNGLUNMAPBUFFERPROC)   loader("glUnmapBuffer");
        gl_DeleteBuffers = (PFNGLDELETEBUFFERSPROC)  loader("glDeleteBuffers");
        g_pbo_available = (gl_GenBuffers && gl_BindBuffer && gl_BufferData &&
                           gl_MapBuffer && gl_UnmapBuffer && gl_DeleteBuffers);
    }

    /* ── Attempt zero-copy GL Memory path ── */
    if (!egl_display || !egl_context) {
        printf("[VideoWallpaper] No EGL context — using PBO fallback\n");
        return;
    }

    EGLDisplay edpy = (EGLDisplay)egl_display;
    EGLContext ectx = (EGLContext)egl_context;

    /* ── Create GStreamer GL display wrapping our EGL display ── */
    g_gst_display = GST_GL_DISPLAY(
        gst_gl_display_egl_new_with_egl_display(edpy));
    if (!g_gst_display) {
        printf("[VideoWallpaper] GstGLDisplayEGL failed — using PBO fallback\n");
        return;
    }

    /* ── Create a NEW EGL context that SHARES objects with ours ────────────────
     * CRITICAL: We must NOT give GStreamer our main EGL context directly.
     * An EGL context can only be current on ONE thread at a time. Our main
     * rendering thread keeps our context current, so GStreamer's internal GL
     * thread would fail to make it current — producing blank textures.
     *
     * Instead, we create a new EGL context that shares the object namespace
     * (textures, buffers) with our main context. GStreamer uses this new
     * context on its own thread; our main thread keeps using ours. Textures
     * created by GStreamer's glupload/glcolorconvert are immediately visible
     * in our rendering context because they share the same GPU object names.
     * ─────────────────────────────────────────────────────────────────────── */

    /* Find our EGL config by querying the config ID from our context */
    EGLint config_id = 0;
    eglQueryContext(edpy, ectx, EGL_CONFIG_ID, &config_id);

    EGLConfig shared_cfg = NULL;
    if (config_id) {
        EGLConfig all_cfgs[64];
        EGLint n = 0;
        eglGetConfigs(edpy, all_cfgs, 64, &n);
        for (EGLint i = 0; i < n; i++) {
            EGLint id = 0;
            eglGetConfigAttrib(edpy, all_cfgs[i], EGL_CONFIG_ID, &id);
            if (id == config_id) { shared_cfg = all_cfgs[i]; break; }
        }
    }

    if (!shared_cfg) {
        /* Fallback: find any suitable config */
        static const EGLint fa[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_NONE
        };
        EGLint n = 0;
        eglChooseConfig(edpy, fa, &shared_cfg, 1, &n);
    }

    /* Create the shared context for GStreamer */
    static const EGLint gst_ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    EGLContext gst_egl_ctx = eglCreateContext(
        edpy,
        shared_cfg,
        ectx,            /* share group: our main context */
        gst_ctx_attribs);

    if (gst_egl_ctx == EGL_NO_CONTEXT) {
        gst_object_unref(g_gst_display); g_gst_display = NULL;
        printf("[VideoWallpaper] Shared EGL context creation failed — using PBO fallback\n");
        return;
    }

    /* Wrap the NEW shared context (not our main context) */
    g_gst_context = gst_gl_context_new_wrapped(
        g_gst_display,
        (guintptr)gst_egl_ctx,
        GST_GL_PLATFORM_EGL,
        GST_GL_API_OPENGL3);

    if (!g_gst_context) {
        eglDestroyContext(edpy, gst_egl_ctx);
        gst_object_unref(g_gst_display); g_gst_display = NULL;
        printf("[VideoWallpaper] GstGLContext wrap failed — using PBO fallback\n");
        return;
    }

    g_gl_zero_copy = true;
    printf("[VideoWallpaper] GL Memory zero-copy: ready "
           "(GPU Decode → EGLImage → GL Texture, 0 CPU copies)\n");
}

bool is_video_file(const char* path) {
    if (!path || !*path) return false;
    static const char* const exts[] = {
        ".mp4", ".mkv", ".webm", ".avi", ".mov", ".flv",
        ".wmv", ".m4v",  ".ogv",  ".ts",  ".m2ts", ".mpg",
        ".mpeg", ".3gp", ".hevc", NULL
    };
    const char* dot = strrchr(path, '.');
    if (!dot) return false;
    for (int i = 0; exts[i]; i++) {
        const char* a = dot; const char* b = exts[i]; bool ok = true;
        while (*a && *b) { char ca = (*a>='A'&&*a<='Z')?*a+32:*a; if(ca!=*b){ok=false;break;} a++;b++; }
        if (ok && !*a && !*b) return true;
    }
    return false;
}

void video_wallpaper_load(const char* path) {
    if (!path || !*path) return;
    if (g_active && g_cur_path && strcmp(g_cur_path, path) == 0) return;

    pipeline_destroy();
    pbo_release();
    free(g_cur_path);
    g_cur_path = strdup(path);
    g_active   = true;

    if (g_tex) { glDeleteTextures(1, &g_tex); g_tex = 0; g_tex_w = g_tex_h = 0; }

    int vol = config_get_int("Desktop", "VideoVolume", 50);
    (void)vol;

    GError* err = NULL;

    /* ════════════════════════════════════════════════════════════
     * PIPELINE 1: GL Memory zero-copy
     * GPU Decode (VA-API) → glupload (EGLImage) → glcolorconvert (GLSL YUV→RGBA)
     * → GLMemory texture (stays in VRAM — zero CPU involvement)
     * ════════════════════════════════════════════════════════════ */
    if (g_gl_zero_copy) {
        char desc_gl[4096];
        snprintf(desc_gl, sizeof(desc_gl),
            "filesrc location=\"%s\" "
            "! parsebin "
            "! vaapidecodebin "
            "! glupload "
            "! glcolorconvert "
            "! video/x-raw(memory:GLMemory),format=RGBA "
            "! appsink name=sink max-buffers=1 drop=true sync=true emit-signals=false",
            path);

        g_pipeline = gst_parse_launch(desc_gl, &err);
        if (err) { g_error_free(err); err = NULL; g_pipeline = NULL; }

        if (g_pipeline) {
            /* Inject our EGL context BEFORE going to READY */
            inject_gl_context(g_pipeline);

            GstStateChangeReturn ret =
                gst_element_set_state(g_pipeline, GST_STATE_READY);
            if (ret == GST_STATE_CHANGE_FAILURE) {
                gst_element_set_state(g_pipeline, GST_STATE_NULL);
                gst_object_unref(g_pipeline);
                g_pipeline = NULL;
                printf("[VideoWallpaper] GL Memory path failed → trying VA-API+PBO\n");
            } else {
                printf("[VideoWallpaper] Zero-copy GL Memory path active ✓\n");
            }
        }
    }

    /* ════════════════════════════════════════════════════════════
     * PIPELINE 2: VA-API hardware decode + PBO upload
     * GPU Decode (VA-API) → RAM (videoconvert) → PBO DMA → VRAM
     * ════════════════════════════════════════════════════════════ */
    if (!g_pipeline) {
        char desc_hw[4096];
        snprintf(desc_hw, sizeof(desc_hw),
            "filesrc location=\"%s\" "
            "! parsebin ! vaapidecodebin ! videoconvert "
            "! video/x-raw,format=RGBA "
            "! appsink name=sink max-buffers=1 drop=true sync=true emit-signals=false",
            path);
        g_pipeline = gst_parse_launch(desc_hw, &err);
        if (err) { g_error_free(err); err = NULL; g_pipeline = NULL; }
        if (g_pipeline) {
            GstStateChangeReturn ret =
                gst_element_set_state(g_pipeline, GST_STATE_READY);
            if (ret == GST_STATE_CHANGE_FAILURE) {
                gst_element_set_state(g_pipeline, GST_STATE_NULL);
                gst_object_unref(g_pipeline); g_pipeline = NULL;
            } else {
                printf("[VideoWallpaper] VA-API + PBO double-buffer upload\n");
            }
        }
    }

    /* ════════════════════════════════════════════════════════════
     * PIPELINE 3: Pure software fallback
     * ════════════════════════════════════════════════════════════ */
    if (!g_pipeline) {
        char desc_sw[4096];
        snprintf(desc_sw, sizeof(desc_sw),
            "filesrc location=\"%s\" "
            "! decodebin ! videoconvert "
            "! video/x-raw,format=RGBA "
            "! appsink name=sink max-buffers=1 drop=true sync=true emit-signals=false",
            path);
        g_pipeline = gst_parse_launch(desc_sw, &err);
        if (!g_pipeline || err) {
            fprintf(stderr, "[VideoWallpaper] All pipelines failed: %s\n",
                    err ? err->message : "unknown");
            if (err) g_error_free(err);
            g_active = false;
            return;
        }
        printf("[VideoWallpaper] Software decode (CPU fallback)\n");
    }

    g_appsink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(g_pipeline), "sink"));
    if (!g_appsink) {
        fprintf(stderr, "[VideoWallpaper] Cannot find appsink\n");
        pipeline_destroy(); g_active = false; return;
    }

    gst_element_set_state(g_pipeline, GST_STATE_PLAYING);
    printf("[VideoWallpaper] Loaded: %s\n", path);
}

void video_wallpaper_stop(void) {
    if (!g_active) return;
    g_active = false;
    pipeline_destroy();
    pbo_release();
    free(g_cur_path); g_cur_path = NULL;
    if (g_tex) { glDeleteTextures(1, &g_tex); g_tex = 0; g_tex_w = g_tex_h = 0; }
    printf("[VideoWallpaper] Stopped\n");
}

bool video_wallpaper_is_active(void) { return g_active; }

void video_wallpaper_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    config_set_int("Desktop", "VideoVolume", volume);
    if (g_pipeline) {
        GstElement* vol_el = gst_bin_get_by_name(GST_BIN(g_pipeline), "vol");
        if (vol_el) {
            g_object_set(vol_el, "volume", (double)volume / 100.0, NULL);
            gst_object_unref(vol_el);
        }
    }
}

bool video_wallpaper_update_texture(GLuint* tex_out, int* w_out, int* h_out) {
    if (!g_active || !g_appsink) return false;

    GstSample* sample = gst_app_sink_try_pull_sample(g_appsink, 0);
    if (!sample) {
        /* EOS → loop */
        if (gst_app_sink_is_eos(g_appsink))
            gst_element_seek_simple(g_pipeline, GST_FORMAT_TIME,
                GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, 0);
        return false;
    }

    GstBuffer* buf  = gst_sample_get_buffer(sample);
    GstCaps*   caps = gst_sample_get_caps(sample);
    if (!buf || !caps) { gst_sample_unref(sample); return false; }

    GstStructure* st = gst_caps_get_structure(caps, 0);
    int w = 0, h = 0;
    gst_structure_get_int(st, "width",  &w);
    gst_structure_get_int(st, "height", &h);
    if (w <= 0 || h <= 0) { gst_sample_unref(sample); return false; }

    /* ── GL Memory zero-copy path ─────────────────────────────────────────── */
    GstMemory* mem = gst_buffer_peek_memory(buf, 0);
    if (g_gl_zero_copy && mem && gst_is_gl_memory(mem)) {
        GstGLMemory* gl_mem = (GstGLMemory*)mem;

        /* Wait for GStreamer's GL commands to land before sampling in our ctx */
        GstGLSyncMeta* sync = gst_buffer_get_gl_sync_meta(buf);
        if (sync) gst_gl_sync_meta_wait_cpu(sync, g_gst_context);

        GLuint tex = gst_gl_memory_get_texture_id(gl_mem);

        /* Keep sample alive — releasing it lets GStreamer recycle the texture */
        if (g_gl_sample) gst_sample_unref(g_gl_sample);
        g_gl_sample = sample;   /* take ownership — do NOT unref here */

        g_tex   = tex;
        g_tex_w = w;
        g_tex_h = h;

        *tex_out = tex;
        *w_out   = w;
        *h_out   = h;
        return true;
    }

    /* ── PBO / CPU upload path ─────────────────────────────────────────────── */
    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) {
        gst_sample_unref(sample); return false;
    }

    /* Create or resize texture */
    if (g_tex == 0 || g_tex_w != w || g_tex_h != h) {
        if (g_tex) glDeleteTextures(1, &g_tex);
        pbo_release();
        glGenTextures(1, &g_tex);
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, map.data);
        g_tex_w = w; g_tex_h = h;
        pbo_ensure(w, h);
    } else {
        if (g_pbo_available && g_pbo[0])
            pbo_upload(w, h, map.data);
        else {
            glBindTexture(GL_TEXTURE_2D, g_tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                            GL_RGBA, GL_UNSIGNED_BYTE, map.data);
        }
    }

    gst_buffer_unmap(buf, &map);
    gst_sample_unref(sample);

    *tex_out = g_tex;
    *w_out   = g_tex_w;
    *h_out   = g_tex_h;
    return true;
}

bool video_wallpaper_get_texture(GLuint* tex_out, int* w_out, int* h_out) {
    if (!g_active || g_tex == 0) return false;
    *tex_out = g_tex; *w_out = g_tex_w; *h_out = g_tex_h;
    return true;
}
