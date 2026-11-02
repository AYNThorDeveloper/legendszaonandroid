#include "../nvn_vk_xfb_storage.h"
#include <assert.h>
#include <stdio.h>
int main(int argc, char **argv) {
    assert(argc == 3);
    FILE *f = fopen(argv[1], "rb"); assert(f);
    fseek(f, 0, SEEK_END); long bytes = ftell(f); rewind(f); assert(bytes > 20 && bytes % 4 == 0);
    uint32_t *w = malloc((size_t)bytes); assert(w); assert(fread(w, 1, (size_t)bytes, f) == (size_t)bytes); fclose(f);
    size_t n = 0; NvkXfbStorage layout;
    uint32_t *out = nvk_spv_xfb_storage(w, (size_t)bytes / 4, 3, &n, &layout); assert(out);
    assert(layout.mask == 7 && layout.stride[0] == 12 && layout.stride[1] == 4 && layout.stride[2] == 4);
    assert(layout.binding[0] == 3 && layout.binding[1] == 4 && layout.binding[2] == 5);
    for(size_t at=5;at<n;) {
        unsigned len=out[at]>>16,op=out[at]&65535; assert(len && at+len<=n);
        assert(!(op==17 && len==2 && out[at+1]==53));
        assert(!(op==16 && len>=3 && out[at+2]==11));
        at+=len;
    }
    f=fopen(argv[2],"wb");assert(f);assert(fwrite(out,4,n,f)==n);fclose(f);
    free(out);
    uint32_t magic=w[0];w[0]=0;assert(!nvk_spv_xfb_storage(w,(size_t)bytes/4,3,&n,&layout));w[0]=magic;
    assert(!nvk_spv_xfb_storage(w,4,3,&n,&layout));
    free(w);puts("XFB storage lowering: PASS (bindings, strides, removed native XFB, invalid modules rejected)");return 0;
}
