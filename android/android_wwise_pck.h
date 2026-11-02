/* Bounds-checked Wwise AKPK v1 stream table. The caller reads the header/table
 * from the already verified RomFS image; ranges stay relative to the package. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
static uint32_t za_pck_u32(const void *p) { uint32_t v; memcpy(&v,p,4); return v; }
static int za_pck_layout(const uint8_t h[28], uint64_t package_size, uint64_t *table_at, uint32_t *table_size) {
    if (memcmp(h,"AKPK",4) || za_pck_u32(h+8)!=1) return 0;
    uint64_t end=8ULL+za_pck_u32(h+4), at=28ULL+za_pck_u32(h+12)+za_pck_u32(h+16);
    uint32_t bytes=za_pck_u32(h+20);
    if(end>package_size || at>end || bytes<4 || bytes>end-at || za_pck_u32(h+24)>end-at-bytes) return 0;
    *table_at=at; *table_size=bytes; return 1;
}
static int za_pck_find(const uint8_t *table,size_t bytes,uint64_t package_size,uint32_t id,uint64_t *offset,uint64_t *size) {
    if(bytes<4) return 0;
    uint32_t n=za_pck_u32(table);
    if(n>(bytes-4)/20 || bytes!=4ULL+20ULL*n) return 0;
    /* These tables are sorted by file id; duplicate languages remain adjacent. */
    uint32_t lo=0,hi=n;
    while(lo<hi) {uint32_t m=lo+(hi-lo)/2; if(za_pck_u32(table+4+20ULL*m)<id)lo=m+1;else hi=m;}
    for(;lo<n;lo++) {
        const uint8_t *e=table+4+20ULL*lo;
        if(za_pck_u32(e)!=id)break;
        if(za_pck_u32(e+16)!=0)continue; /* this path requests the language-neutral media */
        uint64_t block=za_pck_u32(e+4), start=block*za_pck_u32(e+12), len=za_pck_u32(e+8);
        if(!block || start>package_size || len>package_size-start)return 0;
        *offset=start;*size=len;return 1;
    }
    return 0;
}
