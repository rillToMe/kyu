// apps/browser/browser_app.cpp — chrome XML + navigasi + viewport FreeType.
#include "browser_app.hpp"

#include "engine/strutil.hpp"
#include "platform.hpp"

namespace browser {
namespace {

// Chrome deklaratif: toolbar + status + panel error + slot viewport.
// Viewport (ScrollView+FtText) dipasang native ke viewport_slot — XML tak
// bisa menyatakan widget kustom (terdokumentasi, pola Phase F).
const char BROWSER_XML[] =
    "<window>"
    "<vbox spacing=\"6\" id=\"root\">"
    "<hbox spacing=\"6\" id=\"toolbar\">"
    "<button id=\"back\" text=\"Back\"/>"
    "<button id=\"fwd\" text=\"Fwd\"/>"
    "<button id=\"reload\" text=\"Reload\"/>"
    "<textbox id=\"addr\" width=\"520\"/>"
    "<button id=\"go\" text=\"Go\" variant=\"primary\"/>"
    "</hbox>"
    "<label id=\"status\" text=\"Ready\"/>"
    "<vbox spacing=\"6\" id=\"errbox\" visible=\"false\">"
    "<label text=\"Unable to load page\"/>"
    "<label id=\"err_url\" text=\"\"/>"
    "<label id=\"err_reason\" text=\"\"/>"
    "<hbox spacing=\"6\">"
    "<button id=\"err_retry\" text=\"Retry\" variant=\"primary\"/>"
    "<button id=\"err_back\" text=\"Back\"/>"
    "</hbox>"
    "</vbox>"
    "<vbox spacing=\"0\" id=\"viewport_slot\"/>"
    "</vbox>"
    "</window>";

void on_go(void* ud) { BrowserApp::self(ud)->onGo(); }
void on_addr(void* ud) { BrowserApp::self(ud)->onAddrEnter(); }
void on_back(void* ud) { BrowserApp::self(ud)->onBack(); }
void on_fwd(void* ud) { BrowserApp::self(ud)->onFwd(); }
void on_reload(void* ud) { BrowserApp::self(ud)->onReload(); }
void on_retry(void* ud) { BrowserApp::self(ud)->onRetry(); }
void on_err_back(void* ud) { BrowserApp::self(ud)->onErrBack(); }
void on_view_click(void* ud, int x, int y) { BrowserApp::self(ud)->onViewClick(x, y); }
void on_draw(void* ud, std::uint32_t* canvas, int cw, int ch, int x, int y) {
    BrowserApp::self(ud)->drawView(canvas, cw, ch, x, y);
}

}  // namespace

BrowserApp::BrowserApp() {}
BrowserApp::~BrowserApp() {
    if (buf_) sys_free(buf_);
}

void BrowserApp::log(const char* tag, const std::string& msg) {
    std::string line = std::string("[browser") + tag + "] " + msg;
    print(const_cast<char*>(line.c_str()));
}

bool BrowserApp::build_chrome() {
    win_ = ui_window_create(WIN_W, WIN_H);
    if (!win_) return false;
    ui_window_set_title(win_, "Browser");
    unsigned n = 0;
    while (BROWSER_XML[n]) n++;
    ui_xml_error_t err;
    ui_xml_doc_t* doc = ui_xml_parse(BROWSER_XML, n, &err);
    if (!doc) return false;
    xctx_ = ui_xml_ctx_create(win_);
    if (!xctx_) {
        ui_xml_doc_destroy(doc);
        return false;
    }
    if (!ui_xml_inflate(xctx_, doc, &err)) {
        ui_xml_doc_destroy(doc);
        return false;
    }
    ui_xml_doc_destroy(doc);
    addr_ = ui_xml_find(xctx_, "addr");
    status_ = ui_xml_find(xctx_, "status");
    errbox_ = ui_xml_find(xctx_, "errbox");
    err_url_ = ui_xml_find(xctx_, "err_url");
    err_reason_ = ui_xml_find(xctx_, "err_reason");
    ui_widget_t* slot = ui_xml_find(xctx_, "viewport_slot");
    if (!addr_ || !status_ || !errbox_ || !err_url_ || !err_reason_ || !slot)
        return false;
    ui_xml_bind(xctx_, "go", UI_XML_ON_CLICK, on_go, this);
    ui_xml_bind(xctx_, "addr", UI_XML_ON_CHANGE, on_addr, this);
    ui_xml_bind(xctx_, "back", UI_XML_ON_CLICK, on_back, this);
    ui_xml_bind(xctx_, "fwd", UI_XML_ON_CLICK, on_fwd, this);
    ui_xml_bind(xctx_, "reload", UI_XML_ON_CLICK, on_reload, this);
    ui_xml_bind(xctx_, "err_retry", UI_XML_ON_CLICK, on_retry, this);
    ui_xml_bind(xctx_, "err_back", UI_XML_ON_CLICK, on_err_back, this);

    // Viewport native: ScrollView (roda + scrollbar bawaan) + FtText
    // (canvas mentah + klik koordinat). Ukuran fix (tanpa resize ABI).
    scroll_ = ui_scrollview_create(win_, VIEW_W, VIEW_H);
    ft_ = ui_fttext_create(win_, LAYOUT_W, PAGE_MAX_H);
    if (!scroll_ || !ft_) return false;
    ui_fttext_set_draw(ft_, on_draw, this);
    ui_fttext_set_click(ft_, on_view_click, this);
    ui_scrollview_set_child(scroll_, ft_);
    ui_layout_add(slot, scroll_);

    // SENGAJA tanpa shortcut keyboard: registry shortcut window fire SEBELUM
    // dispatch fokus (window.hpp), sehingga huruf biasa (Space/b/0/r/[/])
    // akan membajak ketikan address bar. Navigasi = tombol mouse; scroll =
    // roda/scrollbar (jalur Scrollable bawaan). Shortcut sadar-fokus = fase
    // berikutnya (butuh focus-getter ABI yang belum ada).
    return true;
}

int BrowserApp::run(const char* initial_url) {
    bool font_ok = font_.load();
    if (!build_chrome()) return 1;
    if (!font_ok) set_status("Font missing: text disabled");
    log("", "started");
    if (initial_url && initial_url[0]) {
        ui_textbox_set_text(addr_, initial_url);
        navigate_text(initial_url, true);
    } else {
        set_status("Enter a URL, then Go");
    }
    ui_window_run(win_);
    ui_window_destroy(win_);
    if (xctx_) ui_xml_ctx_destroy(xctx_);
    return 0;
}

void BrowserApp::set_status(const std::string& s) {
    if (status_) ui_label_set_text(status_, s.c_str());
}

void BrowserApp::navigate_text(const std::string& text, bool push) {
    // Kebijakan address bar (§41): tanpa skema -> http://. Schéma lain yang
    // parse_url tolak (ftp:/...) = error jujur, bukan tebakan.
    std::string t = text;
    // Trim spasi luar (ketikan pengguna).
    while (!t.empty() && (t[0] == ' ' || t[0] == '\t')) t.erase(t.begin());
    while (!t.empty() && (t[t.size() - 1] == ' ' || t[t.size() - 1] == '\t'))
        t.resize(t.size() - 1);
    if (t.empty()) {
        show_error("", "empty address");
        return;
    }
    if (t.find("://") == std::string::npos) t = detail::lit_plus("http://", t);
    Url u;
    UrlError ue = parse_url(t, u);
    if (ue != UrlError::Ok) {
        show_error(t, to_string(ue));
        return;
    }
    navigate_url(u, push);
}

void BrowserApp::navigate_url(const Url& url, bool push) {
    std::uint64_t my_nav = ++nav_id_;
    set_status("Loading...");
    // Catatan §43: fetch SINKRON di GUI thread (tanpa thread di subset C++).
    // Status "Loading..." mungkin belum terpaint saat handler diblokir —
    // ini batasan terdokumentasi, bukan bug: timeout membatasi blokir.
    Page next;
    std::string detail;
    http::FetchError fe = next.load(url, font_, LAYOUT_W, detail);
    if (my_nav != nav_id_) return;  // navigasi lebih baru menyalip (masa depan)
    if (fe != http::FetchError::Ok) {
        log("/net", url.serialize() + " -> " + http::to_string(fe));
        show_error(url.serialize(), detail.empty() ? http::to_string(fe) : detail);
        return;
    }
    // Sukses: ganti halaman atomik (next dibangun penuh SEBELUM commit —
    // halaman lama utuh bila gagal, §37). Page move-assignable (lihat page.hpp).
    page_ = std::move(next);
    show_page();
    if (push) {
        // Potong forward-branch saat navigasi baru dari tengah riwayat.
        while ((int)history_.size() > hist_idx_ + 1) history_.pop_back();
        history_.push_back(page_.url());
        hist_idx_ = (int)history_.size() - 1;
        if (history_.size() > 50) {
            history_.erase(history_.begin());
            hist_idx_--;
        }
    }
    std::string nav = page_.url().serialize() + " [";
    nav += page_.title();
    nav += "]";
    log("/nav", nav);
}

void BrowserApp::show_page() {
    ui_widget_set_visible(errbox_, 0);
    ui_widget_set_visible(scroll_, 1);
    ui_scrollview_set_scroll(scroll_, 0);
    ui_window_set_title(win_, page_.title().c_str());
    ui_textbox_set_text(addr_, page_.url().serialize().c_str());
    set_status(page_.url().serialize());
    ui_fttext_refresh(ft_);
    log_links();
}

void BrowserApp::show_error(const std::string& url, const std::string& reason) {
    last_error_url_ = url;
    last_error_reason_ = reason;
    ui_widget_set_visible(scroll_, 0);
    ui_widget_set_visible(errbox_, 1);
    ui_label_set_text(err_url_, detail::lit_plus("URL: ", url).c_str());
    ui_label_set_text(err_reason_, detail::lit_plus("Reason: ", reason).c_str());
    set_status(detail::lit_plus("Error: ", reason));
    log("/error", url + " :: " + reason);
}

void BrowserApp::onGo() { onAddrEnter(); }

void BrowserApp::onAddrEnter() {
    const char* t = ui_textbox_text(addr_);
    navigate_text(t ? t : "", true);
}

void BrowserApp::onBack() {
    if (hist_idx_ > 0) {
        hist_idx_--;
        navigate_url(history_[hist_idx_], false);
    }
}

void BrowserApp::onFwd() {
    if (hist_idx_ + 1 < (int)history_.size()) {
        hist_idx_++;
        navigate_url(history_[hist_idx_], false);
    }
}

void BrowserApp::onReload() {
    if (page_.has_page()) navigate_url(page_.url(), false);
}

void BrowserApp::onRetry() {
    if (!last_error_url_.empty()) navigate_text(last_error_url_, false);
}

void BrowserApp::onErrBack() {
    if (page_.has_page())
        show_page();
    else {
        ui_widget_set_visible(errbox_, 0);
        ui_widget_set_visible(scroll_, 1);
        set_status("Enter a URL, then Go");
    }
}

void BrowserApp::onScrollKey(int dir) {
    if (!scroll_) return;
    int cur = ui_scrollview_scroll(scroll_);
    int step = VIEW_H - 40;
    if (dir > 0) ui_scrollview_set_scroll(scroll_, cur + step);
    else if (dir < 0) ui_scrollview_set_scroll(scroll_, cur - step);
    else ui_scrollview_set_scroll(scroll_, 0);
}

void BrowserApp::onViewClick(int mx, int my) {
    if (!page_.has_page() || !ft_) return;
    int fx = 0, fy = 0;
    ui_widget_pos(ft_, &fx, &fy);
    int cx = mx - fx, cy = my - fy;
    std::string href;
    if (layout::hit_link(page_.layout(), cx, cy, href) && !href.empty()) {
        Url next;
        if (resolve_url(page_.url(), href, next) != UrlError::Ok) {
            show_error(href, to_string(UrlError::BadScheme));
            return;
        }
        if (!next.is_http()) {
            show_error(next.serialize(), "unsupported scheme (need http://)");
            return;
        }
        log("/link", href + " -> " + next.serialize());
        navigate_url(next, true);
    }
}

void BrowserApp::log_links() {
    if (!page_.has_page() || !page_.layout().root) return;
    // Walk layout, log setiap run link (probe deterministik butuh koordinat).
    std::vector<const layout::Box*> stack;
    stack.push_back(page_.layout().root.get());
    int vx = 0, vy = 0;
    if (ft_) ui_widget_pos(ft_, &vx, &vy);
    std::string org = "origin ";
    detail::append_ulong(org, (unsigned long)vx);
    org += ",";
    detail::append_ulong(org, (unsigned long)vy);
    log("/view", org);
    int n_links = 0, n_imgs = 0;
    while (!stack.empty()) {
        const layout::Box* b = stack.back();
        stack.pop_back();
        for (size_t i = 0; i < b->lines.size(); i++)
            for (size_t r = 0; r < b->lines[i].runs.size(); r++) {
                const layout::Run& run = b->lines[i].runs[r];
                if (run.is_image) {
                    n_imgs++;
                    const PageImage* im = page_.find_image(run.img_src);
                    std::string m = run.img_src + " ";
                    detail::append_ulong(m, (unsigned long)(im ? im->w : 0));
                    m += "x";
                    detail::append_ulong(m, (unsigned long)(im ? im->h : 0));
                    m += (run.img_broken || !im) ? " BROKEN" : " ok";
                    log("/img", m);
                    continue;
                }
                if (!run.link || run.href.empty()) continue;
                n_links++;
                std::string m = run.href + " ";
                detail::append_ulong(m, (unsigned long)run.x);
                m += " ";
                detail::append_ulong(m, (unsigned long)run.y);
                m += " ";
                detail::append_ulong(m, (unsigned long)run.w);
                m += " ";
                detail::append_ulong(m, (unsigned long)run.h);
                log("/link", m);
            }
        for (size_t i = 0; i < b->kids.size(); i++) stack.push_back(b->kids[i].get());
    }
    std::string sum = "links=";
    detail::append_ulong(sum, (unsigned long)n_links);
    sum += " imgs=";
    detail::append_ulong(sum, (unsigned long)n_imgs);
    log("/content", sum);
}

// ---------- Viewport rendering ----------

bool BrowserApp::ensure_buf(int w, int h) {
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
    if (buf_ && buf_w_ == w && buf_h_ == h) return true;
    if (buf_) {
        sys_free(buf_);
        buf_ = nullptr;
    }
    buf_ = static_cast<std::uint32_t*>(sys_alloc((std::uint32_t)(w * h * 4)));
    if (!buf_) return false;
    buf_w_ = w;
    buf_h_ = h;
    return true;
}

void BrowserApp::fill_rect(int x0, int y0, int x1, int y1, std::uint32_t argb) {
    if (!buf_) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > buf_w_) x1 = buf_w_;
    if (y1 > buf_h_) y1 = buf_h_;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) buf_[y * buf_w_ + x] = argb;
}

