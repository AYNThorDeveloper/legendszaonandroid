/* VI bridge for an activity-owned ANativeWindow. Inert window tokens are
 * restricted to the explicit headless diagnostic mode. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <android/native_window.h>
#include <pthread.h>
#include <time.h>

extern _Noreturn void runtime_fail(const char *message);
extern void hle_bind_system_event(void *event, int autoclear);
extern void hle_signal_system_event(void *event);
static unsigned display_token, layer_token, window_token;
static pthread_mutex_t window_lock=PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *activity_window;
static void *vsync_events[16];
static unsigned vsync_count;
static int activity_focused=1;

extern void runtime_oe_push_message(uint32_t message);
void android_front_set_focus(int focused) {
    pthread_mutex_lock(&window_lock);activity_focused=focused;pthread_mutex_unlock(&window_lock);
    runtime_oe_push_message(15); /* FocusStateChanged, as the macOS frontend sends */
}

int runtime_front_focus(void) {
    pthread_mutex_lock(&window_lock);int focused=activity_focused;pthread_mutex_unlock(&window_lock);
    return focused;
}

static uint64_t last_tick_ns, tick_period_ns;

/* Refresh rate measured from Choreographer callbacks (0 until known). */
int android_front_display_hz(void) {
    pthread_mutex_lock(&window_lock);uint64_t period=tick_period_ns;pthread_mutex_unlock(&window_lock);
    return period?(int)((1000000000ull+period/2)/period):0;
}

int android_front_has_window(void) {
    pthread_mutex_lock(&window_lock);int has=activity_window!=NULL;pthread_mutex_unlock(&window_lock);
    return has;
}

static int vsync_follows_display(void) {   /* TOTK_VSYNC_DISPLAY=1: the game's vsync event follows Choreographer (old) */
    static int on=-1;
    if (on<0) { const char *e=getenv("TOTK_VSYNC_DISPLAY"); on=e&&!strcmp(e,"1"); }
    return on;
}

