#define STB_IMAGE_IMPLEMENTATION
#define STBI_MALLOC(sz)           malloc(sz)
#define STBI_REALLOC_SIZED(p,os,ns) realloc(p,ns)
#define STBI_FREE(p)              free(p)
#include <stdio.h>
#include <stdlib.h>
#include "include/stb_image.h"
int main(int argc, char** argv) {
    const char* fn = argc > 1 ? argv[1] : "kyuzen.png";
    FILE* f = fopen(fn, "rb");
    if (!f) { fprintf(stderr, "open failed: %s\n", fn); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    unsigned char* raw = malloc((size_t)sz);
    fread(raw, 1, (size_t)sz, f); fclose(f);
    fprintf(stderr, "file=%s size=%ld\n", fn, sz);
    int iw, ih, ch;
    unsigned char* px = stbi_load_from_memory(raw, (int)sz, &iw, &ih, &ch, 4);
    fprintf(stderr, "decoded: %dx%d ch=%d px=%p\n", iw, ih, ch, (void*)px);
    if (px) { free(px); fprintf(stderr, "free ok\n"); }
    free(raw);
    return 0;
}
