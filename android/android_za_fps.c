/* Pokémon Legends Z-A 60 FPS mode. The game's frame clock reads its "vsyncs per frame - 1" byte (1 on the Switch:
 * 30 FPS) with ldrsb w20, [x9, #0x20]; it sets the present interval from it and steps every subsystem by
 * max(that, the vsyncs that really passed). At 0 the game presents every vsync and steps its world 1/60 s, so it
 * keeps its speed (found by Hexveil404/PokeZ-ARecomp, tools/patches.py ZA_BUILDS; there 2.0.0 +0x1b29cf8).
 * A veneer in the zero padding after main's text reads our mode byte in main's key page: 1 = 60 FPS (w20 = 0),
 * 0 = the game's own value. The byte can change while the game runs (video menu FPS choice). */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern _Noreturn void runtime_fail(const char *message);

#define ZA_FRAME_SITE 0x1b296d8u    /* main 2.0.2 */
#define ZA_FRAME_INSN 0x39c08134u   /* ldrsb w20, [x9, #0x20] */
#define ZA_FRAME_PREV 0xf9402269u   /* ldr x9, [x19, #0x40] */
#define ZA_DATA_OFFSET 0x2000u      /* within main's 16 KiB key page, 4 KiB aligned (ADRP, lo12 = 0) */

static _Atomic uint8_t *za_mode;
static int za_wanted = -1;          /* chosen before the loader patched main */

static int za_env(void) {
    const char *e = getenv("ZA_FPS60");
    return e && *e ? atoi(e) != 0 : -1;
}
void android_za_fps_set(int on) {
    int env = za_env();
    if (env >= 0) on = env;
    za_wanted = on;
    if (!za_mode) return;
    uint8_t old = atomic_exchange(za_mode, (uint8_t)(on ? 1 : 0));
    if (old != (on ? 1 : 0)) fprintf(stderr, "[android-timing] Z-A frame mode: %s\n", on ? "60 FPS" : "game's own (30 FPS)");
}

/* Keep the selected frame mode until the user changes it. Performance dips must
 * not silently replace the user's 60 FPS selection with 30 FPS. */
void android_za_fps_present(uint64_t now_ns) {
    (void)now_ns;
    extern void android_za_probe_report(void);
    android_za_probe_report();
}

static uint32_t word_at(const uint8_t *base, uint64_t off) { uint32_t w; memcpy(&w, base + off, 4); return w; }
static uint32_t branch(uint64_t pc, uint64_t target) {
    int64_t delta = (int64_t)target - (int64_t)pc;
    if ((delta & 3) || delta < -(1 << 27) || delta >= (1 << 27)) runtime_fail("Z-A frame-mode branch outside range");
    return 0x14000000 | (((uint32_t)(delta >> 2)) & 0x3ffffff);
}
static uint32_t adrp(unsigned reg, uint64_t pc, uint64_t target) {
    int64_t pages = (int64_t)(target >> 12) - (int64_t)(pc >> 12);
    if (pages < -(1 << 20) || pages >= (1 << 20)) runtime_fail("Z-A frame-mode data outside ADRP range");
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    return 0x90000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | reg;
}

/* base: main's mapping (still writable); text_end / ro_off: end of code and start of read-only data; key_off: main's
 * key page. Returns 1 when applied (the key page must then stay writable). */
int android_za_fps_patch(uint8_t *base, uint64_t text_end, uint64_t ro_off, uint64_t key_off) {
    if (word_at(base, ZA_FRAME_SITE) != ZA_FRAME_INSN || word_at(base, ZA_FRAME_SITE - 4) != ZA_FRAME_PREV) {
        fprintf(stderr, "[android-loader] Z-A frame-mode site %#x holds %#x %#x: not main 2.0.2, 60 FPS mode unavailable\n",
            ZA_FRAME_SITE, word_at(base, ZA_FRAME_SITE - 4), word_at(base, ZA_FRAME_SITE));
        return 0;
    }
    uint64_t stub = (text_end + 15) & ~15ULL, data = key_off + ZA_DATA_OFFSET;
    if (stub + 32 > ro_off) runtime_fail("Z-A frame-mode veneer does not fit after the text");
    for (unsigned i = 0; i < 32; i++) if (base[stub + i]) runtime_fail("Z-A frame-mode veneer padding not empty");
    if (base[data]) runtime_fail("Z-A frame-mode byte in the key page not empty");
    /* Only w20 is used: it is the instruction's own destination. */
    uint32_t veneer[] = {
        adrp(20, (uintptr_t)base + stub, (uintptr_t)base + data),  /* adrp x20, mode byte */
        0x39400294,                                                 /* ldrb w20, [x20] */
        0x35000074,                                                 /* cbnz w20, +12 (60 FPS) */
        ZA_FRAME_INSN,                                              /* ldrsb w20, [x9, #0x20] (the game's value) */
        branch(stub + 16, ZA_FRAME_SITE + 4),
        0x52800014,                                                 /* mov w20, #0 */
        branch(stub + 24, ZA_FRAME_SITE + 4),
    };
    memcpy(base + stub, veneer, sizeof veneer);
    uint32_t jump = branch(ZA_FRAME_SITE, stub);
    memcpy(base + ZA_FRAME_SITE, &jump, 4);
    za_mode = (_Atomic uint8_t *)(void *)(base + data);
    fprintf(stderr, "[android-loader] Z-A frame mode switchable (site +%#x, veneer +%#llx, byte key+%#x)\n", ZA_FRAME_SITE,
        (unsigned long long)stub, ZA_DATA_OFFSET);
    android_za_fps_set(za_wanted < 0 ? 1 : za_wanted);
    return 1;
}
