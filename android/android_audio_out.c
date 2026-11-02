/* audout:u — Horizon audio output service on AAudio.
 * Protocol reference: switchbrew.org/wiki/Audio_services (IAudioOutManager, IAudioOut) and libnx audout.c.
 * The SDK's nn::audio AudioOut client runs unchanged. Appended PCM16 buffers are played in order by one
 * host thread per output (blocking AAudio writes pace it in real time); a buffer is released, and the
 * buffer event signalled, once all of its samples were written to the device. Without an AAudio device
 * the same thread consumes buffers on a real-time clock, so the game's pacing does not change.
 */
#include "runtime.h"
#include "ipc.h"
#include <aaudio/AAudio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OUT_RATE 48000u
#define OUT_CHUNK 240u              /* frames per device write: 5 ms */
#define OUT_MAX_BUFFERS 64u
#define FMT_PCM16 2u
#define STATE_STARTED 0u
#define STATE_STOPPED 1u
static const char device_name[] = "DeviceOut";

typedef struct { uint64_t next, sample_buffer, buffer_capacity, data_size, data_offset; } AudioOutBuffer;
_Static_assert(sizeof(AudioOutBuffer) == 0x28, "AudioOutBuffer wire layout");
typedef struct { uint64_t tag; const int16_t *samples; uint64_t frames, done; } Queued;

typedef struct {
    pthread_mutex_t lock; pthread_t thread; int thread_ok, quit;
    uint32_t channels, state, event;
    float volume;
    Queued queue[OUT_MAX_BUFFERS]; unsigned head, count;          /* ring of appended, not yet released buffers */
    uint64_t released[OUT_MAX_BUFFERS]; unsigned nreleased;       /* tags waiting for GetReleasedAudioOutBuffers */
    uint64_t played;                                               /* sample frames played since open */
    AAudioStream *stream;
} AudioOut;

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void sleep_until(uint64_t ns) {
    struct timespec t = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL)) {}
}
static int16_t clamp16(float v) { return v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int16_t)v; }

/* Fill one stereo chunk from the queue; returns 1 when a buffer was released. 5.1 is downmixed
   (FL FR FC LFE BL BR, centre and surround at -3 dB, LFE dropped). */
static int fill_chunk(AudioOut *o, int16_t *lr) {
    memset(lr, 0, OUT_CHUNK * 2 * sizeof *lr);
    if (o->state != STATE_STARTED) return 0;
    int released = 0; unsigned at = 0;
    while (at < OUT_CHUNK && o->count) {
        Queued *q = &o->queue[o->head];
        uint64_t n = q->frames - q->done; if (n > OUT_CHUNK - at) n = OUT_CHUNK - at;
        const int16_t *s = q->samples + q->done * o->channels;
        for (uint64_t i = 0; i < n; i++, s += o->channels, at++) {
            float l, r;
            if (o->channels == 2) { l = s[0]; r = s[1]; }
            else { float c = 0.7071f * s[2]; l = s[0] + c + 0.7071f * s[4]; r = s[1] + c + 0.7071f * s[5]; l *= 0.5858f; r *= 0.5858f; }
            lr[2 * at] = clamp16(l * o->volume); lr[2 * at + 1] = clamp16(r * o->volume);
        }
        q->done += n; o->played += n;
        if (q->done == q->frames) {
            if (o->nreleased == OUT_MAX_BUFFERS) runtime_fail("audout released-buffer list overflow");
            o->released[o->nreleased++] = q->tag;
            o->head = (o->head + 1) % OUT_MAX_BUFFERS; o->count--; released = 1;
        }
    }
    return released;
}

