#include "audio_analyzer.h"
#include <pthread.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define RATE       44100
#define CHUNK      512
#define SMOOTH     0.80f

static pthread_t g_thread;
static volatile int g_running = 0;
static volatile float g_current_level = 0.0f;

static char* get_monitor_src(void) {
    FILE *f = popen("pactl get-default-sink 2>/dev/null", "r");
    if (!f) return strdup("auto_null.monitor");
    char buf[256] = {0};
    if (fgets(buf, sizeof buf, f))
        buf[strcspn(buf, "\n\r")] = 0;
    pclose(f);
    if (buf[0]) {
        char out[512];
        snprintf(out, sizeof(out), "%s.monitor", buf);
        return strdup(out);
    }
    return strdup("auto_null.monitor");
}

static void* audio_thread(void* arg) {
    (void)arg;
    pa_sample_spec ss = { PA_SAMPLE_S16LE, RATE, 1 };
    
    pa_buffer_attr ba;
    ba.maxlength = (uint32_t)-1;
    ba.tlength   = (uint32_t)-1;
    ba.prebuf    = (uint32_t)-1;
    ba.minreq    = (uint32_t)-1;
    ba.fragsize  = sizeof(int16_t) * CHUNK;

    int err = 0;
    char *src = get_monitor_src();
    fprintf(stderr, "[AudioAnalyzer] monitor: %s\n", src);
    
    pa_simple *pa = pa_simple_new(NULL, "vaxp-background", PA_STREAM_RECORD,
                                  src, "spectrum", &ss, NULL, &ba, &err);
    free(src);

    if (!pa) {
        fprintf(stderr, "[AudioAnalyzer] pa_simple_new error: %s\n", pa_strerror(err));
        return NULL;
    }

    int16_t tmp[CHUNK];
    float level = 0.0f;

    while (g_running) {
        if (pa_simple_read(pa, tmp, sizeof tmp, &err) < 0) {
            fprintf(stderr, "[AudioAnalyzer] read error\n");
            break;
        }

        float sum = 0.0f;
        for (int i = 0; i < CHUNK; i++) {
            float v = tmp[i] / 32768.0f;
            sum += v * v;
        }
        float rms = sqrtf(sum / CHUNK);
        // Boost RMS for visual effect, cap at 1.0
        float mag = rms * 6.0f;
        if (mag > 1.0f) mag = 1.0f;
        
        // Exponential smoothing (SMOOTH = 0.8 means retain 80% of old level, 20% new)
        level = level * SMOOTH + mag * (1.0f - SMOOTH);
        g_current_level = level;
    }

    pa_simple_free(pa);
    return NULL;
}

void audio_analyzer_start(void) {
    if (g_running) return;
    g_running = 1;
    pthread_create(&g_thread, NULL, audio_thread, NULL);
}

void audio_analyzer_stop(void) {
    if (!g_running) return;
    g_running = 0;
    pthread_join(g_thread, NULL);
}

float audio_analyzer_get_level(void) {
    return g_current_level;
}
