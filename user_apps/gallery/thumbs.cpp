// user_apps/gallery/thumbs.cpp — implementasi cache thumbnail berbatas.
#include "thumbs.hpp"

namespace gal {

namespace {

void copy_name(char* dst, int cap, const char* src) {
    int i = 0;
    if (src) for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// Nama hasil listing filesystem sudah dipotong ABI (file_info_t.filename[24]);
// perbandingan dibatasi MEDIA_NAME_MAX supaya tahan terhadap pemotongan.
bool name_eq(const char* a, const char* b) {
    int i = 0;
    for (; a[i] && b[i] && i < MEDIA_NAME_MAX - 1; i++)
        if (a[i] != b[i]) return false;
    return true;
}

}  // namespace

ThumbCache::ThumbCache()
    : box_(THUMB_BOX_DEFAULT), budget_(DEFAULT_BUDGET), bytes_(0), tick_(0),
      evict_fn_(0), evict_ud_(0) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        slots_[i].name[0] = '\0';
        slots_[i].px = 0;
        slots_[i].w = slots_[i].h = 0;
        slots_[i].box = 0;
        slots_[i].age = 0;
        slots_[i].state = SLOT_EMPTY;
        slots_[i].pinned = false;
    }
}

ThumbCache::~ThumbCache() { reset(); }

void ThumbCache::set_box(int box) {
    if (box <= 0 || box == box_) return;
    box_ = box;
    reset();
}

void ThumbCache::set_budget(int bytes) {
    if (bytes > 0) budget_ = bytes;
}

void ThumbCache::reset() {
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (slots_[i].px) sys_free(slots_[i].px);
        slots_[i].px = 0;
        slots_[i].name[0] = '\0';
        slots_[i].w = slots_[i].h = 0;
        slots_[i].box = 0;
        slots_[i].age = 0;
        slots_[i].state = SLOT_EMPTY;
        slots_[i].pinned = false;
    }
    bytes_ = 0;
}

void ThumbCache::on_evict(void (*fn)(void*, const char*), void* ud) {
    evict_fn_ = fn;
    evict_ud_ = ud;
}

ThumbCache::Slot* ThumbCache::find(const char* name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < MAX_SLOTS; i++)
        if (slots_[i].state != SLOT_EMPTY && name_eq(slots_[i].name, name))
            return &slots_[i];
    return 0;
}

ThumbCache::Slot* ThumbCache::victim() {
    for (int i = 0; i < MAX_SLOTS; i++)
        if (slots_[i].state == SLOT_EMPTY) return &slots_[i];
    return lru_excluding(0);   // 0 = semua slot dilindungi (kisi > kapasitas cache)
}

ThumbCache::Slot* ThumbCache::lru_excluding(const Slot* skip) {
    Slot* best = 0;
    for (int i = 0; i < MAX_SLOTS; i++) {
        Slot& s = slots_[i];
        if (s.state == SLOT_EMPTY || s.pinned || &s == skip) continue;
        if (!best || s.age < best->age) best = &s;
    }
    return best;
}

void ThumbCache::release(Slot& s, bool notify) {
    if (s.px) {
        sys_free(s.px);
        bytes_ -= s.w * s.h * 4;
        if (bytes_ < 0) bytes_ = 0;
    }
    if (notify && evict_fn_ && s.name[0]) evict_fn_(evict_ud_, s.name);
    s.px = 0;
    s.name[0] = '\0';
    s.w = s.h = 0;
    s.box = 0;
    s.age = 0;
    s.state = SLOT_EMPTY;
    s.pinned = false;
}

void ThumbCache::mark_failed(const char* name) {
    Slot* v = victim();
    if (!v) return;
    release(*v, true);
    copy_name(v->name, MEDIA_NAME_MAX, name);
    v->state = SLOT_FAILED;
    v->age = ++tick_;
}

void ThumbCache::mark_live(const char* const* names, int count) {
    for (int i = 0; i < MAX_SLOTS; i++) slots_[i].pinned = false;
    for (int i = 0; i < count; i++) {
        Slot* s = find(names[i]);
        if (s && s->state == SLOT_READY) s->pinned = true;
    }
}

const uint32_t* ThumbCache::get(const char* path, int* w, int* h) {
    if (!path || !path[0]) return 0;
    const char* nm = media_basename(path);

    Slot* s = find(nm);
    if (s) {
        if (s->state == SLOT_READY) {
            s->age = ++tick_;
            if (w) *w = s->w;
            if (h) *h = s->h;
            return s->px;
        }
        return 0;      // SLOT_FAILED: jangan decode ulang tiap frame
    }

    // Satu decode penuh pada satu waktu: hasilnya dipakai untuk membuat
    // thumbnail lalu dibebaskan (bukan disimpan bersama buffer asli).
    int fw = 0, fh = 0;
    uint32_t* full = image_decode(path, &fw, &fh);
    if (!full || fw <= 1 || fh <= 1) {
        if (full) image_free(full);
        mark_failed(nm);
        return 0;
    }
    int dw = 0, dh = 0;
    if (!media_fit_box(fw, fh, box_, box_, &dw, &dh)) {
        image_free(full);
        mark_failed(nm);
        return 0;
    }

    Slot* v = victim();
    if (!v) { image_free(full); return 0; }     // semua slot dilindungi
    release(*v, true);

    uint32_t* small = (uint32_t*)sys_alloc((uint32_t)(dw * dh * 4));
    if (!small) { image_free(full); return 0; }
    media_scale_rgba(full, fw, fh, small, dw, dh);
    image_free(full);

    // Batas memori: buang slot TERISI paling lama tak terpakai (selain slot `v`
    // yang sedang disiapkan) sampai ada ruang. Batas bersifat "soft + satu
    // thumbnail": bila semua slot tersisa sedang tampil (dilindungi), hasil
    // akhirnya ≤ budget + satu thumbnail, bukan tumbuh tanpa batas.
    for (;;) {
        if (bytes_ + dw * dh * 4 <= budget_) break;
        Slot* extra = lru_excluding(v);
        if (!extra) break;
        release(*extra, true);
    }

    copy_name(v->name, MEDIA_NAME_MAX, nm);
    v->px = small;
    v->w = dw;
    v->h = dh;
    v->box = box_;
    v->state = SLOT_READY;
    v->age = ++tick_;
    v->pinned = false;
    bytes_ += dw * dh * 4;

    if (w) *w = dw;
    if (h) *h = dh;
    return small;
}

int ThumbCache::ready_count() const {
    int n = 0;
    for (int i = 0; i < MAX_SLOTS; i++)
        if (slots_[i].state == SLOT_READY) n++;
    return n;
}

int ThumbCache::failed_count() const {
    int n = 0;
    for (int i = 0; i < MAX_SLOTS; i++)
        if (slots_[i].state == SLOT_FAILED) n++;
    return n;
}

}  // namespace gal
