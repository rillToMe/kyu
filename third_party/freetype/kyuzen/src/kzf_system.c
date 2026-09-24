/***************************************************************************
 *
 * kzf_system.c — Kyuzen FreeType system boundary (pengganti ftsystem.c).
 *
 * docs/CUSTOMIZE hulu membolehkan mengganti ftsystem.c seluruhnya;
 * file ini + kzf_port.c adalah pengganti tersebut. Prinsip: HANYA
 * sediakan yang dipakai set modul minimal; yang tidak didukung GAGAL
 * JUJUR (error code), bukan stub diam yang mengubah perilaku.
 *
 * Disediakan:
 * - FT_Stream_Open ....... DITOLAK (Cannot_Open_Resource). Kyuzen hanya
 *   memakai memory stream (FT_Stream_OpenMemory di ftbase, dan
 *   FT_New_Memory_Face) — tanpa fopen/VFS (Phase 6). Pemakaian path
 *   file (FT_New_Face/FT_Open_Face dgn pathname) gagal bersih di
 *   FT_Stream_New, bukan crash.
 * - FT_Trace_Enable/Disable  no-op. Output trace/debug tidak didukung
 *   (FT_DEBUG_LEVEL_TRACE mati — semua pembaca trace terkompilasi
 *   hilang; perilaku == build upstream non-trace).
 * - FT_New_Memory ........ NULL. FT_Init_FreeType/FT_Done_FreeType
 *   TIDAK didukung (butuh allocator default global; Kyuzen memakai
 *   FT_New_Library + FT_MemoryRec_ -> kz_heap_t, persis yang dipakai
 *   kzraster_ft.c). FT_Init_FreeType yang terpanggil akan gagal bersih
 *   (Out_Of_Memory), bukan memakai heap liar.
 * - FT_Done_Memory ....... no-op (pasangan konstruktor-NULL di atas;
 *   hanya terjangkau via FT_Done_FreeType yang tak didukung).
 *
 * FT_Gzip_Uncompress TIDAK di-stub: src/gzip/ftgzip.c dikompilasi
 * (embedded zlib, alokasi via FT_Memory) sehingga WOFF/.gz tetap
 * jalan di atas allocator Kyuzen.
 *
 */
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYSTEM_H
#include FT_ERRORS_H

/* File-backed stream: tidak didukung — pakai memory stream. */
FT_Error
FT_Stream_Open( FT_Stream   stream,
                const char *filepathname )
{
    (void)stream;
    (void)filepathname;
    return FT_Err_Cannot_Open_Resource;
}

/* Trace/debug output: tidak didukung (no-op aman, lihat header). */
void
FT_Trace_Disable( void )
{
}

void
FT_Trace_Enable( void )
{
}

/* Allocator default global: tidak ada — pakai FT_New_Library + kz_heap. */
FT_Memory
FT_New_Memory( void )
{
    return NULL;
}

void
FT_Done_Memory( FT_Memory  memory )
{
    (void)memory;
}

/* END */
