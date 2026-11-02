/* audout:u: open parameters, buffers released in append order after their samples played (real-time clock without
   a device), tags, Contains, buffer count, played sample count, flush, volume, session and domain shapes. */
#include <assert.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifndef TIMER_ABSTIME   /* macOS host: no clock_nanosleep */
#define TIMER_ABSTIME 1
static int test_clock_nanosleep(clockid_t c, int flags, const struct timespec *t, struct timespec *rem) {
    (void)flags; (void)rem; struct timespec now; clock_gettime(c, &now);
    long long ns = (long long)(t->tv_sec - now.tv_sec) * 1000000000ll + (t->tv_nsec - now.tv_nsec);
    if (ns <= 0) return 0; struct timespec d = {(time_t)(ns / 1000000000ll), (long)(ns % 1000000000ll)}; return nanosleep(&d, NULL) ? -1 : 0;
}
#define clock_nanosleep test_clock_nanosleep
#endif
#include "../android_audio_out.c"
static IpcSession sessions[16]; static unsigned ns;
static _Atomic int signals;
_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "%s\n", s); abort(); }
_Noreturn void ipc_unimplemented(const IpcSession *s, const IpcRequest *q) { (void)s; (void)q; abort(); }
void runtime_ipc_register(const char *n, IpcHandler h, IpcOpen o, void (*c)(void *)) { (void)n; (void)h; (void)o; (void)c; }
uint32_t runtime_ipc_new_session(const char *n, IpcHandler h, void *p, void (*c)(void *)) { sessions[++ns] = (IpcSession){.name = n, .handler = h, .state = p, .close = c}; return ns; }
uint32_t runtime_ipc_domain_new_object(IpcSession *s, const char *n, IpcHandler h, void *p) { uint32_t id = ++s->next_domain_id; s->domain_objects[s->ndomain_objects++] = (IpcDomainObject){id, n, h, p}; return id; }
void ipc_out(IpcResponse *r, const void *p, uint32_t n) { assert(r->data_size + n <= sizeof r->data); memcpy(r->data + r->data_size, p, n); r->data_size += n; }
const void *ipc_arg(const IpcRequest *q, uint32_t off, uint32_t n) { assert(off + n <= q->data_size); return q->data + off; }
void *ipc_guest(uint64_t addr, uint64_t size) { assert(addr || !size); return (void *)(uintptr_t)addr; }
uint32_t runtime_event_new(void) { return 40; }
uint32_t runtime_event_readable(uint32_t h) { return h + 64; }
void runtime_event_signal(uint32_t h) { assert(h == 40); signals++; }
void runtime_event_clear(uint32_t h) { assert(h == 40); }

static IpcResponse call(IpcSession *s, IpcHandler h, int domain, uint32_t object, unsigned cmd, const void *data, uint32_t n,
                        const void *in, uint64_t in_size, void *out, uint64_t out_size, int auto_buffers) {
    IpcRequest q = {.cmd = cmd, .domain = domain, .object_id = object, .data = data, .data_size = n};
    if (in) { if (auto_buffers) q.x[q.nx++] = (IpcBuffer){(uint64_t)(uintptr_t)in, in_size, 0}; else q.a[q.na++] = (IpcBuffer){(uint64_t)(uintptr_t)in, in_size, 0}; }
    if (out) { if (auto_buffers) q.c[q.nc++] = (IpcBuffer){(uint64_t)(uintptr_t)out, out_size, 0}; else q.b[q.nb++] = (IpcBuffer){(uint64_t)(uintptr_t)out, out_size, 0}; }
    IpcResponse r = {0}; h(s, &q, &r); assert(r.result == 0); return r;
}
static uint32_t u32(const IpcResponse *r, unsigned at) { uint32_t v; assert(r->data_size >= at + 4); memcpy(&v, r->data + at, 4); return v; }

