# ============================================================================
# KyuzenUiXml.cmake — UI deklaratif dari file .xml nyata.
#
#   kyuzen_embed_xml(<target> SOURCES a.xml b.xml)
#
# Menghasilkan header berisi array `ui_xml_<nama>` + `ui_xml_<nama>_len` dari
# setiap file .xml, lalu menaruh direktori hasil generate di include path
# target. Pemanggil cukup:
#
#   #include "ui_xml_data.h"
#   ui_xml_parse(ui_xml_<nama>, ui_xml_<nama>_len, &err);
#
# XML tinggal di ui/xml/ sebagai dokumen sungguhan (bisa di-review, di-diff,
# dan disorot editor), bukan string literal yang terkubur di .cpp.
#
# Kenapa header (bukan `ld -b binary` seperti font): simbol dari ld diturunkan
# dari PATH file, jadi berubah kalau file dipindah. Header yang di-generate
# memberi nama stabil, ikut sistem dependency CMake, dan tidak menambah
# aturan link khusus di tiap app.
# ============================================================================

include_guard(GLOBAL)

# KYUZEN_ROOT sudah di-set oleh build target, TAPI test host adalah invokasi
# CMake terpisah (tests/host/CMakeLists.txt) yang tidak mewarisinya. Turunkan
# dari lokasi file ini sendiri (cmake/ ada tepat di bawah akar repo) supaya
# modul ini bekerja di kedua pohon build tanpa konfigurasi tambahan.
if(NOT DEFINED KYUZEN_ROOT OR KYUZEN_ROOT STREQUAL "")
    get_filename_component(KYUZEN_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(KYUZEN_UI_XML_DIR "${KYUZEN_ROOT}/ui/xml"
    CACHE INTERNAL "Direktori dokumen XML UI deklaratif")

function(kyuzen_embed_xml target)
    cmake_parse_arguments(ARG "" "" "SOURCES;CONSUMERS" ${ARGN})
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "kyuzen_embed_xml(${target}): SOURCES kosong")
    endif()

    # Path absolut: file .xml boleh disebut relatif ke ui/xml/ atau absolut.
    set(_xmls "")
    set(_deps "")
    foreach(_s IN LISTS ARG_SOURCES)
        if(IS_ABSOLUTE "${_s}")
            set(_p "${_s}")
        else()
            set(_p "${KYUZEN_UI_XML_DIR}/${_s}")
        endif()
        if(NOT EXISTS "${_p}")
            message(FATAL_ERROR
                "kyuzen_embed_xml(${target}): XML tidak ada: ${_p}")
        endif()
        list(APPEND _xmls "${_p}")
        list(APPEND _deps "${_p}")
    endforeach()

    set(_gen_dir "${CMAKE_BINARY_DIR}/obj/ui_xml/${target}")
    set(_gen_hdr "${_gen_dir}/ui_xml_data.h")

    # Satu nama output untuk semua app: setiap target punya direktori sendiri,
    # jadi `#include "ui_xml_data.h"` selalu bekerja tanpa nama unik.
    add_custom_command(
        OUTPUT "${_gen_hdr}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_gen_dir}"
        COMMAND ${CMAKE_COMMAND}
                "-DXML_INPUTS=${_xmls}"
                "-DXML_OUTPUT=${_gen_hdr}"
                -P "${KYUZEN_ROOT}/cmake/embed_xml.cmake"
        DEPENDS ${_deps} "${KYUZEN_ROOT}/cmake/embed_xml.cmake"
        COMMENT "Embedding UI XML for ${target}"
        VERBATIM
    )

    # Sumber generated: ditandai GENERATED supaya CMake tidak mengeluh soal
    # file yang belum ada saat configure, dan EXTERNAL_OBJECT untuk target
    # object-library (resep C++ toolkit app).
    set_source_files_properties("${_gen_hdr}" PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
    )

    add_custom_target(${target}-ui-xml DEPENDS "${_gen_hdr}")
    add_dependencies(${target} ${target}-ui-xml)

    # KE MANA INCLUDE DIR DIPASANG
    # Tiga resep build di repo ini menaruh SUMBER di target terpisah dari
    # executable-nya:
    #   kyuzen_add_plain_app  -> <name>-objects   (OBJECT library)
    #   _kyuzen_add_cpp_*_app -> <name>-objects   (OBJECT library)
    #   kyuzen_add_sdk_app    -> <name> itu sendiri (sumber langsung)
    # Jadi direktori hasil generate dipasang ke <target>-objects BILA ada,
    # dan ke <target> juga (SDK app / target biasa). Tanpa ini, header hasil
    # generate tidak terlihat saat kompilasi — gejalanya "file not found"
    # padahal custom command sudah jalan.
    set(_consumers "")
    if(TARGET ${target}-objects)
        list(APPEND _consumers ${target}-objects)
    endif()
    list(APPEND _consumers ${target})
    list(APPEND _consumers ${ARG_CONSUMERS})

    foreach(_consumer IN LISTS _consumers)
        if(TARGET ${_consumer})
            target_include_directories(${_consumer} PRIVATE "${_gen_dir}")
            # Objek library tidak otomatis mewarisi urutan dari executable:
            # deklarasikan ketergantungannya agar header sudah ada saat compile.
            if(_consumer MATCHES "-objects$")
                add_dependencies(${_consumer} ${target}-ui-xml)
            endif()
        endif()
    endforeach()

    set(KYUZEN_UI_XML_GEN_DIR "${_gen_dir}" PARENT_SCOPE)
endfunction()
