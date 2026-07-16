/*
 * audio_analyzer_widget.c  –  Radial Audio Analyzer (vaxp desktop widget)
 * System audio via PipeWire/PulseAudio monitor source.
 *
 * - Lock-free double buffer (No Mutex).
 * - Live low-latency PulseAudio stream (fragsize forced).
 * - Safe g_idle_add with atomic flag to prevent queue flooding.
 *
 * Build:
 *   gcc -O3 -shared -fPIC -o audio_analyzer_widget.so audio_analyzer_widget.c \
 *       $(pkg-config --cflags --libs gtk+-3.0 libpulse-simple libpulse) \
 *       -lm -lpthread
 */

#include <gtk/gtk.h>
#include <cairo.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include "vaxp-widget-api.h"

/* ── config ───────────────────────────────────────────────────── */
#define WID_W      560          /* widget width                      */
#define WID_H      280          /* widget height                     */
#define NUM_BARS   80           /* spectrum bars (audio side)        */
#define WAVE_PTS   64           /* visual wave points                */
#define RATE       44100
#define CHUNK      256          /* ~5.8ms per read for live feedback */
#define WIN_N      1024
#define SMOOTH     0.20f        /* Snappy response                   */

/* ── lock-free double buffer ─────────────────────────────────── */
static float     spec_buf[2][NUM_BARS];
static volatile gint  buf_w = 0;
static volatile gint  buf_r = 1;

/* ── state ────────────────────────────────────────────────────── */
typedef struct {
    GtkWidget      *canvas;
    vaxpDesktopAPI *api;

    gboolean  dragging;
    gint      drag_rx, drag_ry;
    gint      drag_wx, drag_wy;

    pa_simple       *pa;
    pthread_t        thread;
    volatile gboolean running;

    gint             redraw_pending;
    gdouble          scale_factor;
    GtkWidget       *config_win;
} WidgetState;

static WidgetState S;

/* ══════════════════════════════════════════════
   Configuration Window
   ══════════════════════════════════════════════ */
static void on_scale_changed(GtkRange *range, gpointer user_data) {
    (void)user_data;
    S.scale_factor = gtk_range_get_value(range);
    gtk_widget_set_size_request(S.canvas,
        (int)(WID_W * S.scale_factor),
        (int)(WID_H * S.scale_factor));
    gtk_widget_queue_draw(S.canvas);
}

static void on_config_close(GtkWidget *w, gpointer data) {
    (void)w; (void)data;
    S.config_win = NULL;
}

static void show_config_window(void) {
    if (S.config_win) {
        gtk_window_present(GTK_WINDOW(S.config_win));
        return;
    }
    S.config_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(S.config_win), "حجم المحلل الصوتي");
    gtk_window_set_default_size(GTK_WINDOW(S.config_win), 300, 100);
    gtk_container_set_border_width(GTK_CONTAINER(S.config_win), 20);
    g_signal_connect(S.config_win, "destroy", G_CALLBACK(on_config_close), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(S.config_win), vbox);

    GtkWidget *label = gtk_label_new("قم بتعديل حجم الودجت:");
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.5, 3.0, 0.1);
    gtk_range_set_value(GTK_RANGE(scale), S.scale_factor);
    g_signal_connect(scale, "value-changed", G_CALLBACK(on_scale_changed), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), scale, TRUE, TRUE, 0);

    gtk_widget_show_all(S.config_win);
}

/* ══════════════════════════════════════════════
   Drag & Drop
   ══════════════════════════════════════════════ */
static gboolean on_card_press(GtkWidget *w, GdkEventButton *ev, gpointer d) {
    (void)d;
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == 1) {
        show_config_window();
        return TRUE;
    }
    if (ev->button != 1) return FALSE;
    S.dragging = TRUE;
    S.drag_rx  = ev->x_root; S.drag_ry = ev->y_root;
    gint wx, wy;
    gtk_widget_translate_coordinates(w, gtk_widget_get_toplevel(w), 0, 0, &wx, &wy);
    S.drag_wx = wx; S.drag_wy = wy;
    return TRUE;
}

static gboolean on_card_motion(GtkWidget *w, GdkEventMotion *ev, gpointer d) {
    (void)d;
    if (!S.dragging || !S.api || !S.api->layout_container) return FALSE;
    GtkWidget *target = w;
    while (target && gtk_widget_get_parent(target) != S.api->layout_container)
        target = gtk_widget_get_parent(target);
    if (target) {
        gtk_layout_move(GTK_LAYOUT(S.api->layout_container), target,
            S.drag_wx + (int)(ev->x_root - S.drag_rx),
            S.drag_wy + (int)(ev->y_root - S.drag_ry));
    }
    return TRUE;
}

