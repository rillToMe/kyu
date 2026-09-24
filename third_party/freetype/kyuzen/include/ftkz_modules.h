/***************************************************************************
 *
 * ftkz_modules.h — Kyuzen FreeType module list (minimal milestone).
 *
 * Dipakai via -DFT_CONFIG_MODULES_H="ftkz_modules.h" (mekanisme resmi
 * ftinit.c_pt — tanpa patch upstream, tanpa modules.cfg, tanpa GNU make).
 *
 * Isi = kebutuhan TrueType + grayscale, bukan yang lain:
 *   truetype (glyph loading + bytecode interpreter bawaan modul)
 *   sfnt     (wrapper SFNT; butuh psnames)
 *   psnames  (nama glyph PostScript)
 *   smooth   (rasterizer anti-alias grayscale)
 *
 * SENGAJA MATI: autofit, type1, cff, cid, pfr, type42, winfonts, pcf,
 * bdf, psaux (hanya untuk driver PS), pshinter, gzip, lzw, bzip2,
 * cache, gxvalid, otvalid, raster-mono, svg, sdf. Jangan menambah
 * modul tanpa milestone yang membutuhkannya.
 *
 */
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )

/* EOF */