void BrowserApp::blend_px(int x, int y, std::uint32_t src) {
    if (!buf_ || x < 0 || y < 0 || x >= buf_w_ || y >= buf_h_) return;
    std::uint32_t a = src >> 24;
    if (a == 0) return;
    if (a == 255) {
        buf_[y * buf_w_ + x] = src | 0xFF000000u;
        return;
    }
    std::uint32_t d = buf_[y * buf_w_ + x];
    std::uint32_t sr = (src >> 16) & 0xFFu, sg = (src >> 8) & 0xFFu, sb = src & 0xFFu;
    std::uint32_t dr = (d >> 16) & 0xFFu, dg = (d >> 8) & 0xFFu, db = d & 0xFFu;
    std::uint32_t na = 255u - a;
    buf_[y * buf_w_ + x] = 0xFF000000u | (((sr * a + dr * na) / 255u) << 16) |
                           (((sg * a + dg * na) / 255u) << 8) | ((sb * a + db * na) / 255u);
}

void BrowserApp::blit_run(const layout::Run& r, int dx, int dy) {
    int x0 = r.x + dx, y0 = r.y + dy;
    if (x0 + r.w <= 0 || y0 + r.h <= 0 || x0 >= buf_w_ || y0 >= buf_h_) return;
    if (!r.is_image) return;
    const PageImage* im = page_.find_image(r.img_src);
    if (!im || !im->px) {
        // Placeholder rusak: kotak garis + silang (deterministik).
        std::uint32_t edge = 0xFFAA0000u;
        for (int x = x0; x < x0 + r.w; x++) {
            blend_px(x, y0, edge);
            blend_px(x, y0 + r.h - 1, edge);
        }
        for (int y = y0; y < y0 + r.h; y++) {
            blend_px(x0, y, edge);
            blend_px(x0 + r.w - 1, y, edge);
        }
        int n = r.w < r.h ? r.w : r.h;
        for (int k = 0; k < n; k++) {
            blend_px(x0 + k * r.w / (n ? n : 1), y0 + k * r.h / (n ? n : 1), edge);
        }
        return;
    }
    // Nearest-neighbor ke rect run (layout sudah scale-down).
    for (int py = 0; py < r.h; py++) {
        int sy = py * im->h / (r.h ? r.h : 1);
        for (int px = 0; px < r.w; px++) {
            int sx = px * im->w / (r.w ? r.w : 1);
            blend_px(x0 + px, y0 + py, im->px[sy * im->w + sx]);
        }
    }
}

