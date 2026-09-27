// libs/media/png.c — Decoder gambar bersama untuk toolkit widget + aplikasi media.
//
// Konfigurasi stb_image identik dengan apps/viewer.c (pola yang sudah
// terbukti): STBI_ONLY_PNG + STBI_ONLY_BMP, memori disambung ke
// sys_alloc/sys_free. Dipisah ke file C sendiri (bukan di toolkit
// libs/gui/widget/) agar kode C stb_image tidak ikut dikompilasi sebagai C++.
//
// Format yang diaktifkan SENGAJA hanya yang benar-benar bisa didekode dengan
// flag build KyuzenOS (freestanding, -msoft-float -mno-sse):
//   PNG — integer (STBI_NO_LINEAR mematikan jalur float)
//   BMP — integer
// JPEG/GIF TIDAK diaktifkan: dekoder JPEG stb memakai aritmetika float sebagai
// inti IDCT-nya, dan build ini tidak boleh mengemisikan FP (tanpa libm).
// include/media.h menandai .jpg/.jpeg sebagai "keluarga gambar" tapi TIDAK
// "supported" — UI menampilkan itu apa adanya, bukan gagal misterius.
//
// png_decode / png_free dipertahankan sebagai nama lama (dipakai ABI internal
// libs/gui/widget/include/primitives/image.hpp). Nama netral image_decode /
// image_free (include/media.h) dipakai aplikasi media baru: yang di-decode
// mendeteksi jenis berkas dari ISI (content sniffing stb), bukan ekstensi —
// file .bmp yang valid memang lolos lewat nama fungsi "png_*".
#include "userlib.h"
#include "media.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ASSERT(x)
#define STBI_MALLOC(sz)                       sys_alloc(sz)
#define STBI_FREE(p)                          sys_free(p)
#define STBI_REALLOC_SIZED(p, old_sz, new_sz) sys_realloc(p, old_sz, new_sz)
#include "stb_image.h"

// Decode gambar -> buffer ARGB8888 (alpha dipertahankan; 0 = transparan penuh,
// 0xFF = opaque). Canvas hasil blend selalu ditulis opaque (0xFF) agar
// deklarasi opaque compositor tetap valid. Return 0 jika gagal / OOM.
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
        out[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    sys_free(px);

    *out_w = iw;
    *out_h = ih;
    return out;
}

void png_free(uint32_t* buf) {
    if (buf) sys_free(buf);
}

// Nama netral (include/media.h) untuk aplikasi media. Satu implementasi:
// hanya delegasi, tidak ada jalur decode kedua.
uint32_t* image_decode(const char* filename, int* out_w, int* out_h) {
    return png_decode(filename, out_w, out_h);
}

void image_free(uint32_t* buf) {
    png_free(buf);
}

// Decode dari buffer memori (konten HTTP) — instance stb yang SAMA, tanpa
// TU kedua. Batas dimensi milik caller (browser: 2048²); di sini hanya
// menolak dimensi gila sebelum alokasi ARGB (>16MP, pola STBI_MAX_DIMENSIONS
// hulu tapi tanpa config global).
uint32_t* image_decode_memory(const uint8_t* data, uint32_t len, int* out_w, int* out_h) {
    if (!data || len == 0 || !out_w || !out_h) return 0;
    int iw = 0, ih = 0;
    uint8_t* px = stbi_load_from_memory(data, (int)len, &iw, &ih, 0, 4);
    if (!px || iw <= 0 || ih <= 0) return 0;
    if ((uint32_t)iw * (uint32_t)ih > (uint32_t)(4096 * 4096)) {
        sys_free(px);
        return 0;
    }
    // Konversi RGBA -> ARGB IN-PLACE (tanpa alokasi kedua: buffer stb milik
    // sys_alloc, jadi image_free/sys_free tetap valid). Baca-dulu-tulis di
    // alamat yang sama per piksel = aman maju.
    uint64_t n = (uint64_t)iw * (uint64_t)ih;
    uint32_t* u = (uint32_t*)(void*)px;
    for (uint64_t i = 0; i < n; i++) {
        uint8_t r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2], a = px[i * 4 + 3];
        u[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
    *out_w = iw;
    *out_h = ih;
    return u;
}
