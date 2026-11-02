#include "../android_wwise_pck.h"
#include <assert.h>
#include <stdio.h>
static void put(uint8_t *p,uint32_t v){memcpy(p,&v,4);}
int main(void) {
 uint8_t header[28]={0}, table[64]={0};uint64_t at,off,size;uint32_t bytes;
 memcpy(header,"AKPK",4);put(header+4,84);put(header+8,1);put(header+12,16);put(header+16,4);put(header+20,40);put(header+24,0);
 assert(za_pck_layout(header,1000,&at,&bytes)&&at==48&&bytes==40);
 put(header+4,20);assert(!za_pck_layout(header,1000,&at,&bytes));put(header+4,84);
 assert(!za_pck_layout(header,80,&at,&bytes));header[0]='X';assert(!za_pck_layout(header,1000,&at,&bytes));header[0]='A';
 put(table,3);
 /* Same id in a localized entry and a neutral entry; use only neutral. */
 uint32_t entries[]={7,1,10,100,1, 7,1,20,200,0, 99,4096,100,0x100001,0};memcpy(table+4,entries,sizeof entries);
 assert(za_pck_find(table,sizeof table,0x200000000ULL,7,&off,&size)&&off==200&&size==20);
 assert(za_pck_find(table,sizeof table,0x200000000ULL,99,&off,&size)&&off==0x100001000ULL&&size==100);
 assert(!za_pck_find(table,sizeof table,219,7,&off,&size));
 assert(!za_pck_find(table,sizeof table,1000,8,&off,&size));
 assert(!za_pck_find(table,sizeof table-1,1000,7,&off,&size));
 put(table,0xffffffffu);assert(!za_pck_find(table,sizeof table,1000,7,&off,&size));
 puts("Wwise package bounds, language choice, missing ids, truncation and 64-bit slices: pass");
}