static void *playback(void *arg) {
    AudioOut *o = arg; int16_t lr[OUT_CHUNK * 2]; uint64_t next = now_ns();
    int level_log = getenv("ZA_AUDIO_LEVEL_LOG") != NULL;
    unsigned chunks = 0, peak = 0; uint64_t nonzero = 0;
    for (;;) {
        pthread_mutex_lock(&o->lock);
        if (o->quit) { pthread_mutex_unlock(&o->lock); return NULL; }
        int released = fill_chunk(o, lr);
        pthread_mutex_unlock(&o->lock);
        if (level_log) {
            for (unsigned i=0;i<OUT_CHUNK*2;i++) { int v=lr[i]; unsigned a=v<0?-v:v; if(a>peak)peak=a; if(a)nonzero++; }
            if (++chunks == 200) {
                pthread_mutex_lock(&o->lock);
                fprintf(stderr,"[android-audout] PCM/second peak=%u nonzero=%llu played=%llu queued=%u state=%u volume=%g\n",
                    peak,(unsigned long long)nonzero,(unsigned long long)o->played,o->count,o->state,o->volume);
                pthread_mutex_unlock(&o->lock);
                chunks=peak=0;nonzero=0;
            }
        }
        if (released) runtime_event_signal(o->event);
        if (o->stream) {
            aaudio_result_t w = AAudioStream_write(o->stream, lr, (int32_t)OUT_CHUNK, 1000000000ll);
            if (w < 0) { fprintf(stderr, "[android-audout] AAudio write failed (%s): continuing without sound\n", AAudio_convertResultToText(w));
                AAudioStream_close(o->stream); o->stream = NULL; next = now_ns(); }
        } else { next += OUT_CHUNK * 1000000000ull / OUT_RATE; sleep_until(next); }
    }
}

static void open_device(AudioOut *o) {
    if (getenv("TOTK_AUDIO_OFF")) { fprintf(stderr, "[android-audout] TOTK_AUDIO_OFF: consuming buffers without a device\n"); return; }
    AAudioStreamBuilder *b = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) { fprintf(stderr, "[android-audout] AAudio unavailable: consuming buffers without sound\n"); return; }
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)OUT_RATE);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &o->stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || AAudioStream_getSampleRate(o->stream) != (int32_t)OUT_RATE || AAudioStream_requestStart(o->stream) != AAUDIO_OK) {
        fprintf(stderr, "[android-audout] AAudio output setup failed (%s): consuming buffers without sound\n", AAudio_convertResultToText(r));
        if (r == AAUDIO_OK) AAudioStream_close(o->stream);
        o->stream = NULL; return;
    }
    fprintf(stderr, "[android-audout] AAudio output %u Hz stereo, burst %d frames\n", OUT_RATE, AAudioStream_getFramesPerBurst(o->stream));
}

static void audio_out_close(void *state) {
    AudioOut *o = state;
    pthread_mutex_lock(&o->lock); o->quit = 1; pthread_mutex_unlock(&o->lock);
    if (o->thread_ok) pthread_join(o->thread, NULL);
    if (o->stream) { AAudioStream_requestStop(o->stream); AAudioStream_close(o->stream); }
    pthread_mutex_destroy(&o->lock); free(o);
}

/* Input/output buffers arrive as A/B descriptors or, for the Auto commands, as either A/B or X/C. */
static const IpcBuffer *in_buffer(const IpcRequest *q) { return q->na && q->a[0].size ? &q->a[0] : q->nx && q->x[0].size ? &q->x[0] : NULL; }
static uint64_t put_out(const IpcRequest *q, IpcResponse *r, const void *data, uint64_t n) {
    if (q->nb && q->b[0].size) { if (n > q->b[0].size) n = q->b[0].size; memcpy(ipc_guest(q->b[0].addr, n), data, n); return n; }
    if (q->nc && q->c[0].size) { if (n > q->c[0].size) n = q->c[0].size; memcpy(ipc_guest(q->c[0].addr, n), data, n);
        r->xout[r->nxout++] = (IpcBuffer){q->c[0].addr, n, 0}; return n; }
    return 0;
}

static AudioOut *object_state(IpcSession *s, const IpcRequest *q) {
    if (!q->domain) return s->state;
    for (unsigned i = 0; i < s->ndomain_objects; i++) if (s->domain_objects[i].id == q->object_id) return s->domain_objects[i].state;
    runtime_fail("audout object state missing");
}

