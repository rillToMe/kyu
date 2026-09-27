// apps/browser/page.hpp — satu halaman web: URL + dokumen + gambar + layout.
#ifndef BROWSER_PAGE_HPP
#define BROWSER_PAGE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "engine/dom.hpp"
#include "engine/html.hpp"
#include "engine/http.hpp"
#include "engine/layout.hpp"
#include "engine/url.hpp"
#include "font.hpp"
#include "transport.hpp"

namespace browser {

// Batas halaman (bagian dari §53 resource limits, didokumentasikan):
// - gambar per halaman: 16, tiap input ≤512KB, piksel ≤2048² (imgdec)
// - stylesheet eksternal: 8, tiap ≤64KB (fetch lewat MAX_BODY lalu potong)
inline constexpr unsigned long PAGE_MAX_IMAGES = 16;
inline constexpr unsigned long PAGE_MAX_STYLESHEETS = 8;
inline constexpr unsigned long PAGE_CSS_FETCH_CAP = 64ul * 1024ul;
inline constexpr unsigned long PAGE_IMG_FETCH_CAP = 512ul * 1024ul;

struct PageImage {
    std::string src;   // resolved absolute URL (kunci cache)
    std::uint32_t* px = nullptr;
    int w = 0, h = 0;
};

class Page : public layout::ImageSize {
public:
    Page();
    ~Page();
    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;
    Page(Page&&) = default;             // commit atomik butuh move
    Page& operator=(Page&&) = default;

    // Muat URL penuh (fetch + content-type gate + parse + css + layout).
    // Sukses: ganti seluruh state atomik (halaman lama utuh bila gagal).
    // Return FetchError; err_detail diisi untuk error page (URL + alasan).
    http::FetchError load(const Url& url, BrowserFont& font, int viewport_width,
                          std::string& err_detail);

    bool has_page() const { return loaded_; }
    const Url& url() const { return url_; }
    const std::string& title() const { return title_; }
    const layout::Layout& layout() const { return layout_; }
    const std::vector<PageImage>& images() const { return images_; }

    // layout::ImageSize: lookup cache SAJA (fetch eager sebelum layout).
    bool size(const std::string& src, int& w, int& h) const override;

    // Cari piksel cache (lookup gambar saat draw). nullptr bila tak ada.
    const PageImage* find_image(const std::string& src) const;

private:
    Url url_;
    std::string title_;
    dom::Document doc_;
    std::vector<css::Stylesheet> sheets_;  // storage (layout pakai pointer)
    layout::Layout layout_;
    std::vector<PageImage> images_;  // milik commit terakhir; clear_images yg free
    bool loaded_ = false;

    void clear_images();
    // Fetch + decode satu gambar ke `out` (dipanggil eager sebelum layout).
    // PageImage disalin shallow (px shared, dtor no-op); kepemilikan TUNGGAL:
    // hanya vektor yang di-commit (images_) yang di-free via clear_images().
    bool fetch_one(const std::string& abs_src, std::vector<PageImage>& out) const;
};

}  // namespace browser

#endif  // BROWSER_PAGE_HPP
