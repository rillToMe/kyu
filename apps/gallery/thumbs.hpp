// apps/gallery/thumbs.hpp — cache thumbnail berbatas (LRU) untuk kisi.
//
// ARSITEKTUR (yang membuat Gallery tidak men-decode seluruh koleksi):
//
//   direktori → media_scan() → media_entry_t[]        (metadata saja)
//                  ↓ hanya sel yang terlihat (GridView::visible_range)
//           get(path): decode 1 gambar → perkecil ke kotak → simpan di slot
//                  ↓
//              GridView menggambar buffer milik slot ini (non-owning)
//
// Yang menjaga memori tetap terbatas:
//   * Cache memiliki SEMUA buffer (satu slot = satu gambar). Gallery hanya
//     memegang pointer; tidak ada salinan buffer di sisi aplikasi.
//   * Thumbnail diskalakan SEKALI saat masuk cache (media_scale_rgba) — tidak
//     ada penskalaan ulang per frame, dan bukan buffer penuh-resolusi.
//   * Budget byte: slot paling lama TAK TERPAKAI dibuang lebih dulu (LRU).
//     Hasil decode penuh dipakai sesaat lalu langsung dibebaskan, satu gambar
//     pada satu waktu (bukan N buffer full-resolusi).
//   * Slot yang sedang TAMPIL dilindungi lewat mark_live(): cache tidak pernah
//     membebaskan buffer yang pointer-nya masih dipegang GridView. Saat sebuah
//     slot benar-benar dibuang, cache memanggil evict callback supaya Gallery
//     mengosongkan sel itu (tidak ada dangling pointer).
//   * Decode yang gagal dicatat sebagai SLOT_FAILED — tidak dicoba lagi tiap
//     frame.
#ifndef GALLERY_THUMBS_HPP
#define GALLERY_THUMBS_HPP

#include "platform.hpp"

namespace gal {

class ThumbCache {
public:
    enum { MAX_SLOTS = 64, THUMB_BOX_DEFAULT = 110 };
    // ~1.5 MB: cukup untuk belasan thumbnail 110px sekaligus (yang terlihat di
    // kisi hanya ~15 sel), jauh di bawah biaya satu gambar penuh-resolusi.
    enum { DEFAULT_BUDGET = 1536 * 1024 };

    enum { SLOT_EMPTY = 0, SLOT_READY = 1, SLOT_FAILED = 2 };

    struct Slot {
        char     name[MEDIA_NAME_MAX];   // basename = kunci (unik per direktori)
        uint32_t* px;                    // dimiliki cache
        int      w, h;
        int      box;                    // kotak saat dibuat (validasi skala)
        uint64_t age;                    // stempel LRU
        uint8_t  state;
        bool     pinned;                 // sedang tampil → tidak boleh dibuang
    };

    ThumbCache();
    ~ThumbCache();

    // Ukuran kotak thumbnail (px). Berubah → cache dikosongkan (isi lama
    // dibuat untuk skala lain).
    void set_box(int box);
    int  box() const { return box_; }

    // Batas memori cache (byte). Default DEFAULT_BUDGET; disediakan supaya
    // pemanggil (atau pengujian) bisa memperketat batas tanpa menyentuh
    // mekanisme eviction-nya.
    void set_budget(int bytes);
    int  budget() const { return budget_; }

    // Buang semua isi. Digunakan saat daftar berkas berubah (rescan); TIDAK
    // memanggil evict callback — Gallery sudah membersihkan kisinya sendiri.
    void reset();

    // Dipanggil cache saat sebuah slot dibuang karena tekanan memori:
    // app menandai ulang sel dengan nama itu (jangan simpan pointer lama).
    void on_evict(void (*fn)(void* ud, const char* name), void* ud);

    // Tandai thumbnail mana yang SEDANG TAMPIL (hanya ini yang dilindungi).
    // Dipanggil Gallery sekali per tick dengan sel-sel yang terlihat.
    void mark_live(const char* const* names, int count);

    // Buffer thumbnail untuk `path` (dimiliki cache), atau 0 bila gagal /
    // tidak ada ruang yang bisa dibebaskan. *w/*h = ukuran buffer.
    const uint32_t* get(const char* path, int* w, int* h);

    // Statistik ringan (dipakai jejak serial + verifikasi batas memori).
    int used_bytes() const { return bytes_; }
    int ready_count() const;
    int failed_count() const;

private:
    Slot slots_[MAX_SLOTS];
    int  box_;
    int  budget_;
    int  bytes_;
    uint64_t tick_;
    void (*evict_fn_)(void*, const char*);
    void* evict_ud_;

    Slot* find(const char* name);
    Slot* victim();                       // slot kosong / LRU tidak terpakai
    // Slot TERISI paling lama tak terpakai, selain `skip` (dipakai pencarian
    // ruang: slot kosong tidak "membebaskan" byte apa pun).
    Slot* lru_excluding(const Slot* skip);
    void  release(Slot& s, bool notify);  // bebaskan isi slot
    void  mark_failed(const char* name);  // catat kegagalan tanpa retry
};

}  // namespace gal

#endif // GALLERY_THUMBS_HPP