static void audio_out(IpcSession *s, const IpcRequest *q, IpcResponse *r) {
    AudioOut *o = object_state(s, q);
    static uint64_t seen; if (q->cmd < 64 && !(seen & (1ull << q->cmd))) { seen |= 1ull << q->cmd; fprintf(stderr, "[android-audout] IAudioOut command %u\n", q->cmd); }
    switch (q->cmd) {
    case 0: { uint32_t v; pthread_mutex_lock(&o->lock); v = o->state; pthread_mutex_unlock(&o->lock); IPC_OUT(r, v); return; }
    case 1: pthread_mutex_lock(&o->lock); o->state = STATE_STARTED; pthread_mutex_unlock(&o->lock); return;
    case 2: pthread_mutex_lock(&o->lock); o->state = STATE_STOPPED; pthread_mutex_unlock(&o->lock); return;
    case 3: case 7: {   /* AppendAudioOutBuffer(Auto): u64 tag, in AudioOutBuffer */
        uint64_t tag = IPC_ARG(q, uint64_t, 0); const IpcBuffer *b = in_buffer(q);
        if (!b || b->size < sizeof(AudioOutBuffer)) runtime_fail("audout append without AudioOutBuffer");
        AudioOutBuffer ab; memcpy(&ab, ipc_guest(b->addr, sizeof ab), sizeof ab);
        uint64_t frame_bytes = 2ull * o->channels;
        if (ab.data_size % frame_bytes || ab.data_offset > ab.buffer_capacity || ab.data_size > ab.buffer_capacity - ab.data_offset) runtime_fail("audout buffer outside its capacity");
        const int16_t *samples = ab.data_size ? ipc_guest(ab.sample_buffer + ab.data_offset, ab.data_size) : NULL;
        pthread_mutex_lock(&o->lock);
        if (o->count == OUT_MAX_BUFFERS) { pthread_mutex_unlock(&o->lock); runtime_fail("audout buffer queue full"); }
        o->queue[(o->head + o->count) % OUT_MAX_BUFFERS] = (Queued){tag, samples, ab.data_size / frame_bytes, 0};
        o->count++;   /* an empty buffer is released by the next chunk */
        pthread_mutex_unlock(&o->lock);
        return;
    }
    case 4: r->copy[r->ncopy++] = runtime_event_readable(o->event); return;
    case 5: case 8: {   /* GetReleasedAudioOutBuffers(Auto): out u64 tags, u32 count */
        pthread_mutex_lock(&o->lock);
        uint64_t cap = q->nb && q->b[0].size ? q->b[0].size : q->nc ? q->c[0].size : 0;
        uint32_t n = o->nreleased; if (n > cap / 8) n = (uint32_t)(cap / 8);
        uint64_t tags[OUT_MAX_BUFFERS]; memcpy(tags, o->released, n * 8u);
        memmove(o->released, o->released + n, (o->nreleased - n) * 8u); o->nreleased -= n;
        if (!o->nreleased) runtime_event_clear(o->event);
        pthread_mutex_unlock(&o->lock);
        if (n) put_out(q, r, tags, n * 8u);
        IPC_OUT(r, n); return;
    }
    case 6: {   /* ContainsAudioOutBuffer(u64 tag) */
        uint64_t tag = IPC_ARG(q, uint64_t, 0); uint8_t found = 0;
        pthread_mutex_lock(&o->lock);
        for (unsigned i = 0; i < o->count && !found; i++) found = o->queue[(o->head + i) % OUT_MAX_BUFFERS].tag == tag;
        for (unsigned i = 0; i < o->nreleased && !found; i++) found = o->released[i] == tag;
        pthread_mutex_unlock(&o->lock);
        IPC_OUT(r, found); return;
    }
    case 9: { pthread_mutex_lock(&o->lock); uint32_t n = o->count; pthread_mutex_unlock(&o->lock); IPC_OUT(r, n); return; }
    case 10: { pthread_mutex_lock(&o->lock); uint64_t n = o->played; pthread_mutex_unlock(&o->lock); IPC_OUT(r, n); return; }
    case 11: {   /* FlushAudioOutBuffers: queued buffers are released unplayed */
        pthread_mutex_lock(&o->lock);
        uint8_t flushed = o->count != 0;
        while (o->count) {
            if (o->nreleased == OUT_MAX_BUFFERS) runtime_fail("audout released-buffer list overflow");
            o->released[o->nreleased++] = o->queue[o->head].tag; o->head = (o->head + 1) % OUT_MAX_BUFFERS; o->count--;
        }
        pthread_mutex_unlock(&o->lock);
        if (flushed) runtime_event_signal(o->event);
        IPC_OUT(r, flushed); return;
    }
    case 12: { float v = IPC_ARG(q, float, 0); if (!(v >= 0.f && v <= 128.f)) runtime_fail("audout volume out of range");
        pthread_mutex_lock(&o->lock); o->volume = v; pthread_mutex_unlock(&o->lock); return; }
    case 13: { pthread_mutex_lock(&o->lock); float v = o->volume; pthread_mutex_unlock(&o->lock); IPC_OUT(r, v); return; }
    default: ipc_unimplemented(s, q);
    }
}

