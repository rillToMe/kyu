# ============================================================================
# embed_xml.cmake — ubah file .xml menjadi header C++ yang bisa di-include.
#
#   cmake -DXML_INPUTS=a.xml;b.xml -DXML_OUTPUT=out.h -P embed_xml.cmake
#
# MENGAPA XML DIPISAH DARI KODE
#   XML yang ditulis sebagai string literal di dalam .cpp tidak terlihat:
#   tidak bisa di-review sebagai dokumen, tidak bisa di-diff dengan enak, dan
#   editor tidak memberinya penyorotan sintaks. Karena itu UI deklaratif
#   KyuzenOS hidup sebagai file .xml nyata di ui/xml/, dan BUILD yang
#   menyambungkannya ke kode — bukan sebaliknya.
#
# MENGAPA BUKAN BLOB BINER (pola `ld -b binary` seperti font)
#   Symbol dari `ld -b binary` diturunkan dari PATH, sehingga nama simbol
#   berubah kalau file dipindah dan tidak terbaca di kode. Header yang
#   di-generate memberi nama simbol yang stabil dan deterministik, plus
#   kompilasi tetap satu jalur biasa.
#
# Format keluaran: satu array `static const char` per file, memakai raw string
# literal C++ (R"KZXML(...)KZXML") supaya tanda kutip dan baris baru di XML
# tidak perlu di-escape. Panjang juga tersedia (`_len`) karena raw string
# literal membawa NUL implisit, dan parser XML libui menerima (data, size).
#
# Header di-generate dengan tipe `char` (bukan `char[]` tanpa panjang) supaya
# pemanggil C maupun C++ bisa memakainya langsung.
# ============================================================================

if(NOT XML_INPUTS)
    message(FATAL_ERROR "embed_xml: XML_INPUTS is required")
endif()
if(NOT XML_OUTPUT)
    message(FATAL_ERROR "embed_xml: XML_OUTPUT is required")
endif()

set(_out "// ============================================================================\n")
string(APPEND _out "// AUTO-GENERATED oleh cmake/embed_xml.cmake — JANGAN EDIT FILE INI.\n")
string(APPEND _out "//\n")
string(APPEND _out "// Sumber: file .xml di ui/xml/. Edit XML-nya, bukan header ini.\n")
string(APPEND _out "// Header ini ditulis ulang setiap kali salah satu file XML berubah.\n")
string(APPEND _out "// ============================================================================\n")
string(APPEND _out "#ifndef KZ_UI_XML_DATA_H\n")
string(APPEND _out "#define KZ_UI_XML_DATA_H\n\n")

foreach(_file IN LISTS XML_INPUTS)
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "embed_xml: input tidak ada: ${_file}")
    endif()
    get_filename_component(_base "${_file}" NAME_WE)
    # Nama simbol: ui_xml_<base>, dengan '-' -> '_' (nama file boleh pakai '-').
    string(REPLACE "-" "_" _sym_base "${_base}")
    string(TOLOWER "${_sym_base}" _sym_base)

    file(READ "${_file}" _xml)

    # Buang BOM UTF-8 kalau ada: parser libui membaca byte apa adanya dan BOM
    # di depan '<' akan terbaca sebagai teks ilegal (UI_XML_BAD_TEXT).
    # CMake tidak menerima escape `\x` di regex, jadi BOM dibandingkan sebagai
    # byte literal lewat `string(SUBSTRING)`.
    string(SUBSTRING "${_xml}" 0 3 _head3)
    string(ASCII 239 187 191 _bom)
    if(_head3 STREQUAL _bom)
        string(SUBSTRING "${_xml}" 3 -1 _xml)
    endif()

    # Tolak delimiter yang akan menutup raw string lebih awal. Kalau ini
    # muncul, ganti penanda di file XML-nya.
    string(FIND "${_xml}" ")KZXML\"" _bad)
    if(NOT _bad EQUAL -1)
        message(FATAL_ERROR
            "embed_xml: ${_file} memuat urutan ')KZXML\"' yang menutup raw "
            "string literal. Ganti penanda KZXML di embed_xml.cmake atau "
            "hapus urutan itu dari XML.")
    endif()

    string(APPEND _out "// ${_file}\n")
    string(APPEND _out "static const char ui_xml_${_sym_base}[] =\n")
    string(APPEND _out "    R\"KZXML(${_xml})KZXML\";\n")
    # Panjang dokumen (tanpa NUL): parser libui menerima (data, size) eksplisit.
    string(LENGTH "${_xml}" _len)
    string(APPEND _out "static const unsigned ui_xml_${_sym_base}_len = ${_len}u;\n\n")
endforeach()

string(APPEND _out "#endif // KZ_UI_XML_DATA_H\n")

# Tulis hanya kalau berubah: mencegah rebuild berantai yang tidak perlu
# (ninja akan menganggap target kotor setiap kali file ditulis).
set(_old "")
if(EXISTS "${XML_OUTPUT}")
    file(READ "${XML_OUTPUT}" _old)
endif()
if(NOT _old STREQUAL _out)
    file(WRITE "${XML_OUTPUT}" "${_out}")
endif()
