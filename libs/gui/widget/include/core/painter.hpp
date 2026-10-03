// libs/widget/include/core/painter.hpp — satu-satunya jembatan widget → renderer.
//
// Tanggung jawab:
//   1. clipping (scissor widget + render clip + bounds window) — satu jalur,
//      semua primitif lewat `clip_rect()`.
//   2. primitif geometri (rect, rrect, border, bayangan) dengan anti-alias.
//   3. TEKS: jalur bitmap 8×16 dengan dukungan peran tipografi (tracking +
//      huruf besar) dan pemotongan ellipsis.
//   4. blit gambar ARGB.
//
// Painter TIDAK menyimpan state desain: warna/ukuran selalu datang dari
// `theme` (palet + token) supaya pergantian tema berlaku tanpa mengubah widget.
#ifndef KWIDGET_CORE_PAINTER_HPP
#define KWIDGET_CORE_PAINTER_HPP

#include "runtime/platform.hpp"
#include "core/theme.hpp"
#include "core/text_provider.hpp"
#include "theme/icons.hpp"
#include "theme/trig.hpp"

namespace ui {

class Painter {
public:
    gui_window_t* win;
    const Theme& theme;
    // Scissor rect widget-level — set_clip/clear_clip dipakai widget.
    bool clip_on;
    int clip_x, clip_y, clip_w, clip_h;
    // Render/dirty clip — dipasang Window::render, TIDAK disentuh widget.
    bool rclip_on;
    int rclip_x, rclip_y, rclip_w, rclip_h;
    // Permukaan di belakang teks. Dipakai jalur text provider untuk mengomposit
    // glyph secara IDEMPOTEN (lihat text()). Widget menyetelnya saat menggambar
    // latar (surface()); default = bg tema, yang benar untuk widget yang
    // menggambar di atas halaman tanpa latar sendiri.
    color_t text_bg;
    Painter(gui_window_t* w, const Theme& t)
        : win(w), theme(t), clip_on(false), clip_x(0), clip_y(0),
          clip_w(0), clip_h(0), rclip_on(false), rclip_x(0), rclip_y(0),
          rclip_w(0), rclip_h(0), text_bg(t.bg) {}
    // Set permukaan di belakang teks. WAJIB dipanggil widget yang menggambar
    // latar sendiri sebelum menggambar teks di atasnya; kalau tidak, teks
    // dikomposit di atas `bg` tema dan tepi glyph akan salah di atas latar
    // yang berbeda (mis. baris terpilih).
    void set_text_bg(color_t c) { text_bg = color_opaque(c); }
    void set_render_clip(int x, int y, int w, int h) {
        rclip_on = true; rclip_x = x; rclip_y = y; rclip_w = w; rclip_h = h;
    }
    void set_clip(int x, int y, int w, int h) {
        clip_on = true; clip_x = x; clip_y = y; clip_w = w; clip_h = h;
    }
    void clear_clip() { clip_on = false; }
    // Potong rect ke scissor widget + render clip + bounds window.
    // Return false bila kosong. Semua primitif lewat sini (satu jalur clipping).
    bool clip_rect(int& x, int& y, int& w, int& h) {
        int x1 = x + w, y1 = y + h;
        if (clip_on) {
            if (x < clip_x) x = clip_x;
            if (y < clip_y) y = clip_y;
            int cx = clip_x + clip_w, cy = clip_y + clip_h;
            if (x1 > cx) x1 = cx;
            if (y1 > cy) y1 = cy;
        }
        if (rclip_on) {
            if (x < rclip_x) x = rclip_x;
            if (y < rclip_y) y = rclip_y;
            int rx = rclip_x + rclip_w, ry = rclip_y + rclip_h;
            if (x1 > rx) x1 = rx;
            if (y1 > ry) y1 = ry;
        }
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x1 > (int)win->width)  x1 = (int)win->width;
        if (y1 > (int)win->height) y1 = (int)win->height;
        if (x1 <= x || y1 <= y) return false;
        w = x1 - x; h = y1 - y;
        return true;
    }
    void rect(int x, int y, int w, int h, color_t c) {
        if (!clip_rect(x, y, w, h)) return;
        // libgui memaksa alpha opaque saat menulis canvas (mask transparansi
        // window urusan compositor), jadi `c` dikirim apa adanya.
        gui_draw_rect(win, x, y, w, h, c);
    }
    // Rect yang juga MENYATAKAN permukaan di belakang teks berikutnya. Dipakai
    // widget yang menggambar latar sendiri (baris daftar terpilih, header,
    // track): tanpa ini teks di atasnya akan dikomposit di atas bg tema.
    void surface_rect(int x, int y, int w, int h, color_t c) {
        set_text_bg(c);
        rect(x, y, w, h, c);
    }
    // ------------------------------------------------------------
    // Pengukuran teks — SATU jalur untuk bitmap 8×16 DAN font nyata.
    // Widget memakai text_measure()/text_vcenter() dari core/text_provider.hpp;
    // di sini hanya alias supaya pemanggil lama (p.text_line_h()) tetap jalan.
    // ------------------------------------------------------------
    static int text_line_h() { return text_line_height(); }
    static int text_ascent_px() { return text_ascent(); }