void android_front_frame_tick(void) {
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    uint64_t now=(uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec;
    pthread_mutex_lock(&window_lock);
    if(last_tick_ns&&now-last_tick_ns<100000000ull) {
        uint64_t d=now-last_tick_ns; /* smoothed; a skipped callback is ignored */
        if(!tick_period_ns)tick_period_ns=d;
        else if(d<tick_period_ns*3/2)tick_period_ns=(tick_period_ns*7+d)/8;
    }
    last_tick_ns=now;
    if (vsync_follows_display() && activity_window) for (unsigned i=0;i<vsync_count;++i)
        hle_signal_system_event(vsync_events[i]);
    pthread_mutex_unlock(&window_lock);
}

/* The console's display vsync event fires at 60 Hz. Z-A's frame clock steps each subsystem by max(its interval, the
 * vsyncs that really passed) (Hexveil404/PokeZ-ARecomp runtime/hle/framepace.c, tools/za_patches.py), so the vsync rate is
 * game semantics, not presentation. Choreographer follows the panel (Thor: up to 120 Hz) and made the game count twice
 * the vsyncs per frame. The event now comes from a fixed 60 Hz clock, as on the Switch, in Hexveil (runtime/hle/vi.c)
 * and in Eden; Choreographer only measures the display rate. TOTK_VSYNC_DISPLAY=1 (before process start): the old
 * Choreographer-driven event, for A/B. */
static uint64_t vsync_signals;
static void *vsync_timer(void *arg) {
    (void)arg;
    const uint64_t period=16666667ull;
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    uint64_t next=(uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec;
    unsigned late=0;
    for (;;) {
        next+=period;
        struct timespec at={(time_t)(next/1000000000ull),(long)(next%1000000000ull)};
        while (clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&at,NULL)==EINTR) {}
        clock_gettime(CLOCK_MONOTONIC,&ts);
        uint64_t now=(uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec;
        if (now>next+period*3) {   /* a long stall: carry on from now instead of burst-signalling to catch up */
            if (late++<20) fprintf(stderr,"[android-front] 60 Hz vsync timer late by %.1f ms\n",(double)(now-next)/1e6);
            next=now;
        }
        pthread_mutex_lock(&window_lock);
        if (activity_window) for (unsigned i=0;i<vsync_count;++i) hle_signal_system_event(vsync_events[i]);
        uint64_t n=++vsync_signals;
        pthread_mutex_unlock(&window_lock);
        if (n==300||n%18000==0)
            fprintf(stderr,"[android-front] vsync event: fixed 60 Hz clock (%llu signals); display measured at %d Hz\n",
                (unsigned long long)n,android_front_display_hz());
    }
    return NULL;
}
static void vsync_timer_create(void) {
    pthread_t t;
    if (pthread_create(&t,NULL,vsync_timer,NULL)) runtime_fail("Android VI 60 Hz vsync timer thread");
    pthread_detach(t);
}
static void vsync_timer_start(void) {
    static pthread_once_t once=PTHREAD_ONCE_INIT;
    pthread_once(&once,vsync_timer_create);
}

void android_front_set_window(ANativeWindow *window) {
    pthread_mutex_lock(&window_lock);
    if (window) ANativeWindow_acquire(window);
    ANativeWindow *old=activity_window;
    activity_window=window;
    pthread_mutex_unlock(&window_lock);
    if (old) ANativeWindow_release(old);
}

static ANativeWindow *current_window(void) {
    pthread_mutex_lock(&window_lock);
    ANativeWindow *window=activity_window;
    pthread_mutex_unlock(&window_lock);
    return window;
}

ANativeWindow *android_front_acquire_window(void) {
    pthread_mutex_lock(&window_lock);
    ANativeWindow *window=activity_window;
    if(window)ANativeWindow_acquire(window);
    pthread_mutex_unlock(&window_lock);
    return window;
}

static void vi_initialize(void) {
    if (current_window()) {
        fprintf(stderr, "[android-front] VI initialized with Android native window\n");
    } else {
        if (!getenv("TOTK_PROBE_HEADLESS_VI")) runtime_fail("Android VI requires an activity window");
        fprintf(stderr, "[android-front] diagnostic VI initialized without a display\n");
    }
}
static uint32_t vi_open(void **out) {
    if (!out) return 0xf601;
    *out=&display_token;
    return 0;
}
static uint32_t vi_layer(void **out, void *display) {
    if (!out || display!=&display_token) return 0xf601;
    *out=&layer_token;
    return 0;
}
static uint32_t vi_window(void **out, void *layer) {
    if (!out || layer!=&layer_token) return 0xf601;
    ANativeWindow *window=current_window();
    if (!window && !getenv("TOTK_PROBE_HEADLESS_VI")) return 0xf601;
    *out=window ? (void *)window : (void *)&window_token;
    fprintf(stderr, "[android-front] %s native window returned\n",window?"Android":"diagnostic");
    return 0;
}
static uint32_t vi_vsync(void *event, void *display) {
    if (!event || display!=&display_token) return 0xf601;
    hle_bind_system_event(event, 1);
    pthread_mutex_lock(&window_lock);
    unsigned i=0;while(i<vsync_count&&vsync_events[i]!=event)++i;
    if(i==vsync_count) {
        if(vsync_count==16) runtime_fail("Android VI vsync event limit");
        vsync_events[vsync_count++]=event;
    }
    int real_window=activity_window!=NULL;
    pthread_mutex_unlock(&window_lock);
    int follows=vsync_follows_display();
    if (real_window && !follows) vsync_timer_start();
    fprintf(stderr, "[android-front] VI vsync event bound (%s)\n",
        !real_window?"headless diagnostic, no frame timer":follows?"Android Choreographer, TOTK_VSYNC_DISPLAY=1":"fixed 60 Hz clock");
    return 0;
}

/* The SDK's versions read the layer's android::Surface, which our layer token is not. Frame numbers count the frames
   presented to the Android surface; the history is kept by the present layer (nvk_frame_history). */
extern uint64_t nvk_presented_frames(void);
extern int nvk_frame_history(uint64_t (*out)[5], int max);
static uint32_t vi_latest_frame(uint64_t *out, const void *layer) {
    if (!out || layer!=&layer_token) return 0xf601;
    *out=nvk_presented_frames();
    return 0;
}
typedef struct { uint64_t w[5]; } FrameInfo;   /* 0x28 bytes, layout from the game's reader (nvn_vk_present.c) */
_Static_assert(sizeof(FrameInfo)==0x28, "nn::vi::FrameInfo");
static int vi_frame_history(FrameInfo *info, int count, const void *layer) {
    if (layer!=&layer_token) { static int said; if (!said++) fprintf(stderr,"[android-front] ListFrameHistory for unknown layer %p\n",layer); return 0; }
    return nvk_frame_history(info ? (uint64_t (*)[5])info->w : NULL, info ? count : 0);
}

__attribute__((weak)) void android_front_screen_capture(int enabled) { (void)enabled; }
static void enable_screen_capture(void) { android_front_screen_capture(1); }
static void disable_screen_capture(void) { android_front_screen_capture(0); }
void *runtime_front_override(const char *name) {
    if (!strcmp(name,"_ZN2nn2oe16EnableScreenShotEv")) return enable_screen_capture;
    if (!strcmp(name,"_ZN2nn2oe17DisableScreenShotEv")) return disable_screen_capture;
    if (!current_window() && !getenv("TOTK_PROBE_HEADLESS_VI")) return NULL;
    if (!strcmp(name,"_ZN2nn2vi10InitializeEv")) return vi_initialize;
    if (!strcmp(name,"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE")) return vi_open;
    if (!strcmp(name,"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE")) return vi_layer;
    if (!strcmp(name,"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE")) return vi_window;
    if (!strcmp(name,"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE")) return vi_vsync;
    if (!strcmp(name,"_ZN2nn2vi20GetLatestFrameNumberEPmPKNS0_5LayerE")) return vi_latest_frame;
    if (!strcmp(name,"_ZN2nn2vi16ListFrameHistoryEPNS0_9FrameInfoEiPKNS0_5LayerE")) return vi_frame_history;
    return NULL;
}