int main(void) {
    setenv("TOTK_AUDIO_OFF", "1", 1);
    static int16_t pcm[3][480 * 2];
    for (int domain = 0; domain < 2; domain++) {
        int automatic = domain;   /* cover both buffer descriptor kinds */
        IpcSession root = {.name = "audout:u"};
        char names[0x100] = {0};
        IpcResponse r = call(&root, audio_out_manager, domain, 0, automatic ? 2 : 0, NULL, 0, NULL, 0, names, sizeof names, automatic);
        assert(u32(&r, 0) == 1 && !strcmp(names, "DeviceOut"));
        uint8_t param[16] = {0}; uint32_t rate = 48000; uint16_t ch = 2; memcpy(param, &rate, 4); memcpy(param + 4, &ch, 2);
        char out_name[0x100] = {0};
        r = call(&root, audio_out_manager, domain, 0, automatic ? 3 : 1, param, sizeof param, "DeviceOut", 10, out_name, sizeof out_name, automatic);
        assert(r.data_size == 16 && u32(&r, 0) == 48000 && u32(&r, 4) == 2 && u32(&r, 8) == 2 && u32(&r, 12) == 1 && !strcmp(out_name, "DeviceOut"));
        uint32_t id = domain ? r.domain_objects[0] : 0;
        IpcSession *s = domain ? &root : &sessions[r.move[0]];
        AudioOut *o = domain ? root.domain_objects[0].state : s->state;
        r = call(s, audio_out, domain, id, 4, NULL, 0, NULL, 0, NULL, 0, 0); assert(r.ncopy == 1 && r.copy[0] == 104);
        uint64_t tags[3] = {0x111, 0x222, 0x333}; uint64_t frames[3] = {480, 240, 120};
        for (int i = 0; i < 2; i++) {
            AudioOutBuffer b = {0, (uint64_t)(uintptr_t)pcm[i], sizeof pcm[i], frames[i] * 4, 0};
            call(s, audio_out, domain, id, automatic ? 7 : 3, &tags[i], 8, &b, sizeof b, NULL, 0, automatic);
        }
        r = call(s, audio_out, domain, id, 9, NULL, 0, NULL, 0, NULL, 0, 0); assert(u32(&r, 0) == 2);
        uint8_t found; r = call(s, audio_out, domain, id, 6, &tags[0], 8, NULL, 0, NULL, 0, 0); found = r.data[0]; assert(found == 1);
        r = call(s, audio_out, domain, id, 6, &tags[2], 8, NULL, 0, NULL, 0, 0); assert(r.data[0] == 0);
        usleep(30000);   /* stopped: nothing plays */
        r = call(s, audio_out, domain, id, 10, NULL, 0, NULL, 0, NULL, 0, 0); { uint64_t played; memcpy(&played, r.data, 8); assert(played == 0); }
        call(s, audio_out, domain, id, 1, NULL, 0, NULL, 0, NULL, 0, 0);
        r = call(s, audio_out, domain, id, 0, NULL, 0, NULL, 0, NULL, 0, 0); assert(u32(&r, 0) == 0);
        uint64_t got[8]; unsigned ngot = 0;
        for (int t = 0; t < 200 && ngot < 2; t++) {
            uint64_t rel[8] = {0};
            r = call(s, audio_out, domain, id, automatic ? 8 : 5, NULL, 0, NULL, 0, rel, sizeof rel, automatic);
            for (uint32_t k = 0; k < u32(&r, 0); k++) got[ngot++] = rel[k];
            usleep(5000);
        }
        assert(ngot == 2 && got[0] == 0x111 && got[1] == 0x222);   /* in append order, after 480 + 240 frames */
        r = call(s, audio_out, domain, id, 10, NULL, 0, NULL, 0, NULL, 0, 0); { uint64_t played; memcpy(&played, r.data, 8); assert(played == 720); }
        r = call(s, audio_out, domain, id, 9, NULL, 0, NULL, 0, NULL, 0, 0); assert(u32(&r, 0) == 0);
        call(s, audio_out, domain, id, 2, NULL, 0, NULL, 0, NULL, 0, 0);
        AudioOutBuffer b = {0, (uint64_t)(uintptr_t)pcm[2], sizeof pcm[2], frames[2] * 4, 0};
        call(s, audio_out, domain, id, 3, &tags[2], 8, &b, sizeof b, NULL, 0, 0);
        r = call(s, audio_out, domain, id, 11, NULL, 0, NULL, 0, NULL, 0, 0); assert(r.data[0] == 1);
        uint64_t rel[8] = {0}; r = call(s, audio_out, domain, id, 5, NULL, 0, NULL, 0, rel, sizeof rel, 0); assert(u32(&r, 0) == 1 && rel[0] == 0x333);
        float v = 0.25f; call(s, audio_out, domain, id, 12, &v, 4, NULL, 0, NULL, 0, 0);
        r = call(s, audio_out, domain, id, 13, NULL, 0, NULL, 0, NULL, 0, 0); { float g; memcpy(&g, r.data, 4); assert(g == 0.25f); }
        assert(signals > 0);
        audio_out_close(o);
    }
    puts("PASS: audout opens DeviceOut 48 kHz PCM16 stereo; buffers play only when started and release in append order with exact played-sample count; Contains/count/flush/volume; A/B and X/C buffers; session and domain shapes.");
}
