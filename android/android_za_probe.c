/* Z-A 2.0.2 audio bootstrap and optional diagnostics.
 * The port reaches SoundEngine::Init but misses the engine's base-bank loading.
 * At a guarded, empty startup callback after the paths are ready, load Init first,
 * then the game's 15 remaining banks through its own synchronous Wwise API.
 * On-device: all 16 return success, PCM is nonzero, user confirms audible sound.
 * ZA_AUDIO_BOOTSTRAP=0 disables this workaround. ZA_AUDIO_PROBE=1 enables counters;
 * ZA_AUDIO_LOAD_INIT=1 retains the single-bank diagnostic (unless ALL_BANKS=1).
 * Code is in the loader's reserved RX tail; counters are in main's RW key page.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern _Noreturn void runtime_fail(const char *message);

#define PROBE_DATA 0x2100u
typedef struct { uint32_t site, insn; int keep_w0; } Probe;
static const Probe probes[] = {
    {0xa975b4, 0xa9bb7bfd, 0}, {0x949160, 0xa9be7bfd, 0}, {0x9491f4, 0xd10103ff, 0},
    {0xadd480, 0x2a0003f3, 1}, {0xadd490, 0x7100041f, 1}, {0xadd4b8, 0x2a0003f3, 1},
    {0xadd5ec, 0xd10007a0, 1}, {0xadd9b0, 0x7100041f, 1}, {0xadd9c8, 0x7100041f, 1},
    {0xadd9d4, 0x7100041f, 1}, {0xadd9e4, 0x7100041f, 1},
    {0x1d87ac8, 0x2a0103e0, 0}, {0x1d87bd0, 0xaa0703e8, 0},
    {0x1d87c24, 0xaa0103e0, 2}, {0x1d87c34, 0xaa0103e0, 2},
    {0xd226fc, 0x7100041f, 1}, {0xd22750, 0x2a0003e2, 1}, {0xd22768, 0xa9584ff4, 1},
};
#define NPROBE (sizeof probes / sizeof *probes)
static volatile uint32_t *init_bank_result;
static const char *bank_names[] = {"Init.bnk", "COMMON.bnk", "UI.bnk", "BGM.bnk", "DEMO.bnk", "ENV.bnk", "ME.bnk", "VIB.bnk",
    "PM.bnk", "PL.bnk", "BATTLE_SYSTEM.bnk", "PROPS.bnk", "NPC.bnk", "PM_SKILLS.bnk", "PM_VOICE.bnk", "DIALOG.bnk"};
static unsigned bank_count;
static volatile uint64_t *counts;   /* [i * 2] count, [i * 2 + 1] last w0 */

static uint32_t word_at(const uint8_t *b, uint64_t o) { uint32_t w; memcpy(&w, b + o, 4); return w; }
static uint32_t branch(uint64_t pc, uint64_t target) {
    int64_t d = (int64_t)target - (int64_t)pc;
    if ((d & 3) || d < -(1 << 27) || d >= (1 << 27)) runtime_fail("Z-A probe branch outside range");
    return 0x14000000 | (((uint32_t)(d >> 2)) & 0x3ffffff);
}
static uint32_t adrp(unsigned reg, uint64_t pc, uint64_t target) {
    int64_t pages = (int64_t)(target >> 12) - (int64_t)(pc >> 12);
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    return 0x90000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | reg;
}

