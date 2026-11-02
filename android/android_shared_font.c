/* nn::pl shared fonts on Android: the verified system BFTTF files laid out back to back as in the pl shared memory,
   each payload decoded to the sfnt font it holds (games/totk/runtime/shared_font.c on macOS passes it still encoded).
   GetSharedFontAddress points 8 bytes past each file start (after the BFTTF header); the size is the font size.

   TOTK_SHARED_FONTS=<dir> holds the six files extract_shared_fonts.py writes and fonts.list, one line per font in
   type order: "<type> <file> <sha256>". Every file is checked against its digest before the game sees it. */
#include "runtime.h"
#include "sha256.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHARED_FONT_COUNT 6
static uint8_t *region;
static uint64_t offsets[SHARED_FONT_COUNT], font_bytes[SHARED_FONT_COUNT];
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void need(int c, const char *m) { if (!c) runtime_fail(m); }

static void load(void) {
    const char *dir = getenv("TOTK_SHARED_FONTS");
    need(dir && *dir, "TOTK_SHARED_FONTS is not set (shared fonts from extract_shared_fonts.py)");
    char path[4096], files[SHARED_FONT_COUNT][256], digests[SHARED_FONT_COUNT][65];
    need(snprintf(path, sizeof path, "%s/fonts.list", dir) < (int)sizeof path, "shared font path");
    FILE *list = fopen(path, "r"); need(list != NULL, "open shared fonts.list");
    for (int i = 0; i < SHARED_FONT_COUNT; i++) {
        int type;
        need(fscanf(list, "%d %255s %64s", &type, files[i], digests[i]) == 3 && type == i && strlen(digests[i]) == 64, "shared fonts.list row");
    }
    fclose(list);
    uint64_t sizes[SHARED_FONT_COUNT], total = 0;
    for (int i = 0; i < SHARED_FONT_COUNT; i++) {
        need(snprintf(path, sizeof path, "%s/%s", dir, files[i]) < (int)sizeof path, "shared font path");
        FILE *f = fopen(path, "rb"); need(f != NULL, "open shared font");
        need(fseek(f, 0, SEEK_END) == 0, "shared font size"); long n = ftell(f); fclose(f);
        need(n > 8 && n < (64 << 20), "shared font size");
        sizes[i] = (uint64_t)n; total += sizes[i];
    }
    region = malloc(total); need(region != NULL, "shared font memory");
    uint64_t at = 0;
    for (int i = 0; i < SHARED_FONT_COUNT; i++) {
        need(snprintf(path, sizeof path, "%s/%s", dir, files[i]) < (int)sizeof path, "shared font path");
        FILE *f = fopen(path, "rb"); need(f != NULL, "open shared font");
        need(fread(region + at, 1, sizes[i], f) == sizes[i] && fgetc(f) == EOF, "shared font read"); fclose(f);
        Sha256 s; uint8_t d[32]; char hex[65];
        sha_init(&s); sha_update(&s, region + at, sizes[i]); sha_final(&s, d); digest_hex(d, hex);
        need(!strcmp(hex, digests[i]), "shared font integrity mismatch");
        /* The firmware stores BFTTF: big-endian words XORed with key = word0 ^ 0x7f9a0218, word1 = font size. The pl
           service places the font itself in shared memory behind that header, so the payload is decoded here. */
        uint8_t *p = region + at;
        uint32_t key = ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]) ^ 0x7f9a0218u;
        uint32_t decoded = ((uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7]) ^ key;
        need(decoded == sizes[i] - 8, "shared font BFTTF size");
        for (uint64_t k = 8; k < sizes[i]; k++) p[k] ^= (uint8_t)(key >> (24 - 8 * (k & 3)));
        need(!memcmp(p + 8, "\0\1\0\0", 4) || !memcmp(p + 8, "OTTO", 4) || !memcmp(p + 8, "true", 4), "shared font is not an sfnt font");
        offsets[i] = at + 8; font_bytes[i] = sizes[i] - 8; at += sizes[i];
    }
    fprintf(stderr, "[totk] shared fonts loaded: %d fonts, %llu bytes\n", SHARED_FONT_COUNT, (unsigned long long)total);
}
static int valid(int type) { if (type < 0 || type >= SHARED_FONT_COUNT) runtime_fail("invalid shared font type"); pthread_once(&once, load); return type; }
static void request_load(int type) { valid(type); }   /* loaded with the first request: nothing is pending afterwards */
static int load_state(int type) { valid(type); return 1; }   /* SharedFontLoadState_Loaded */
static const void *address(int type) { return region + offsets[valid(type)]; }
static size_t size(int type) { return (size_t)font_bytes[valid(type)]; }
void *runtime_font_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE", request_load},
        {"_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", load_state},
        {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", address},
        {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", size}};
    for (unsigned i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}
