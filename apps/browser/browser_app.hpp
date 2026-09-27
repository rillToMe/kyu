// apps/browser/browser_app.hpp — aplikasi browser (chrome XML + viewport).
#ifndef BROWSER_APP_HPP
#define BROWSER_APP_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "engine/url.hpp"
#include "font.hpp"
#include "page.hpp"

typedef struct ui_window ui_window_t;
typedef struct ui_widget ui_widget_t;
typedef struct ui_xml_ctx ui_xml_ctx_t;
typedef struct ui_xml_doc ui_xml_doc_t;

namespace browser {

// Geometri viewport (sinkron dengan XML + ukuran window; terdokumentasi):
// window 860x640, toolbar ~32, status ~20, margin/spacing 8/6.
inline constexpr int WIN_W = 860;
inline constexpr int WIN_H = 640;
inline constexpr int VIEW_W = 824;    // ScrollView (root 860 - margin 2x8 - bar?)
inline constexpr int LAYOUT_W = 812;  // konten (scrollbar 12)
inline constexpr int VIEW_H = 470;
inline constexpr int PAGE_MAX_H = 8000;  // FtText fixed (tanpa widget-resize ABI)

class BrowserApp {
public:
    BrowserApp();
    ~BrowserApp();
    int run(const char* initial_url);

    // --- Callback (thunk C di bawah) ---
    void onGo();
    void onAddrEnter();
    void onBack();
    void onFwd();
    void onReload();
    void onRetry();
    void onErrBack();
    void onViewClick(int mx, int my);
    void onScrollKey(int dir);  // +1/-1 halaman, 0 = atas
    void drawView(std::uint32_t* canvas, int cw, int ch, int x, int y);

    static BrowserApp* self(void* ud) { return static_cast<BrowserApp*>(ud); }

private:
    ui_window_t* win_ = nullptr;
    ui_xml_ctx_t* xctx_ = nullptr;
    ui_widget_t* addr_ = nullptr;
    ui_widget_t* status_ = nullptr;
    ui_widget_t* errbox_ = nullptr;
    ui_widget_t* err_url_ = nullptr;
    ui_widget_t* err_reason_ = nullptr;
    ui_widget_t* scroll_ = nullptr;
    ui_widget_t* ft_ = nullptr;

    BrowserFont font_;
    Page page_;
    std::vector<Url> history_;
    int hist_idx_ = -1;
    std::uint64_t nav_id_ = 0;  // token navigasi (§44; sinkron kini, siap async)
    std::string last_error_url_;
    std::string last_error_reason_;

    // Offscreen viewport (w=VIEW_W... gunakan lebar FtText aktual) + logika.
    std::uint32_t* buf_ = nullptr;
    int buf_w_ = 0, buf_h_ = 0;

    bool build_chrome();
    void navigate_text(const std::string& text, bool push);
    void navigate_url(const Url& url, bool push);
    void show_page();
    void show_error(const std::string& url, const std::string& reason);
    void set_status(const std::string& s);
    void log_links();
    bool ensure_buf(int w, int h);
    void blit_run(const layout::Run& r, int dx, int dy);
    void fill_rect(int x0, int y0, int x1, int y1, std::uint32_t argb);
    void blend_px(int x, int y, std::uint32_t src);
    void log(const char* tag, const std::string& msg);
};

}  // namespace browser

#endif  // BROWSER_APP_HPP