    void text(const char* s, int x, int y, color_t c) {
        if (!s || !s[0]) return;
        // Provider: gambar di baseline. `y` adalah ATAS baris, jadi baseline =
        // y + ascent. Itu yang membuat teks provider sejajar dengan teks
        // bitmap pada koordinat yang sama.
        const TextProvider* tp = text_provider_get();
        if (tp) {
            int n = 0;
            while (s[n]) n++;
            // Komposit DI ATAS text_bg, bukan di atas isi canvas: hasilnya
            // idempoten, jadi menggambar ulang area yang sama (ScrollView
            // menggambar anaknya dua kali per frame, hover melukis ulang
            // sebagian baris) tidak menggelapkan tepi glyph.
            tp->draw(tp->ud, win->canvas, (int)win->width, (int)win->height,
                     x, y + text_ascent(), c, text_bg, s);
            // Provider menulis canvas langsung, jadi damage dicatat di sini
            // (kontrak provider: tidak memanggil gui_damage_rect sendiri).
            gui_damage_rect(win, x, y, tp->measure(tp->ud, s, n) + 2,
                            text_line_height());
            return;
        }
        // Tanpa clip: jalur cepat (libgui menandai damage ter-clip sendiri).
        if (!clip_on && !rclip_on) { gui_draw_text(win, s, x, y, c); return; }
        // Ter-clip: gambar per-sel 8x16; hanya sel yang beririsan dengan clip.
        // cx/cy dijejak terpisah (bukan x + i*8): setelah '\n' kolom HARUS
        // kembali ke kiri, kalau tidak baris kedua dan seterusnya melebar ke
        // kanan — inilah yang dulu merusak teks multi-baris di dialog.
        int cx = x, cy = y;
        for (int i = 0; s[i] && i < 512; i++) {
            if (s[i] == '\n') { cy += 16; cx = x; continue; }
            int rx = cx, ry = cy, rw = 8, rh = 16;
            if (clip_rect(rx, ry, rw, rh))
                gui_draw_char(win, s[i], cx, cy, c);
            cx += 8;
        }
    }

