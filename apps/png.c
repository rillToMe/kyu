// apps/png.c — Decoder PNG bersama untuk toolkit widget (Phase 7).
//
// Konfigurasi stb_image identik dengan user_apps/viewer.c (pola yang sudah
// terbukti): STBI_ONLY_PNG, memori disambung ke sys_alloc/sys_free.
// Dipisah ke file C sendiri (bukan di toolkit libs/widget/) agar 276KB kode C stb_image
// tidak ikut dikompilasi sebagai C++ (build libui tetap ramping).
//
// png_decode: baca file PNG dari KyuzenFS -> decode -> konversi RGBA bytes ke
// XRGB8888 (format canvas gui_window_t). Caller wajib memanggil png_free().
#include "userlib.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ASSERT(x)
#define STBI_MALLOC(sz)                       sys_alloc(sz)
#define STBI_FREE(p)                          sys_free(p)
#define STBI_REALLOC_SIZED(p, old_sz, new_sz) sys_realloc(p, old_sz, new_sz)
#include "stb_image.h"

// Decode PNG -> buffer XRGB8888 (alpha byte dipaksa 0xFF, kompositor hanya
// membedakan 0 vs non-0). Return 0 jika file tak ada / decode gagal / OOM.
// out_w / out_h diisi ukuran gambar (hanya bila return non-0).
uint32_t* png_decode(const char* filename, int* out_w, int* out_h) {
    uint32_t fsize = sys_file_size((char*)filename);
    if (fsize == 0) return 0;

    uint8_t* raw = (uint8_t*)sys_alloc(fsize);
    if (!raw) return 0;
    sys_read_file_to_buffer((char*)filename, (char*)raw, fsize);

    int iw, ih;
    uint8_t* px = stbi_load_from_memory(raw, (int)fsize, &iw, &ih, 0, 4);
    sys_free(raw);
    if (!px) return 0;

    uint32_t* out = (uint32_t*)sys_alloc((uint32_t)(iw * ih * 4));
    if (!out) { sys_free(px); return 0; }
    for (int i = 0; i < iw * ih; i++) {
        const uint8_t* p = px + i * 4;
        out[i] = 0xFF000000 | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    sys_free(px);

    *out_w = iw;
    *out_h = ih;
    return out;
}

void png_free(uint32_t* buf) {
    if (buf) sys_free(buf);
}