static gboolean on_card_release(GtkWidget *w, GdkEventButton *ev, gpointer d) {
    (void)d;
    if (ev->button != 1 || !S.dragging) return FALSE;
    S.dragging = FALSE;
    if (S.api && S.api->save_position && S.api->layout_container) {
        gint x, y;
        gtk_widget_translate_coordinates(w, gtk_widget_get_toplevel(w), 0, 0, &x, &y);
        S.api->save_position("audio_analyzer_widget.so", x, y);
    }
    return TRUE;
}

/* ══════════════════════════════════════════════
   PulseAudio / PipeWire monitor
   ══════════════════════════════════════════════ */
static char *get_monitor_src(void) {
    FILE *f = popen("pactl get-default-sink 2>/dev/null", "r");
    if (!f) return g_strdup("auto_null.monitor");
    char buf[256] = {0};
    if (fgets(buf, sizeof buf, f))
        buf[strcspn(buf, "\n\r")] = 0;
    pclose(f);
    return buf[0] ? g_strdup_printf("%s.monitor", buf)
                  : g_strdup("auto_null.monitor");
}

static float goertzel(const float *b, int N, float kf) {
    double w  = 2.0 * M_PI * kf / N;
    double c2 = 2.0 * cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    for (int i = 0; i < N; i++) {
        s0 = b[i] + c2*s1 - s2;
        s2 = s1; s1 = s0;
    }
    double p = s1*s1 + s2*s2 - c2*s1*s2;
    return p < 0 ? 0.f : (float)(sqrt(p) / N);
}

/* ══════════════════════════════════════════════
   Live Redraw Callback
   ══════════════════════════════════════════════ */
static gboolean do_redraw(gpointer _) {
    (void)_;
    g_atomic_int_set(&S.redraw_pending, 0);
    if (S.canvas) gtk_widget_queue_draw(S.canvas);
    return G_SOURCE_REMOVE;
}

/* ══════════════════════════════════════════════
   Audio thread — Lock-Free & Low Latency
   ══════════════════════════════════════════════ */
static void *audio_thread(void *_) {
    (void)_;
    int16_t  tmp[CHUNK];
    float    ring[WIN_N];
    int      ring_pos = 0;
    int      err = 0;

    float lo = 30.f    / RATE * WIN_N;
    float hi = 18000.f / RATE * WIN_N;

    while (S.running) {
        if (pa_simple_read(S.pa, tmp, sizeof tmp, &err) < 0) break;

        /* Fill ring buffer */
        for (int i = 0; i < CHUNK; i++) {
            ring[ring_pos] = tmp[i] / 32768.f;
            ring_pos = (ring_pos + 1) % WIN_N;
        }

        /* Hann window */
        float win[WIN_N];
        for (int i = 0; i < WIN_N; i++) {
            float h = 0.5f * (1.f - cosf(2.f*(float)M_PI*i/(WIN_N-1)));
            win[i]  = ring[(ring_pos + i) % WIN_N] * h;
        }

        /* Compute into write buffer (lock-free) */
        int w = g_atomic_int_get(&buf_w);
        float *dst = spec_buf[w];

        for (int b = 0; b < NUM_BARS; b++) {
            float t   = (float)b / (NUM_BARS - 1);
            float kf  = lo * powf(hi / lo, t);
            float mag = goertzel(win, WIN_N, kf) * 24.f;
            if (mag > 1.f) mag = 1.f;
            dst[b] = dst[b] * SMOOTH + mag * (1.f - SMOOTH);
        }

        /* Swap read and write pointers atomically */
        int r = g_atomic_int_get(&buf_r);
        g_atomic_int_set(&buf_r, w);
        g_atomic_int_set(&buf_w, r);

        /* Request live redraw if not already pending */
        if (g_atomic_int_compare_and_exchange(&S.redraw_pending, 0, 1)) {
            g_idle_add(do_redraw, NULL);
        }
    }
    return NULL;
}

