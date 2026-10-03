# ============================================================================
# libs/time/CMakeLists.txt - waktu sipil + zona waktu (POSIX TZ).
#
#   kyuzen-time      kztime.c      aritmetika kalender + parser aturan TZ
#   kyuzen-time-io   kztime_io.c   baca/tulis "/time.ui" (butuh userlib)
#
# DUA TARGET, BUKAN SATU, DAN ITU DISENGAJA:
# kztime.c murni tanpa syscall, sehingga host test bisa meng-link-nya langsung
# (tests/host/unit/time_test.c) dan menguji kalender + DST tanpa boot. Kalau IO
# digabung, test host gagal link karena sys_open/sys_read_fd tidak ada di sana.
# Aplikasi yang butuh membaca preferensi pengguna memakai kyuzen-time-io.
#
# Kenapa TIDAK masuk kernel: kernel hanya perlu melaporkan UTC mentah dari
# CMOS. Seluruh penerjemahan ke waktu lokal milik userspace supaya zona bisa
# diubah tanpa menyentuh kernel, dan supaya bisa diuji tanpa boot.
#
# Tanpa dependensi lain: tidak butuh FreeType, tidak butuh color, tidak butuh
# libgui. Itu yang membuatnya aman dipakai desktop, Settings, dan aplikasi.
# ============================================================================

set(_time_dir "${KYUZEN_ROOT}/libs/time")

# --- logika murni (tanpa syscall) -------------------------------------------
add_library(kyuzen-time OBJECT "${_time_dir}/src/kztime.c")
target_link_libraries(kyuzen-time PRIVATE kyuzen-flags-applib)
target_include_directories(kyuzen-time PUBLIC ${_time_dir}/include)

# --- IO "/time.ui" (syscall userlib) ----------------------------------------
add_library(kyuzen-time-io OBJECT "${_time_dir}/src/kztime_io.c")
target_link_libraries(kyuzen-time-io PRIVATE kyuzen-flags-applib kyuzen-time)
target_include_directories(kyuzen-time-io PUBLIC
    ${_time_dir}/include
    ${KYUZEN_ROOT}/include
)