    // ------------------------------------------------------------
    // Teks dengan PERAN TIPOGRAFI (theme/typography.hpp).
    // Satu tempat yang tahu bahwa jalur bitmap punya advance 8px + tracking,
    // sehingga tidak ada widget yang menghitung `len * 8` sendiri lagi.
    // ------------------------------------------------------------
    void text_role(const char* s, int x, int y, color_t c, const TypeRole& r) {
        if (!s || !s[0]) return;
        // Dengan provider: ukuran huruf datang dari FONT (peran hanya memilih
        // tone); tracking dan uppercase adalah trik bitmap untuk membangun
        // hierarki tanpa ukuran — keduanya tidak perlu dan justru merusak
        // tipografi nyata. Jadi jalur provider menggambar apa adanya.
        if (text_provider_active()) { text(s, x, y, c); return; }
        const int adv = glyph::ADVANCE + r.bitmap_tracking;
        if (adv == glyph::ADVANCE && !r.bitmap_upper) {
            text(s, x, y, c);          // jalur cepat: peran default
            return;
        }
        // Tracking: gambar per karakter. Huruf besar diterapkan di sini
        // (bukan oleh pemanggil) supaya peran `section` konsisten di semua
        // widget tanpa setiap app menulis uppercase sendiri.
        int cx = x, cy = y;
        for (int i = 0; s[i] && i < 512; i++) {
            char ch = s[i];
            if (ch == '\n') { cy += r.bitmap_line_h; cx = x; continue; }
            if (r.bitmap_upper && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
            if (!clip_on && !rclip_on) {
                gui_draw_char(win, ch, cx, cy, c);
            } else {
                int rx = cx, ry = cy, rw = 8, rh = 16;
                if (clip_rect(rx, ry, rw, rh)) gui_draw_char(win, ch, cx, cy, c);
            }
            cx += adv;
        }
    }

    // Teks satu baris yang DIPOTONG agar tidak melewati `max_w`, diakhiri
    // ellipsis "…" versi ASCII ("...") bila terpotong. Dipakai semua widget
    // yang menampilkan teks milik pengguna (label baris, judul tab, nama file)
    // supaya tidak ada lagi teks yang bocor keluar bounds.
    void text_ellipsis(const char* s, int x, int y, int max_w, color_t c,
                       const TypeRole& r) {
        if (!s || !s[0] || max_w <= 0) return;
        int n = 0;
        while (s[n]) n++;
        // Berapa karakter yang muat? Dengan provider, lebar satu karakter TIDAK
        // konstan, jadi dihitung dari pengukuran nyata (bukan max_w/advance).
        if (text_provider_active()) {
            if (text_measure(s) <= max_w) { text(s, x, y, c); return; }
            const int dot = text_measure("...");
            int keep = n;
            while (keep > 0) {
                // Cari prefiks terpanjang yang muat bersama "...".
                char buf[128];
                int k = 0;
                for (; k < keep && k < 127 && s[k]; k++) buf[k] = s[k];
                buf[k] = '\0';
                if (text_measure(buf) + dot <= max_w) break;
                keep--;
            }
            if (keep <= 0) {
                // Terlalu sempit untuk ellipsis: potong seadanya.
                char buf[8];
                int k = 0;
                for (; k < 7 && s[k]; k++) {
                    buf[k] = s[k];
                    buf[k + 1] = '\0';
                    if (text_measure(buf) > max_w) { buf[k] = '\0'; break; }
                }
                text(buf, x, y, c);
                return;
            }
            char buf[131];
            int k = 0;
            for (; k < keep && k < 127 && s[k]; k++) buf[k] = s[k];
            buf[k++] = '.'; buf[k++] = '.'; buf[k++] = '.';
            buf[k] = '\0';
            text(buf, x, y, c);
            return;
        }
        const int adv = glyph::ADVANCE + r.bitmap_tracking;
        int fit = max_w / adv;
        if (n <= fit) { text_role(s, x, y, c, r); return; }
        if (fit < 4) {                 // terlalu sempit untuk "..." → potong saja
            char buf[8];
            int k = 0;
            for (; k < fit && k < 7; k++) buf[k] = s[k];
            buf[k] = '\0';
            text_role(buf, x, y, c, r);
            return;
        }
        char buf[128];
        int k = 0;
        int keep = fit - 3;            // 3 sel untuk "..."
        for (; k < keep && k < 127 && s[k]; k++) buf[k] = s[k];
        buf[k++] = '.'; buf[k++] = '.'; buf[k++] = '.';
        buf[k] = '\0';
        text_role(buf, x, y, c, r);
    }

    // Blit PNG ARGB8888 (px = iw×ih) diskalakan nearest-neighbor ke rect
    // (x,y,w,h). Alpha per-pixel di-blend ke canvas (a=0 dilewati agar latar
    // terlihat, semi blend integer, hasil opaque). Menulis win->canvas
    // langsung (libgui tak punya draw-image).
    void image(int x, int y, int w, int h, const uint32_t* px, int iw, int ih) {
        if (w <= 0 || h <= 0 || iw <= 0 || ih <= 0 || !px) return;
        int dx = x, dy = y, dw = w, dh = h;
        if (!clip_rect(dx, dy, dw, dh)) return;
        int q0 = dx - x, py0 = dy - y, q1 = q0 + dw, py1 = py0 + dh;
        int cw = (int)win->width;
        for (int py = py0; py < py1; py++) {
            int sy = py * ih / h;
            for (int q = q0; q < q1; q++) {
                int sx = q * iw / w;
                uint32_t s = px[sy * iw + sx];
                uint32_t a = s >> 24;
                if (a == 0) continue;
                uint32_t* d = &win->canvas[(y + py) * cw + x + q];
                if (a == 255) { *d = s | 0xFF000000u; continue; }
                uint32_t sr = (s >> 16) & 0xFFu, sg = (s >> 8) & 0xFFu,
                         sb = s & 0xFFu;
                uint32_t dr = (*d >> 16) & 0xFFu, dg = (*d >> 8) & 0xFFu,
                         db = *d & 0xFFu;
                uint32_t na = 255u - a;
                uint32_t r = (sr * a + dr * na) / 255u;
                uint32_t g = (sg * a + dg * na) / 255u;
                uint32_t b = (sb * a + db * na) / 255u;
                *d = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
        gui_damage_rect(win, dx, dy, dw, dh);
    }

    // ----- Primitif "modern" (blend / rounded / shadow) -----
    // Compositor kernel memakai byte alpha canvas sebagai MASK opaque
    // (0 = tembus), bukan faktor blend → blending harus dilakukan di sini:
    // baca pixel canvas, campur, tulis kembali.
    void blend(int px, int py, color_t c, uint32_t a) {
        if (a == 0) return;
        int x = px, y = py, w = 1, h = 1;
        if (!clip_rect(x, y, w, h)) return;
        uint32_t* d = &win->canvas[y * (int)win->width + x];
        color_t src = color_with_alpha(c, (uint8_t)a);
        color_t dst = color_opaque(color_from_u32(*d, FORMAT_ARGB));
        *d = color_to_u32(color_blend_alpha(src, dst), FORMAT_ARGB);
        gui_damage_rect(win, x, y, 1, 1);
    }
    void blend_rect(int x, int y, int w, int h, color_t c, uint32_t a) {
        // Clip dulu di tingkat rect: menghindari 1 damage-rect per pixel untuk
        // area yang sebagian besar terpotong (jalur bayangan overlay).
        int cx = x, cy = y, cw = w, ch = h;
        if (!clip_rect(cx, cy, cw, ch)) return;
        for (int iy = cy; iy < cy + ch; iy++)
            for (int ix = cx; ix < cx + cw; ix++) {
                uint32_t* d = &win->canvas[iy * (int)win->width + ix];
                color_t src = color_with_alpha(c, (uint8_t)a);
                color_t dst = color_opaque(color_from_u32(*d, FORMAT_ARGB));
                *d = color_to_u32(color_blend_alpha(src, dst), FORMAT_ARGB);
            }
        gui_damage_rect(win, cx, cy, cw, ch);
    }
    // Gradient vertikal (lerp integer per baris).
    void vgrad(int x, int y, int w, int h, color_t top, color_t bot) {
        for (int iy = 0; iy < h; iy++)
            rect(x, y + iy, w, 1, color_blend_alpha(
                color_with_alpha(bot, (uint8_t)(iy * 255 / (h > 1 ? h - 1 : 1))), top));
    }
    // Coverage 0..255 pixel (px,py) di dalam rounded-rect (aa_math.h:
    // supersample 4x4 integer — pengganti Wu yang butuh float).
    static uint32_t rr_cov(int px, int py, int x, int y, int w, int h, int r) {
        return aa_cov(px, py, x, y, w, h, r, r);
    }
    // Rounded rect + gradient vertikal, sudut anti-alias.
    void rrect_grad(int x, int y, int w, int h, int r, color_t top, color_t bot) {
        if (w <= 0 || h <= 0) return;
        if (r > w / 2) r = w / 2;
        if (r > h / 2) r = h / 2;
        const bool flat = color_to_u32(top, FORMAT_ARGB) == color_to_u32(bot, FORMAT_ARGB);
        for (int iy = y; iy < y + h; iy++) {
            color_t c = flat ? top : color_blend_alpha(
                color_with_alpha(bot, (uint8_t)((iy - y) * 255 / (h > 1 ? h - 1 : 1))), top);
            if (iy >= y + r && iy < y + h - r) { rect(x, iy, w, 1, c); continue; }
            rect(x + r, iy, w - 2 * r, 1, c);
            for (int k = 0; k < r; k++) {
                blend(x + k, iy, c, rr_cov(x + k, iy, x, y, w, h, r));
                blend(x + w - 1 - k, iy, c, rr_cov(x + w - 1 - k, iy, x, y, w, h, r));
            }
        }
    }
    void rrect(int x, int y, int w, int h, int r, color_t c) {
        rrect_grad(x, y, w, h, r, c, c);
    }
    // Border 1px halus mengikuti sudut bulat (alpha, bukan garis keras).
    //
    // Implementasi: untuk SETIAP baris, cari pixel paling kiri dan paling
    // kanan yang tersentuh bentuk, lalu blend pixel itu dengan coverage-nya.
    // Cara ini benar untuk semua kasus, termasuk LINGKARAN (r == h/2) —
    // versi lama memakai jalur cepat "baris tengah" yang rentangnya KOSONG
    // saat r == h/2, sehingga pixel tepi kiri/kanan lingkaran (coverage penuh,
    // jadi dilewati loop arc) tidak pernah tergambar: handle slider, cincin
    // radio, dan track switch tampak "terbuka" di kedua sisinya.
    void rrect_border(int x, int y, int w, int h, int r, color_t c, uint32_t a) {
        if (w <= 0 || h <= 0 || a == 0) return;
        if (r > w / 2) r = w / 2;
        if (r > h / 2) r = h / 2;
        // Pencarian tepi dibatasi `r` pixel dari tiap sisi: di luar sudut,
        // pixel paling kiri/kanan sudah pasti tergambar penuh (coverage 255)
        // sehingga tidak perlu dipindai.
        for (int iy = y; iy < y + h; iy++) {
            for (int k = 0; k <= r; k++) {
                uint32_t cl = rr_cov(x + k, iy, x, y, w, h, r);
                if (cl) { blend(x + k, iy, c, a * cl / 255); break; }
            }
            for (int k = w - 1; k >= w - 1 - r; k--) {
                uint32_t cr = rr_cov(x + k, iy, x, y, w, h, r);
                if (cr) { blend(x + k, iy, c, a * cr / 255); break; }
            }
        }
        // Tepi horizontal atas/bawah pada bagian lurus (di luar sudut).
        if (w - 2 * r > 0) {
            blend_rect(x + r, y, w - 2 * r, 1, c, a);
            blend_rect(x + r, y + h - 1, w - 2 * r, 1, c, a);
        }
    }

    // ------------------------------------------------------------
    // Permukaan + border standar (satu jalur untuk semua widget).
    // `level` = Elevation (theme/elevation.hpp) menentukan radius default.
    // ------------------------------------------------------------
    void surface(int x, int y, int w, int h, color_t c, int r) {
        if (r <= 0) { rect(x, y, w, h, c); return; }
        rrect(x, y, w, h, r, c);
    }
    // Outline 1px untuk permukaan BESAR (kotak, track, well). Jauh lebih murah
    // dari rrect_border penuh karena hanya menggambar 4 sisi + sudut.
    void outline(int x, int y, int w, int h, int r, color_t c) {
        if (w <= 0 || h <= 0) return;
        if (r > 0) { rrect_border(x, y, w, h, r, c, 255); return; }
        rect(x, y, w, 1, c);
        rect(x, y + h - 1, w, 1, c);
        rect(x, y, 1, h, c);
        rect(x + w - 1, y, 1, h, c);
    }

    // ------------------------------------------------------------
    // Geometri lingkaran (jam analog, progres melingkar).
    //
    // Coverage dihitung ANALITIS dari jarak, bukan supersample 4×4 seperti
    // aa_cov: tutup jam berdiameter ~200 px berarti ~31 ribu piksel, dan 16
    // sampel per piksel akan menjadi 500 ribu operasi per frame — terlalu
    // mahal untuk komponen yang digambar ulang tiap detik. Ramp linear selebar
    // 1 px di sekitar tepi memberi kualitas yang sama terlihat pada radius
    // besar dengan biaya konstan per piksel.
    //
    // Semua koordinat memakai 1/8 piksel (subpiksel 3 bit) supaya tepi tidak
    // "bergerak" saat pusat lingkaran jatuh di antara dua piksel.
    // ------------------------------------------------------------

    // Coverage 0..255 untuk jarak `d8` (1/8 px) terhadap tepi `edge8`.
    // Transisi linear selebar 1 px yang terpusat di tepi.
    static uint32_t cov_edge(int64_t d8, int64_t edge8) {
        int64_t c = (edge8 + 4 - d8) * 255 / 8;
        if (c <= 0) return 0;
        if (c >= 255) return 255;
        return (uint32_t)c;
    }

    // Cakram penuh berpusat (cx,cy) radius r.
    void circle_fill(int cx, int cy, int r, color_t c) {
        if (r <= 0) return;
        int x0 = cx - r - 1, y0 = cy - r - 1;
        int x1 = cx + r + 1, y1 = cy + r + 1;
        int cw = x1 - x0, ch = y1 - y0;
        if (!clip_rect(x0, y0, cw, ch)) return;
        const int64_t rr = (int64_t)r * 8;
        for (int py = y0; py < y1; py++) {
            int64_t dy = (int64_t)(py - cy) * 8 + 4;   // pusat piksel
            for (int px = x0; px < x1; px++) {
                int64_t dx = (int64_t)(px - cx) * 8 + 4;
                // Bandingkan kuadrat jarak dalam satuan 1/64 px agar akurat.
                int64_t d2 = dx * dx + dy * dy;
                // Isqrt integer: cukup untuk radius layar (<= 4096 px).
                int64_t d = isqrt64(d2);
                uint32_t a = cov_edge(d, rr);
                if (a) blend(px, py, c, a);
            }
        }
        gui_damage_rect(win, x0, y0, x1 - x0, y1 - y0);
    }

    // Cincin (annulus): tepi luar r, tebal t ke arah dalam.
    void circle_ring(int cx, int cy, int r, int t, color_t c) {
        if (r <= 0 || t <= 0) return;
        int ri = r - t;
        if (ri < 0) ri = 0;
        int x0 = cx - r - 1, y0 = cy - r - 1;
        int x1 = cx + r + 1, y1 = cy + r + 1;
        int cw = x1 - x0, ch = y1 - y0;
        if (!clip_rect(x0, y0, cw, ch)) return;
        const int64_t ro8 = (int64_t)r * 8, ri8 = (int64_t)ri * 8;
        for (int py = y0; py < y1; py++) {
            int64_t dy = (int64_t)(py - cy) * 8 + 4;
            for (int px = x0; px < x1; px++) {
                int64_t dx = (int64_t)(px - cx) * 8 + 4;
                int64_t d = isqrt64(dx * dx + dy * dy);
                uint32_t ao = cov_edge(d, ro8);
                if (!ao) continue;
                // Tepi dalam: coverage penuh bila jarak DI ATAS ri.
                uint32_t ai = (ri8 > 0) ? (255 - cov_edge(d, ri8)) : 255;
                uint32_t a = ao * ai / 255;
                if (a) blend(px, py, c, a);
            }
        }
        gui_damage_rect(win, x0, y0, x1 - x0, y1 - y0);
    }

    // Busur tebal dari `start` sepanjang `sweep` (satuan 1/65536 putaran,
    // searah jarum jam karena sumbu Y layar mengarah ke bawah).
    //
    // Pengujian sudut memakai dua perkalian silang terhadap sinar AWAL dan
    // AKHIR, bukan atan2 per piksel: titik berada di dalam busur bila ia
    // searah-jarum-jam dari sinar awal DAN berlawanan dari sinar akhir. Itu
    // benar selama sweep <= 180 derajat, jadi sweep besar dipecah dua.
    void arc_stroke(int cx, int cy, int r, int t, int32_t start, int32_t sweep,
                    color_t c) {
        if (r <= 0 || t <= 0 || sweep <= 0) return;
        if (sweep >= ANGLE_FULL) { circle_ring(cx, cy, r, t, c); return; }
        if (sweep > ANGLE_HALF) {
            arc_stroke(cx, cy, r, t, start, ANGLE_HALF, c);
            arc_stroke(cx, cy, r, t, angle_norm(start + ANGLE_HALF),
                       sweep - ANGLE_HALF, c);
            return;
        }
        int ri = r - t;
        if (ri < 0) ri = 0;
        int x0 = cx - r - 1, y0 = cy - r - 1;
        int x1 = cx + r + 1, y1 = cy + r + 1;
        int cw = x1 - x0, ch = y1 - y0;
        if (!clip_rect(x0, y0, cw, ch)) return;
        const int64_t ro8 = (int64_t)r * 8, ri8 = (int64_t)ri * 8;
        const int32_t s0 = sin_q15(start), c0 = cos_q15(start);
        const int32_t se = sin_q15(start + sweep), ce = cos_q15(start + sweep);
        for (int py = y0; py < y1; py++) {
            int64_t dy = (int64_t)(py - cy) * 8 + 4;
            for (int px = x0; px < x1; px++) {
                int64_t dx = (int64_t)(px - cx) * 8 + 4;
                int64_t d = isqrt64(dx * dx + dy * dy);
                uint32_t ao = cov_edge(d, ro8);
                if (!ao) continue;
                uint32_t ai = (ri8 > 0) ? (255 - cov_edge(d, ri8)) : 255;
                // cross0 > 0: titik searah jarum jam dari sinar awal.
                int64_t cross0 = (int64_t)c0 * dy - (int64_t)s0 * dx;
                if (cross0 < 0) continue;
                int64_t crosse = (int64_t)ce * dy - (int64_t)se * dx;
                if (crosse > 0) continue;
                uint32_t a = ao * ai / 255;
                if (a) blend(px, py, c, a);
            }
        }
        gui_damage_rect(win, x0, y0, x1 - x0, y1 - y0);
    }

    // Garis tebal anti-alias dari (x0,y0) ke (x1,y1) — jarum jam.
    // Coverage dari jarak titik ke SEGMEN (bukan ke garis tak hingga), jadi
    // ujungnya tidak melebar keluar.
    void line_thick(int x0, int y0, int x1, int y1, int t, color_t c) {
        if (t <= 0) return;
        int half = t / 2;
        int bx0 = (x0 < x1 ? x0 : x1) - half - 1;
        int by0 = (y0 < y1 ? y0 : y1) - half - 1;
        int bx1 = (x0 > x1 ? x0 : x1) + half + 2;
        int by1 = (y0 > y1 ? y0 : y1) + half + 2;
        int bw = bx1 - bx0, bh = by1 - by0;
        if (!clip_rect(bx0, by0, bw, bh)) return;
        int64_t vx = (int64_t)(x1 - x0) * 8, vy = (int64_t)(y1 - y0) * 8;
        int64_t vlen2 = vx * vx + vy * vy;
        if (vlen2 == 0) return;
        const int64_t half8 = (int64_t)t * 8 / 2;
        for (int py = by0; py < by1; py++) {
            int64_t dy = (int64_t)(py - y0) * 8 + 4;
            for (int px = bx0; px < bx1; px++) {
                int64_t dx = (int64_t)(px - x0) * 8 + 4;
                // Proyeksi titik ke segmen, dijepit 0..1.
                int64_t proj = dx * vx + dy * vy;
                int64_t d2;
                if (proj <= 0) {
                    d2 = dx * dx + dy * dy;
                } else if (proj >= vlen2) {
                    int64_t ex = dx - vx, ey = dy - vy;
                    d2 = ex * ex + ey * ey;
                } else {
                    // Jarak tegak lurus: |cross| / |v|. Kuadratkan agar tidak
                    // perlu akar di sini.
                    int64_t cross = dx * vy - dy * vx;
                    d2 = cross * cross / vlen2;
                }
                int64_t d = isqrt64(d2);
                uint32_t a = cov_edge(d, half8);
                if (a) blend(px, py, c, a);
            }
        }
        gui_damage_rect(win, bx0, by0, bx1 - bx0, by1 - by0);
    }

    // Akar kuadrat integer (Newton). Dipakai jalur coverage lingkaran; nilai
    // di sini selalu < 2^32 (radius layar), jadi 32 iterasi sudah cukup.
    static int64_t isqrt64(int64_t v) {
        if (v <= 0) return 0;
        int64_t x = v, y = 1;
        // Perkiraan awal: setengah bit tertinggi.
        int64_t bit = 1;
        while (bit * 4 <= v) bit *= 4;
        x = bit;
        while (x > 0) {
            int64_t nx = (x + v / x) / 2;
            if (nx >= x) break;
            x = nx;
        }
        return x;
    }

    // ------------------------------------------------------------
    // Ikon (theme/icons.hpp). Digambar dari spesifikasi segmen pada grid
    // 16×16 lalu diskalakan; `size` bulat kelipatan grid agar goresan tetap
    // 1px tajam. Semua segmen digambar sebagai rect 1px (horizontal/vertikal
    // atau tangga diagonal) — tanpa alokasi, tanpa tabel statis.
    // ------------------------------------------------------------
    void icon(int id, int cx, int cy, int size, color_t c) {
        if (id <= ICON_NONE || id >= ICON_COUNT || size <= 0) return;
        IconSpec spec = icon_spec((Icon)id);
        if (spec.nseg == 0 && spec.ndot == 0) return;
        // Skala dari grid 16 → size. Pembulatan integer: ukuran non-kelipatan
        // 16 tetap proporsional (mis. 12 → x0.75).
        const int ox = cx - size / 2;
        const int oy = cy - size / 2;
        const int st = size <= 12 ? 1 : 1;   // toolkit selalu 1px (grid 8×16)
        for (int i = 0; i < spec.nseg; i++) {
            int x0 = ox + spec.segs[i].x0 * size / 16;
            int y0 = oy + spec.segs[i].y0 * size / 16;
            int x1 = ox + spec.segs[i].x1 * size / 16;
            int y1 = oy + spec.segs[i].y1 * size / 16;
            draw_icon_line(x0, y0, x1, y1, st, c);
        }
        for (int i = 0; i < spec.ndot; i++) {
            int dx = ox + spec.dots[i].x * size / 16;
            int dy = oy + spec.dots[i].y * size / 16;
            int dr = spec.dots[i].r * size / 16;
            if (dr <= 0) { rect(dx, dy, st, st, c); continue; }
            // Titik = kotak kecil (bukan lingkaran): pada ukuran ini lingkaran
            // hanya menambah biaya tanpa menambah keterbacaan.
            rect(dx - dr, dy - dr, dr * 2 + 1, dr * 2 + 1, c);
        }
    }

    // Segmen garis ikon. Hanya 3 kasus yang dipakai ikon: horizontal,
    // vertikal, dan diagonal 45° (digambar sebagai tangga 1px).
    void draw_icon_line(int x0, int y0, int x1, int y1, int st, color_t c) {
        int dx = x1 - x0, dy = y1 - y0;
        if (dy == 0) { rect(x0, y0, dx >= 0 ? dx + st : -dx + st, st, c); return; }
        if (dx == 0) { rect(x0, y0, st, dy >= 0 ? dy + st : -dy + st, c); return; }
        int n = (dx >= 0 ? dx : -dx);
        int m = (dy >= 0 ? dy : -dy);
        if (m > n) n = m;
        for (int i = 0; i <= n; i++) {
            int px = x0 + (n ? dx * i / n : 0);
            int py = y0 + (n ? dy * i / n : 0);
            rect(px, py, st, st, c);
        }
    }

    // ------------------------------------------------------------
    // Ring fokus & underline fokus (core/state.hpp memetakan state → warna;
    // Painter yang menggambar). Keduanya DI DALAM bounds widget supaya damage
    // tracking tetap tepat dan tidak ada pixel yang bocor ke luar rect.
    // ------------------------------------------------------------
    void focus_ring(int x, int y, int w, int h, int r) {
        if (w <= 0 || h <= 0) return;
        rrect_border(x, y, w, h, r, theme.focus, 255);
    }
    void focus_underline(int x, int y, int w) {
        int h = theme.metrics.focus_outline;
        if (w <= 0 || h <= 0) return;
        rect(x, y, w, h, theme.focus);
    }

    // ------------------------------------------------------------
    // Drop shadow menurut token elevasi (theme/elevation.hpp).
    // Gambar SEBELUM isi widget — cincin di dalam rect ditimpa widget.
    // Level BASE/SURFACE/RAISED menghasilkan NOL cincin (by design).
    // ------------------------------------------------------------
    void shadow(int x, int y, int w, int h, int level) {
        ShadowSpec s = elevation_shadow(level);
        if (s.rings <= 0) return;
        int alpha = s.alpha;
        for (int k = 1; k <= s.rings; k++) {
            int sx = x - k * s.spread, sy = y - k * s.spread + s.offset_y;
            int sw = w + 2 * k * s.spread, sh = h + 2 * k * s.spread;
            uint32_t a = alpha > 0 ? (uint32_t)alpha : 0;
            blend_rect(sx, sy, sw, 1, COLOR_BLACK, a);
            blend_rect(sx, sy + sh - 1, sw, 1, COLOR_BLACK, a);
            blend_rect(sx, sy + 1, 1, sh - 2, COLOR_BLACK, a);
            blend_rect(sx + sw - 1, sy + 1, 1, sh - 2, COLOR_BLACK, a);
            alpha -= s.falloff;
            if (alpha <= 0) break;
        }
    }
};

} // namespace ui

#endif // KWIDGET_CORE_PAINTER_HPP
