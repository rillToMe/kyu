// apps/filemanager/icons.cpp — implementasi cache ikon.
#include "icons.hpp"

namespace fm {
namespace {

constexpr int idx(IconKind k) { return (int)k; }

// Aset yang SUDAH ada di akar KyuzenFS (assets/icons di-embed build; tidak ada
// ikon baru yang dibuat untuk File Manager).
const char* assetFor(IconKind k) {
    switch (k) {
    case IconKind::Folder:  return "/folder.png";
    case IconKind::Image:   return "/image_view.png";
    case IconKind::Text:    return "/notepad.png";
    case IconKind::App:     return "/terminal.png";
    case IconKind::Generic:
    default:                return "/default.png";
    }
}

}  // namespace

const std::uint32_t* IconCache::row(IconKind k) const {
    int i = (int)k;
    if (i < 0 || i >= IconCache::kKinds) return nullptr;
    const std::vector<std::uint32_t>& v = buf_[i][1];
    return v.empty() ? nullptr : v.data();
}

const std::uint32_t* IconCache::grid(IconKind k) const {
    int i = (int)k;
    if (i < 0 || i >= IconCache::kKinds) return nullptr;
    const std::vector<std::uint32_t>& v = buf_[i][0];
    return v.empty() ? nullptr : v.data();
}

void IconCache::loadOne(IconKind k, const char* asset) {
    int w = 0, h = 0;
    std::uint32_t* raw = image_decode(asset, &w, &h);
    if (!raw || w <= 0 || h <= 0) {
        if (raw) image_free(raw);
        return;
    }
    int i = idx(k);
    if (i >= IconCache::kKinds || i < 0) { image_free(raw); return; }

    std::vector<std::uint32_t>& big = buf_[i][0];
    std::vector<std::uint32_t>& small = buf_[i][1];
    big.assign((std::size_t)kGridPx * kGridPx, 0);
    small.assign((std::size_t)kRowPx * kRowPx, 0);
    media_scale_rgba(raw, w, h, big.data(), kGridPx, kGridPx);
    media_scale_rgba(raw, w, h, small.data(), kRowPx, kRowPx);
    image_free(raw);
}

void IconCache::load() {
    // Generik dulu: dipakai sebagai cadangan untuk aset lain yang gagal.
    loadOne(IconKind::Generic, assetFor(IconKind::Generic));
    const std::vector<std::uint32_t>& fallback_row = buf_[idx(IconKind::Generic)][1];
    const std::vector<std::uint32_t>& fallback_grid = buf_[idx(IconKind::Generic)][0];

    for (int i = 0; i < IconCache::kKinds; ++i) {
        IconKind k = (IconKind)i;
        if (k == IconKind::Generic) continue;
        loadOne(k, assetFor(k));
        if (buf_[i][0].empty() && !fallback_grid.empty()) buf_[i][0] = fallback_grid;
        if (buf_[i][1].empty() && !fallback_row.empty()) buf_[i][1] = fallback_row;
    }
}

}  // namespace fm
