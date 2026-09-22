#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- canary-fence allocator: detect any over/underflow of stb allocations ---- */
#define FENCE 32
#define MAGIC 0xA5
#define MAXTRACK 4096
typedef struct { void* user; size_t size; } track_t;
static track_t g_track[MAXTRACK];
static int g_ntrack = 0;
static long g_seq = 0;

static void check_all(const char* where) {
    for (int i = 0; i < g_ntrack; i++) {
        if (!g_track[i].user) continue;
        uint8_t* base = (uint8_t*)g_track[i].user - FENCE;
        uint8_t* tail = (uint8_t*)g_track[i].user + g_track[i].size;
        for (int b = 0; b < FENCE; b++) {
            if (base[b] != MAGIC) {
                fprintf(stderr, "\n*** UNDERFLOW at %s: alloc[%d] user=%p size=%zu head_off=%d\n",
                        where, i, g_track[i].user, g_track[i].size, b);
                abort();
            }
            if (tail[b] != MAGIC) {
                fprintf(stderr, "\n*** OVERFLOW at %s: alloc[%d] user=%p size=%zu tail_off=%d (wrote %d bytes past end)\n",
                        where, i, g_track[i].user, g_track[i].size, b, b+1);
                abort();
            }
        }
    }
}
static void* my_malloc(size_t sz) {
    check_all("malloc-entry");
    uint8_t* p = (uint8_t*)malloc(sz + 2*FENCE);
    if (!p) return NULL;
    memset(p, MAGIC, FENCE);
    memset(p + FENCE + sz, MAGIC, FENCE);
    void* user = p + FENCE;
    if (g_ntrack < MAXTRACK) { g_track[g_ntrack].user = user; g_track[g_ntrack].size = sz; g_ntrack++; }
    return user;
}
static int find_track(void* user) {
    for (int i = 0; i < g_ntrack; i++) if (g_track[i].user == user) return i;
    return -1;
}
static void my_free(void* user) {
    if (!user) return;
    check_all("free-entry");
    int i = find_track(user);
    if (i >= 0) g_track[i].user = NULL;
    free((uint8_t*)user - FENCE);
}
static void* my_realloc_sized(void* user, size_t oldsz, size_t newsz) {
    check_all("realloc-entry");
    if (!user) return my_malloc(newsz);
    int i = find_track(user);
    uint8_t* np = (uint8_t*)malloc(newsz + 2*FENCE);
    if (!np) return NULL;
    memset(np, MAGIC, FENCE);
    memset(np + FENCE + newsz, MAGIC, FENCE);
    void* nuser = np + FENCE;
    size_t copy = oldsz < newsz ? oldsz : newsz;
    memcpy(nuser, user, copy);
    if (i >= 0) { g_track[i].user = NULL; }
    free((uint8_t*)user - FENCE);
    if (g_ntrack < MAXTRACK) { g_track[g_ntrack].user = nuser; g_track[g_ntrack].size = newsz; g_ntrack++; }
    return nuser;
}

#define STB_IMAGE_IMPLEMENTATION
#define STBI_MALLOC(sz)             my_malloc(sz)
#define STBI_REALLOC_SIZED(p,os,ns) my_realloc_sized(p,os,ns)
#define STBI_FREE(p)                my_free(p)
#include "include/stb_image.h"

int main(int argc, char** argv) {
    const char* fn = argc > 1 ? argv[1] : "kyuzen.png";
    FILE* f = fopen(fn, "rb");
    if (!f) { fprintf(stderr, "open failed: %s\n", fn); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    unsigned char* raw = my_malloc((size_t)sz);
    fread(raw, 1, (size_t)sz, f); fclose(f);
    fprintf(stderr, "file=%s size=%ld (0x%lX)\n", fn, sz, sz);
    int iw, ih, ch;
    unsigned char* px = stbi_load_from_memory(raw, (int)sz, &iw, &ih, &ch, 4);
    fprintf(stderr, "decoded: %dx%d src_ch=%d px=%p\n", iw, ih, ch, (void*)px);
    check_all("post-decode");
    if (px) { my_free(px); }
    my_free(raw);
    check_all("post-free");
    fprintf(stderr, "ALL CANARIES INTACT — stb did not overflow on this image\n");
    return 0;
}