/* stub..ro_off is the reserved executable diagnostic area (still RW while patching). */
void android_za_probe_patch(uint8_t *base, uint64_t stub, uint64_t ro_off, uint64_t key_off) {
    const char *e = getenv("ZA_AUDIO_PROBE");
    int probe_on = e && !strcmp(e,"1");
    const char *boot = getenv("ZA_AUDIO_BOOTSTRAP");
    int bootstrap = !boot || strcmp(boot,"0");
    if (!probe_on && !bootstrap) return;
    uint64_t data = key_off + PROBE_DATA;
    for (unsigned i = 0; probe_on && i < NPROBE; i++) if (word_at(base, probes[i].site) != probes[i].insn) {
        fprintf(stderr, "[za-probe] site %#x holds %#x, expected %#x: probes off\n", probes[i].site, word_at(base, probes[i].site), probes[i].insn);
        return;
    }
    uint64_t at = (stub + 15) & ~15ULL;
    /* Validate the whole veneer range before changing any game instruction. */
    uint64_t needed = at;
    for (unsigned i = 0; probe_on && i < NPROBE; i++) needed += 4 * (7 + !!probes[i].keep_w0);
    if (needed > ro_off) { fprintf(stderr, "[za-probe] no room for audio probes\n"); return; }
    for (uint64_t k = at; k < needed; k++) if (base[k]) runtime_fail("Z-A probe page not empty");
    for (unsigned i = 0; probe_on && i < NPROBE; i++) {
        uint32_t lo = (uint32_t)((data + 16 * i) & 0xfff);
        uint32_t v[9]; unsigned n = 0;
        v[n++] = adrp(16, (uintptr_t)base + at, (uintptr_t)base + data + 16 * i);
        v[n++] = 0x91000210 | (lo << 10);            /* add x16, x16, #lo */
        v[n++] = 0xf9400211;                         /* ldr x17, [x16] */
        v[n++] = 0x91000631;                         /* add x17, x17, #1 */
        v[n++] = 0xf9000211;                         /* str x17, [x16] */
        if (probes[i].keep_w0) v[n++] = probes[i].keep_w0 == 2 ? 0xf9000601 : 0xb9000a00;  /* str w0, [x16, #8] */
        v[n++] = probes[i].insn;
        v[n] = branch(at + 4 * n, probes[i].site + 4); n++;
        if (at + 4 * n > ro_off) { fprintf(stderr, "[za-probe] no room for probe %u\n", i); return; }
        for (unsigned k = 0; k < 4 * n; k++) if (base[at + k]) runtime_fail("Z-A probe padding not empty");
        memcpy(base + at, v, 4 * n);
        uint32_t jump = branch(probes[i].site, at);
        memcpy(base + probes[i].site, &jump, 4);
        at += 4 * n;
    }
    /* Diagnostic only: replace a verified empty startup callback after the paths
       have been set. It is a call boundary: volatile registers are dead. The
       existing startup epilogue restores LR; the added stack slot holds bank id. */
    const char *force = getenv("ZA_AUDIO_LOAD_INIT");
    if (bootstrap || (force && !strcmp(force, "1"))) {
        uint64_t hook = at, pc = at, result = key_off + 0x2300;
        uint32_t v[256]; unsigned n = 0;
        const char *all = getenv("ZA_AUDIO_LOAD_ALL_BANKS");
        bank_count = (!force || strcmp(force,"1") || (all && !strcmp(all,"1"))) ? sizeof bank_names / sizeof *bank_names : 1;
        init_bank_result = (volatile uint32_t *)(base + result);
        uint64_t names = (stub + 0x2000 + 15) & ~15ULL;
        if (names + 512 > ro_off) runtime_fail("Z-A bank diagnostic names outside reserved page");
        if (word_at(base, 0x948b14) != 0xd65f03c0 || word_at(base, 0x9488e8) != (branch(0x9488e8, 0x948b14) | 0x80000000u))
            runtime_fail("Z-A late Init-bank diagnostic guard mismatch");
        v[n++] = 0xd10083ff; /* sub sp,sp,#32 */
        for (unsigned i=0;i<bank_count;i++) {
            uint64_t name = names, out = result + 4*i;
            size_t len = strlen(bank_names[i])+1;
            for (size_t k=0;k<len;k++) if(base[name+k]) runtime_fail("Z-A bank name area occupied");
            memcpy(base+name,bank_names[i],len); names+=len;
            init_bank_result[i] = 0xffffffffu;
            v[n] = adrp(0, (uintptr_t)base + pc + 4*n, (uintptr_t)base + name); n++;
            v[n++] = 0x91000000 | ((name & 0xfff) << 10);
            v[n++] = 0x910003e1; /* mov x1,sp (bank id output) */
            v[n++] = 0x2a1f03e2; /* mov w2,wzr (bank type) */
            v[n] = branch(pc + 4*n, 0xd225b0) | 0x80000000u; n++;
            v[n] = adrp(16, (uintptr_t)base + pc + 4*n, (uintptr_t)base + out); n++;
            v[n++] = 0xb9000200 | ((out & 0xfff) / 4 << 10);
        }
        v[n++] = 0x910083ff; /* add sp,sp,#32 */
        v[n] = branch(pc + 4*n, 0x9488ec); n++;
        if (hook + sizeof v > stub + 0x2000) runtime_fail("Z-A bank diagnostic code area full");
        for (unsigned k=0;k<4*n;k++) if(base[hook+k]) runtime_fail("Z-A bank diagnostic page occupied");
        memcpy(base+hook,v,4*n);
        uint32_t jump=branch(0x9488e8,hook); memcpy(base+0x9488e8,&jump,4);
        fprintf(stderr,"[za-probe] explicit %u-bank load diagnostic on\n",bank_count);
    }
    if (probe_on) {
        counts = (volatile uint64_t *)(void *)(base + data);
        fprintf(stderr, "[za-probe] %u audio probes on (counts at key+%#x)\n", (unsigned)NPROBE, PROBE_DATA);
    }
}

void android_za_probe_report(void) {
    static int bank_reported;
    if (init_bank_result && !bank_reported) {
        bank_reported=1;
        for (unsigned i=0;i<bank_count;i++) fprintf(stderr,"[za-probe] explicit %s result=%#x\n",bank_names[i],init_bank_result[i]);
    }
    if (!counts) return;
    static unsigned reports;
    if (reports++ && reports % 60) return;
    static uint64_t seen[NPROBE];
    static int first = 1;
    int changed = first; first = 0;
    for (unsigned i = 0; i < NPROBE; i++) if (counts[2 * i] != seen[i]) { seen[i] = counts[2 * i]; changed = 1; }
    if (changed) for (unsigned i = 0; i < NPROBE; i++)
        fprintf(stderr, "[za-probe] main+%#x count=%llu result=%#llx\n", probes[i].site,
            (unsigned long long)seen[i], (unsigned long long)counts[2 * i + 1]);
}
