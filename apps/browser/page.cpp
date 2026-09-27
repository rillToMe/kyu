// apps/browser/page.cpp — pipeline muat halaman.
#include "page.hpp"

#include "platform.hpp"

namespace browser {
namespace {

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

// "text/html; charset=..." -> izinkan text/* saja. Tanpa header -> anggap html
// (server malas, umum di fixture/embedded). Selain text/* -> tolak jujur.
bool content_is_html(const http::Response& r) {
    std::string ct;
    if (!r.get("content-type", ct)) return true;
    std::string l = ct;
    for (size_t i = 0; i < l.size(); i++)
        if (l[i] >= 'A' && l[i] <= 'Z') l[i] = (char)(l[i] + 32);
    size_t semi = l.find(';');
    std::string mime = (semi == std::string::npos) ? l : l.substr(0, semi);
    while (!mime.empty() && mime[mime.size() - 1] == ' ') mime.resize(mime.size() - 1);
    return starts_with(mime, "text/");
}

}  // namespace

Page::Page() : loaded_(false) {}

Page::~Page() { clear_images(); }

void Page::clear_images() {
    for (size_t i = 0; i < images_.size(); i++) image_free(images_[i].px);
    images_.clear();
}

const PageImage* Page::find_image(const std::string& src) const {
    for (size_t i = 0; i < images_.size(); i++)
        if (images_[i].src == src) return &images_[i];
    return nullptr;
}

bool Page::fetch_one(const std::string& abs_src, std::vector<PageImage>& out) const {
    for (size_t i = 0; i < out.size(); i++)
        if (out[i].src == abs_src) return true;  // sudah di-fetch batch ini
    // Skema halaman: http ATAU https (transport dipilih di bawah).
    // abs_src selalu hasil resolve_url dari base yang fetchable.
    Url u;
    if (parse_url(abs_src, u) != UrlError::Ok || !u.is_fetchable()) return false;
    KyuzenTransport t;
    TlsTransport tt;
    http::Transport& tr =
        u.is_https() ? static_cast<http::Transport&>(tt)
                     : static_cast<http::Transport&>(t);
    http::Response r;
    http::FetchError fe = http::fetch(tr, u, r);
    if (fe != http::FetchError::Ok) return false;
    if (r.body.empty() || r.body.size() > PAGE_IMG_FETCH_CAP) return false;
    int w = 0, h = 0;
    std::uint32_t* px = image_decode_memory(
        reinterpret_cast<const std::uint8_t*>(r.body.data()),
        (std::uint32_t)r.body.size(), &w, &h);
    if (!px) return false;
    if (w <= 0 || h <= 0 || (std::uint32_t)w * (std::uint32_t)h > 2048u * 2048u) {
        image_free(px);  // batas aplikasi (di atas batas lib) -> placeholder
        return false;
    }
    PageImage im;
    im.src = abs_src;
    im.px = px;
    im.w = w;
    im.h = h;
    out.push_back(im);
    return true;
}

bool Page::size(const std::string& src, int& w, int& h) const {
    const PageImage* f = find_image(src);
    if (!f) return false;
    w = f->w;
    h = f->h;
    return true;
}

http::FetchError Page::load(const Url& url, BrowserFont& font, int viewport_width,
                            std::string& err_detail) {
    KyuzenTransport t;
    TlsTransport tt;
    http::Transport& tr =
        url.is_https() ? static_cast<http::Transport&>(tt)
                       : static_cast<http::Transport&>(t);
    http::Response r;
    http::FetchError fe = http::fetch(tr, url, r);
    if (fe != http::FetchError::Ok) {
        // Alasan TLS jujur (X.509) bila ada; lainnya via to_string.
        err_detail = (fe == http::FetchError::Tls && !tt.tls_detail().empty())
            ? tt.tls_detail()
            : http::to_string(fe);
        return fe;
    }
    if (!content_is_html(r)) {
        err_detail = "unsupported content (not text/*)";
        return http::FetchError::BadResponse;
    }
    // Bangun kandidat di state lokal; commit hanya bila layout jadi.
    dom::Document doc;
    html::parse(r.body, doc);
    std::vector<css::Stylesheet> sheets;
    sheets.push_back(css::Stylesheet());  // slot <style> gabungan
    for (size_t i = 0; i < doc.styles.size(); i++)
        css::parse_stylesheet(doc.styles[i], sheets[0]);
    // Stylesheet eksternal: resolve relatif, fetch, cap, gagal = abaikan.
    unsigned long fetched_css = 0;
    Url page_url;
    if (parse_url(r.final_url, page_url) != UrlError::Ok) page_url = url;
    for (size_t i = 0; i < doc.style_hrefs.size() && fetched_css < PAGE_MAX_STYLESHEETS;
         i++) {
        Url css_url;
        if (resolve_url(page_url, doc.style_hrefs[i], css_url) != UrlError::Ok) continue;
        if (!css_url.is_fetchable()) continue;
        http::Transport& ctr =
            css_url.is_https() ? static_cast<http::Transport&>(tt)
                               : static_cast<http::Transport&>(t);
        http::Response cr;
        if (http::fetch(ctr, css_url, cr) != http::FetchError::Ok) continue;
        if (cr.body.empty() || cr.body.size() > PAGE_CSS_FETCH_CAP) continue;
        fetched_css++;
        sheets.push_back(css::Stylesheet());
        css::parse_stylesheet(cr.body, sheets.back());
    }
    std::vector<const css::Stylesheet*> sheet_ptrs;
    for (size_t i = 0; i < sheets.size(); i++) sheet_ptrs.push_back(&sheets[i]);

    // Gambar di-resolve absolut SEKARANG + FETCH EAGER sebelum layout
    // (layout tidak boleh melakukan I/O jaringan; size() hanya lookup).
    // Tulis ulang atribut src di DOM kandidat agar kunci konsisten.
    // (Hanya atribut src <img>; href link tetap mentah, di-resolve saat klik.)
    std::vector<std::string> img_srcs;
    {
        std::vector<dom::Node*> stack;
        stack.push_back(doc.root.get());
        while (!stack.empty()) {
            dom::Node* n = stack.back();
            stack.pop_back();
            if (n->type == dom::NodeType::Element && n->tag == "img") {
                const std::string* s = n->get_attr("src");
                if (s && !s->empty()) {
                    Url iu;
                    if (resolve_url(page_url, *s, iu) == UrlError::Ok) {
                        std::string abs = iu.serialize();
                        for (size_t a = 0; a < n->attrs.size(); a++)
                            if (n->attrs[a].name == "src") n->attrs[a].value = abs;
                        bool dup = false;
                        for (size_t k = 0; k < img_srcs.size(); k++)
                            if (img_srcs[k] == abs) {
                                dup = true;
                                break;
                            }
                        if (!dup && img_srcs.size() < PAGE_MAX_IMAGES)
                            img_srcs.push_back(abs);
                    }
                }
            }
            for (size_t c = 0; c < n->children.size(); c++)
                stack.push_back(n->children[c].get());
        }
    }
    std::vector<PageImage> fresh_imgs;
    for (size_t i = 0; i < img_srcs.size(); i++) fetch_one(img_srcs[i], fresh_imgs);
    // Komit gambar DULU (layout::size butuh cache terisi; layout tidak I/O).
    clear_images();
    images_ = std::move(fresh_imgs);

    layout::Layout nl;
    layout::build_layout(doc, sheet_ptrs, viewport_width, font, *this, nl);
    if (!nl.root) {
        err_detail = "layout failed";
        return http::FetchError::BadResponse;
    }
    // Commit atomik (gambar sudah di images_; sisanya swap).
    url_ = page_url;
    if (parse_url(r.final_url, url_) != UrlError::Ok) url_ = url;
    title_ = doc.title.empty() ? url_.serialize() : doc.title;
    doc_ = std::move(doc);
    sheets_ = std::move(sheets);
    layout_ = std::move(nl);
    loaded_ = true;
    return http::FetchError::Ok;
}

}  // namespace browser
