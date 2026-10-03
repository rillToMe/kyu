// libs/gui/widget/include/services/uifont.hpp — font UI sistem untuk toolkit.
//
// MENGAPA DI SINI (dan bukan di tiap aplikasi)
// Pilihan font adalah keputusan PENGGUNA, disimpan di `/font.ui` oleh
// Settings > Fonts. Sebelum ini hanya desktop yang membacanya, sehingga
// mengganti font tidak mengubah aplikasi — aplikasi tetap memakai bitmap 8×16.
//
// Menyalin kode pemuatan font ke setiap aplikasi akan menyebarkan pengetahuan
// tentang format berkas, registry font, dan provider — persis duplikasi yang
// dilarang. Karena itu pemuatan font menjadi LAYANAN toolkit (seperti
// clipboard): satu panggilan, satu implementasi, dan setiap aplikasi yang
// memanggilnya otomatis mengikuti pilihan pengguna.
//
// PEMAKAIAN (satu baris di awal run(), sebelum widget dibuat)
//     ui::uifont_install();
//
// Kapan dipanggil penting: lebar/tinggi tombol, label, dan baris daftar
// dihitung dari metrik font AKTIF saat widget dibuat. Memasang font setelah
// widget dibangun meninggalkan ukuran yang salah.
//
// Jika font gagal dimuat (berkas tidak ada / rusak / decode gagal), layanan ini
// TIDAK melaporkan error keras: toolkit kembali ke bitmap 8×16 dan aplikasi
// tetap berjalan. Return 1 = font sistem terpasang, 0 = fallback bitmap.
#ifndef KWIDGET_SERVICES_UIFONT_HPP
#define KWIDGET_SERVICES_UIFONT_HPP

namespace ui {

// Muat font UI dari `/font.ui` + akar KyuzenFS, lalu pasang sebagai text
// provider proses. Idempoten: panggilan kedua tidak menggandakan font.
int uifont_install();

// Lepas provider + bebaskan font (dipanggil Window terakhir, atau eksplisit
// oleh aplikasi yang mau memuat ulang font setelah Settings menyimpan).
void uifont_uninstall();

// 1 bila font sistem aktif (bukan bitmap). Berguna untuk diagnostik.
int uifont_active();

} // namespace ui

#endif // KWIDGET_SERVICES_UIFONT_HPP