static void audio_out_manager(IpcSession *s, const IpcRequest *q, IpcResponse *r) {
    fprintf(stderr, "[android-audout] IAudioOutManager command %u\n", q->cmd);
    switch (q->cmd) {
    case 0: case 2: {   /* ListAudioOuts(Auto): out names (0x100 bytes each), u32 count */
        char name[0x100] = {0}; memcpy(name, device_name, sizeof device_name);
        uint32_t n = put_out(q, r, name, sizeof name) == sizeof name;
        IPC_OUT(r, n); return;
    }
    case 1: case 3: {   /* OpenAudioOut(Auto): AudioOutParameter, aruid, pid, process handle; in name, out name */
        uint32_t rate = IPC_ARG(q, uint32_t, 0); uint16_t channels = IPC_ARG(q, uint16_t, 4);
        if (rate && rate != OUT_RATE) { fprintf(stderr, "[android-audout] requested %u Hz\n", rate); runtime_fail("audout sample rate other than 48 kHz"); }
        const IpcBuffer *in = in_buffer(q);
        if (in && in->size) {
            char want[0x100] = {0}; uint64_t n = in->size < sizeof want - 1 ? in->size : sizeof want - 1;
            memcpy(want, ipc_guest(in->addr, n), n);
            if (want[0] && strcmp(want, device_name)) { fprintf(stderr, "[android-audout] requested device \"%s\"\n", want); runtime_fail("audout device other than DeviceOut"); }
        }
        char name[0x100] = {0}; memcpy(name, device_name, sizeof device_name); put_out(q, r, name, sizeof name);
        AudioOut *o = calloc(1, sizeof *o);
        if (!o) runtime_fail("audout allocation");
        pthread_mutex_init(&o->lock, NULL);
        o->channels = channels <= 2 ? 2 : 6;   /* as Horizon: mono and default requests get stereo */
        o->state = STATE_STOPPED; o->volume = 1.f; o->event = runtime_event_new();
        open_device(o);
        if (pthread_create(&o->thread, NULL, playback, o)) runtime_fail("audout playback thread");
        o->thread_ok = 1;
        uint32_t out[4] = {OUT_RATE, o->channels, FMT_PCM16, o->state};
        ipc_out(r, out, sizeof out);
        fprintf(stderr, "[android-audout] opened DeviceOut: %u Hz, %u channels requested -> %u, PCM16\n", OUT_RATE, channels, o->channels);
        if (q->domain) r->domain_objects[r->ndomain_objects++] = runtime_ipc_domain_new_object(s, "IAudioOut", audio_out, o);
        else r->move[r->nmove++] = runtime_ipc_new_session("IAudioOut", audio_out, o, audio_out_close);
        return;
    }
    default: ipc_unimplemented(s, q);
    }
}

void android_audio_out_register(void) { runtime_ipc_register("audout:u", audio_out_manager, NULL, NULL); }