static gboolean pa_open(void) {
    pa_sample_spec ss = { PA_SAMPLE_S16LE, RATE, 1 };
    
    /* 
     * CRITICAL FIX FOR LIVE AUDIO:
     * Request small fragments (fragsize) from PulseAudio.
     * Without this, PulseAudio buffers ~2 seconds of audio 
     * by default to save power, resulting in 1-2 FPS visually!
     */
    pa_buffer_attr ba;
    ba.maxlength = (uint32_t)-1;
    ba.tlength   = (uint32_t)-1;
    ba.prebuf    = (uint32_t)-1;
    ba.minreq    = (uint32_t)-1;
    ba.fragsize  = sizeof(int16_t) * CHUNK;

    int err = 0;
    char *src = get_monitor_src();
    fprintf(stderr, "[analyzer] monitor: %s (low latency mode)\n", src);
    
    S.pa = pa_simple_new(NULL, "VAXP Analyzer", PA_STREAM_RECORD,
                         src, "spectrum", &ss, NULL, &ba, &err);
    g_free(src);
    if (!S.pa) {
        fprintf(stderr, "[analyzer] pa_simple_new: %s\n", pa_strerror(err));
        return FALSE;
    }
    return TRUE;
}

/* ══════════════════════════════════════════════
   Particle system
   ══════════════════════════════════════════════ */
#define MAX_PARTICLES 120
typedef struct { double x, y, vx, vy, life; int cyan; } Particle;
static Particle g_particles[MAX_PARTICLES];
static int      g_npart = 0;

static void spawn_particle(double x, double y, int cyan) {
    if (g_npart >= MAX_PARTICLES) return;
    Particle *p = &g_particles[g_npart++];
    p->x    = x;
    p->y    = y;
    /* small random velocity upwards */
    p->vx   = ((double)rand()/RAND_MAX - 0.5) * 1.2;
    p->vy   = -((double)rand()/RAND_MAX * 2.4 + 1.2);
    p->life = 1.0;
    p->cyan = cyan;
}

/* ══════════════════════════════════════════════
   Smooth curve helper — quadratic midpoint spline
   pts[n] arrays of x,y; draws via cairo
   ══════════════════════════════════════════════ */
static void smooth_curve(cairo_t *cr,
                         const double *px, const double *py, int n) {
    cairo_move_to(cr, px[0], py[0]);
    for (int i = 1; i < n - 1; i++) {
        double mx = (px[i] + px[i+1]) * 0.5;
        double my = (py[i] + py[i+1]) * 0.5;
        cairo_curve_to(cr, px[i], py[i], px[i], py[i], mx, my);
    }
    cairo_line_to(cr, px[n-1], py[n-1]);
}

/* ══════════════════════════════════════════════
   Draw — Liquid Wave (lock-free direct read)
   ══════════════════════════════════════════════ */