void BrowserApp::drawView(std::uint32_t* canvas, int cw, int ch, int x, int y) {
    if (!canvas || !scroll_ || !ft_) return;
    // Ukuran widget FtText = area gambar (h fix PAGE_MAX_H, pakai VIEW_H).
    if (!ensure_buf(LAYOUT_W, VIEW_H)) return;
    int scroll = ui_scrollview_scroll(scroll_);
    std::uint32_t bg = 0xFFFFFFFFu;
    if (page_.has_page() && page_.layout().root) {
        const layout::Box* root = page_.layout().root.get();
        if (root->style.has_bg) bg = root->style.bg;
    }
    fill_rect(0, 0, buf_w_, buf_h_, bg);
    if (page_.has_page() && page_.layout().root && font_.loaded()) {
        // Walk boxes: bg box, hr, runs teks/gambar.
        std::vector<const layout::Box*> stack;
        stack.push_back(page_.layout().root.get());
        while (!stack.empty()) {
            const layout::Box* b = stack.back();
            stack.pop_back();
            if (b->style.has_bg && b->w > 0 && b->h > 0)
                fill_rect(b->x, b->y - scroll, b->x + b->w, b->y + b->h - scroll,
                          b->style.bg);
            if (b->is_hr)
                fill_rect(b->x, b->y - scroll, b->x + b->w, b->y + b->h - scroll,
                          0xFF888888u);
            for (size_t i = 0; i < b->lines.size(); i++)
                for (size_t r = 0; r < b->lines[i].runs.size(); r++) {
                    const layout::Run& run = b->lines[i].runs[r];
                    int ry = run.y - scroll;
                    if (ry + run.h <= 0 || ry >= buf_h_) continue;
                    if (run.is_image) {
                        blit_run(run, 0, -scroll);
                        continue;
                    }
                    font_.use_size(run.style.font_size);
                    int baseline = ry + run.h - run.style.font_size / 4;
                    font_.draw(buf_, (std::uint32_t)buf_w_, (std::uint32_t)buf_h_,
                               run.x, baseline, run.style.color, run.text,
                               run.style.bold);
                    if (run.link) {
                        // Underline link (§29): 1px di bawah teks.
                        int uy = ry + run.h - 1;
                        if (uy >= 0 && uy < buf_h_)
                            for (int px = run.x; px < run.x + run.w; px++)
                                blend_px(px, uy, run.style.color | 0xFF000000u);
                    }
                }
            for (size_t i = 0; i < b->kids.size(); i++) stack.push_back(b->kids[i].get());
        }
    } else if (!font_.loaded()) {
        // Tanpa font: status sudah menjelaskan; viewport tetap bg bersih.
    }
    // Copy buf -> canvas pada origin widget, clip batas canvas.
    for (int j = 0; j < buf_h_; j++) {
        int cy = y + j;
        if (cy < 0 || cy >= ch) continue;
        for (int i = 0; i < buf_w_; i++) {
            int cx = x + i;
            if (cx < 0 || cx >= cw) continue;
            canvas[cy * cw + cx] = buf_[j * buf_w_ + i];
        }
    }
}

}  // namespace browser
