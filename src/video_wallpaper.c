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

static bool          g_gl_zero_copy = false;

/* GStreamer pipeline */
static GstElement*  g_pipeline  = NULL;
static GstAppSink*  g_appsink   = NULL;
static bool         g_active    = false;
static char*        g_cur_path  = NULL;

/* ── GL Memory path: current sample (holds texture alive) ── */
static GstSample*   g_gl_sample = NULL;  /* unref when replaced */

/* ── PBO path: owned GL texture ── */
static GLuint        g_tex_y = 0;
static GLuint        g_tex_uv = 0;
static int           g_tex_w = 0, g_tex_h = 0;

/* PBO state for NV12 upload */
static GLuint g_pbo_y[2] = {0, 0};
static GLuint g_pbo_uv[2] = {0, 0};
static int    g_pbo_idx  = 0;

/* ── PBO helpers ──────────────────────────────────────────────────────────── */

static void pbo_ensure(int w, int h) {
    if (!g_pbo_available) return;
    int sz_y = w * h;
    int sz_uv = w * h / 2;
    if (g_pbo_y[0] && g_pbo_uv[0]) return;
    gl_GenBuffers(2, g_pbo_y);
    gl_GenBuffers(2, g_pbo_uv);
    for (int i = 0; i < 2; i++) {
        gl_BindBuffer(GL_PIXEL_UNPACK_BUFFER, g_pbo_y[i]);
        gl_BufferData(GL_PIXEL_UNPACK_BUFFER, sz_y, NULL, GL_STREAM_DRAW);
        gl_BindBuffer(GL_PIXEL_UNPACK_BUFFER, g_pbo_uv[i]);
        gl_BufferData(GL_PIXEL_UNPACK_BUFFER, sz_uv, NULL, GL_STREAM_DRAW);
    }
    gl_BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    g_pbo_idx = 0;
}

static void pbo_release(void) {
    if (!g_pbo_available || !g_pbo_y[0]) return;
    gl_BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    gl_DeleteBuffers(2, g_pbo_y);
    gl_DeleteBuffers(2, g_pbo_uv);
    g_pbo_y[0] = g_pbo_y[1] = g_pbo_uv[0] = g_pbo_uv[1] = 0;
}

static void pbo_upload_nv12(int w, int h, const void* y, const void* uv) {
    int write_idx = (g_pbo_idx + 1) % 2;
    
    gl_BindBuffer(GL_PIXEL_UNPACK_BUFFER, g_pbo_y[write_idx]);
    void* dst_y = gl_MapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
    if (dst_y) { memcpy(dst_y, y, (size_t)(w * h)); gl_UnmapBuffer(GL_PIXEL_UNPACK_BUFFER); }
    glBindTexture(GL_TEXTURE_2D, g_tex_y);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, NULL);
    
    gl_BindBuffer(GL_PIXEL_UNPACK_BUFFER, g_pbo_uv[write_idx]);
    void* dst_uv = gl_MapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
    if (dst_uv) { memcpy(dst_uv, uv, (size_t)(w * h / 2)); gl_UnmapBuffer(GL_PIXEL_UNPACK_BUFFER); }
    glBindTexture(GL_TEXTURE_2D, g_tex_uv);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w / 2, h / 2, GL_RG, GL_UNSIGNED_BYTE, NULL);
    
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

    if (g_tex_y) {
        glDeleteTextures(1, &g_tex_y);
        glDeleteTextures(1, &g_tex_uv);
        g_tex_y = g_tex_uv = 0; g_tex_w = g_tex_h = 0;
    }

    int vol = config_get_int("Desktop", "VideoVolume", 50);
    (void)vol;

    GError* err = NULL;

    /* ════════════════════════════════════════════════════════════
     * PIPELINE 1: VA-API hardware decode + PBO upload
     * GPU Decode (VA-API) → RAM (videoconvert) → PBO DMA → VRAM
     * (We skip GStreamer glupload as it fails on this system during preroll)
     * ════════════════════════════════════════════════════════════ */
    if (!g_pipeline) {
        char desc_hw[4096];
        snprintf(desc_hw, sizeof(desc_hw),
            "filesrc location=\"%s\" "
            "! parsebin "
            "! vaapidecodebin "
            "! video/x-raw,format=NV12 "
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
            "! decodebin "
            "! video/x-raw,format=NV12 "
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
        printf("[VideoWallpaper] VA-API + PBO double-buffer upload\n");
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
    if (g_tex_y) {
        glDeleteTextures(1, &g_tex_y);
        glDeleteTextures(1, &g_tex_uv);
        g_tex_y = g_tex_uv = 0; g_tex_w = g_tex_h = 0;
    }
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

bool video_wallpaper_update_texture(GLuint* tex_y_out, GLuint* tex_uv_out, int* w_out, int* h_out) {
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


    /* ── PBO / CPU upload path ─────────────────────────────────────────────── */
    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) {
        gst_sample_unref(sample); return false;
    }

    /* Create or resize NV12 textures */
    if (g_tex_y == 0 || g_tex_w != w || g_tex_h != h) {
        if (g_tex_y) {
            glDeleteTextures(1, &g_tex_y);
            glDeleteTextures(1, &g_tex_uv);
        }
        pbo_release();
        
        glGenTextures(1, &g_tex_y);
        glBindTexture(GL_TEXTURE_2D, g_tex_y);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, map.data);
        
        glGenTextures(1, &g_tex_uv);
        glBindTexture(GL_TEXTURE_2D, g_tex_uv);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, w / 2, h / 2, 0, GL_RG, GL_UNSIGNED_BYTE, map.data + (w * h));
        
        g_tex_w = w; g_tex_h = h;
        pbo_ensure(w, h);
    } else {
        /* Let the OpenGL driver handle the upload directly.
         * Our manual memcpy into a PBO uses standard libc memcpy, which is
         * excruciatingly slow when reading from mapped VASurfaces (Uncached/WC memory).
         * The Mesa driver has specialized AVX/SSE paths for glTexSubImage2D. */
        glBindTexture(GL_TEXTURE_2D, g_tex_y);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, map.data);
        
        glBindTexture(GL_TEXTURE_2D, g_tex_uv);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w / 2, h / 2, GL_RG, GL_UNSIGNED_BYTE, map.data + (w * h));
    }

    gst_buffer_unmap(buf, &map);
    gst_sample_unref(sample);

    *tex_y_out  = g_tex_y;
    *tex_uv_out = g_tex_uv;
    *w_out      = g_tex_w;
    *h_out      = g_tex_h;
    return true;
}

bool video_wallpaper_get_texture(GLuint* tex_y_out, GLuint* tex_uv_out, int* w_out, int* h_out) {
    if (!g_active || g_tex_y == 0) return false;
    *tex_y_out  = g_tex_y;
    *tex_uv_out = g_tex_uv;
    *w_out      = g_tex_w;
    *h_out      = g_tex_h;
    return true;
}