static gboolean on_draw_analyzer(GtkWidget *w, cairo_t *cr, gpointer d) {
    (void)d;
    int W = gtk_widget_get_allocated_width(w);
    int H = gtk_widget_get_allocated_height(w);

    /* Center + scale */
    cairo_translate(cr, W * 0.5, H * 0.5);
    cairo_scale(cr, S.scale_factor, S.scale_factor);

    /* Logical size */
    double cw = WID_W;
    double ch = WID_H;
    double ox = -cw * 0.5;   /* left edge  */
    double oy = -ch * 0.5;   /* top edge   */
    double midY = oy + ch * 0.58;  /* baseline Y */

    /* Read spectrum */
    int    ri   = g_atomic_int_get(&buf_r);
    float *spec = spec_buf[ri];

    /* Build wave values — downsample NUM_BARS → WAVE_PTS */
    double vals[WAVE_PTS];
    for (int i = 0; i < WAVE_PTS; i++) {
        int src = (int)((double)i / (WAVE_PTS - 1) * (NUM_BARS - 1));
        vals[i] = spec[src];
    }

    /* Wave point coordinates */
    double top_x[WAVE_PTS], top_y[WAVE_PTS];
    double bot_x[WAVE_PTS], bot_y[WAVE_PTS];
    for (int i = 0; i < WAVE_PTS; i++) {
        double x  = ox + cw - (double)i / (WAVE_PTS - 1) * cw;  /* right→left */
        double amp = vals[i] * ch * 0.42;
        top_x[i] = x;  top_y[i] = midY - amp;
        bot_x[i] = x;  bot_y[i] = midY + amp * 0.6;
    }

    /* ── Filled liquid area ─────────────────────── */
    cairo_save(cr);
    smooth_curve(cr, top_x, top_y, WAVE_PTS);
    /* close via bottom curve (reversed) */
    for (int i = WAVE_PTS - 1; i >= 0; i--)
        cairo_line_to(cr, bot_x[i], bot_y[i]);
    cairo_close_path(cr);

    cairo_pattern_t *fill = cairo_pattern_create_linear(ox, oy, ox, oy + ch);
    cairo_pattern_add_color_stop_rgba(fill, 0.0,  0.341, 0.910, 0.831, 0.333); /* #57e8d455 */
    cairo_pattern_add_color_stop_rgba(fill, 0.5,  0.655, 0.545, 0.980, 0.200); /* #a78bfa33 */
    cairo_pattern_add_color_stop_rgba(fill, 1.0,  0.957, 0.447, 0.714, 0.031); /* #f472b608 */
    cairo_set_source(cr, fill);
    cairo_fill(cr);
    cairo_pattern_destroy(fill);
    cairo_restore(cr);

    /* ── Top glowing line (cyan) ────────────────── */
    smooth_curve(cr, top_x, top_y, WAVE_PTS);
    cairo_set_source_rgba(cr, 0.341, 0.910, 0.831, 1.0);   /* #57e8d4 */
    cairo_set_line_width(cr, 2.4);
    cairo_stroke(cr);

    /* Glow pass — wider, transparent */
    smooth_curve(cr, top_x, top_y, WAVE_PTS);
    cairo_set_source_rgba(cr, 0.341, 0.910, 0.831, 0.25);
    cairo_set_line_width(cr, 9.0);
    cairo_stroke(cr);

    /* ── Bottom subtle line (violet) ────────────── */
    smooth_curve(cr, bot_x, bot_y, WAVE_PTS);
    cairo_set_source_rgba(cr, 0.655, 0.545, 0.980, 0.60);   /* #a78bfa99 */
    cairo_set_line_width(cr, 1.4);
    cairo_stroke(cr);

    /* ── Baseline ────────────────────────────────── */
    cairo_move_to(cr, ox,      midY);
    cairo_line_to(cr, ox + cw, midY);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.047);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    /* ── Spawn particles on high peaks ──────────── */
    for (int i = 4; i < WAVE_PTS - 4; i += 6) {
        if (vals[i] > 0.72 && (rand() % 10) > 6)
            spawn_particle(top_x[i], top_y[i], (rand() % 2));
    }

    /* ── Draw & update particles ─────────────────── */
    int alive = 0;
    for (int i = 0; i < g_npart; i++) {
        Particle *p = &g_particles[i];
        p->x    += p->vx;
        p->y    += p->vy;
        p->life -= 0.025;
        if (p->life <= 0.0) continue;
        g_particles[alive++] = *p;
        double alpha = p->life > 1.0 ? 1.0 : p->life;
        if (p->cyan)
            cairo_set_source_rgba(cr, 0.341, 0.910, 0.831, alpha);
        else
            cairo_set_source_rgba(cr, 0.957, 0.447, 0.714, alpha);
        cairo_arc(cr, p->x, p->y, 2.4, 0, 2.0 * M_PI);
        cairo_fill(cr);
    }
    g_npart = alive;

    return FALSE;
}

/* ══════════════════════════════════════════════
   Widget Construction
   ══════════════════════════════════════════════ */
static GtkWidget *create_widget(vaxpDesktopAPI *api) {
    memset(&S, 0, sizeof(S));
    memset(spec_buf, 0, sizeof(spec_buf));
    buf_w = 0; buf_r = 1;
    S.api = api;
    S.redraw_pending = 0;
    S.scale_factor = 1.0;
    S.config_win = NULL;

    g_npart = 0;

    S.canvas = gtk_drawing_area_new();
    gtk_widget_set_size_request(S.canvas, WID_W, WID_H);

    gtk_widget_add_events(S.canvas,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);

    g_signal_connect(S.canvas, "button-press-event",   G_CALLBACK(on_card_press),    NULL);
    g_signal_connect(S.canvas, "motion-notify-event",  G_CALLBACK(on_card_motion),   NULL);
    g_signal_connect(S.canvas, "button-release-event", G_CALLBACK(on_card_release),  NULL);
    g_signal_connect(S.canvas, "draw",                 G_CALLBACK(on_draw_analyzer), NULL);

    gtk_widget_show_all(S.canvas);

    S.running = TRUE;
    if (pa_open())
        pthread_create(&S.thread, NULL, audio_thread, NULL);

    return S.canvas;
}

static void destroy_analyzer_liquid(void) {
    S.running = FALSE;
    if (S.pa) {
        pa_simple_free(S.pa);
        S.pa = NULL;
        pthread_join(S.thread, NULL);
    }
}

/* ══════════════════════════════════════════════
   VAXP Entry Point
   ══════════════════════════════════════════════ */
vaxpWidgetAPI *vaxp_widget_init(void) {
    static vaxpWidgetAPI api;
    api.name           = "Audio Analyzer";
    api.description    = "Live radial bars — PulseAudio monitor (Low latency, lock-free)";
    api.author         = "VAXP User";
    api.create_widget  = create_widget;
    api.update_theme   = NULL;
    api.destroy_widget = destroy_analyzer_liquid;
    return &api;
}
