// libs/widget/src/xml/xml_inflate.cpp — validasi skema + inflasi native (Phase E).
//
// Aturan main: validasi DULU (nama/atribut/anak/penempatan), widget dibangun
// DETACHED, window disentuh hanya saat commit. Gagal di tengah -> widget
// yang sudah dibuat di-delete berjenjang (dtor Layout/Tab/ScrollView/
// ComboBox/ListView cascade; grup radio di ctx dihapus rollback).
// Inflater hanya memanggil API/behavior native — tanpa geometri/tema sendiri.
#include "runtime/platform.hpp"
#include "runtime/memory.hpp"
#include "xml/xml.hpp"
#include "core/widget.hpp"
#include "core/painter.hpp"
#include "primitives/label.hpp"
#include "primitives/button.hpp"
#include "primitives/textbox.hpp"
#include "primitives/checkbox.hpp"
#include "primitives/radio.hpp"
#include "primitives/slider.hpp"
#include "primitives/progressbar.hpp"
#include "primitives/combobox.hpp"
#include "primitives/separator.hpp"
#include "primitives/image.hpp"
#include "layout/layout.hpp"
#include "layout/vbox.hpp"
#include "layout/hbox.hpp"
#include "containers/listview.hpp"
#include "containers/tab.hpp"
#include "containers/scrollview.hpp"
#include "containers/grid.hpp"
#include "window/window.hpp"

namespace ui {
namespace xml {

// --- util string ---
static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static int seq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}
static void scopy(char* d, const char* s, int cap) {
    int i = 0;
    while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

// --- lookup atribut ---
static const char* getattr(Node* nd, const char* name) {
    for (int i = 0; i < nd->nattr; i++)
        if (seq(nd->attrs[i].name, name)) return nd->attrs[i].value;
    return 0;
}

// --- parse nilai ---
// int desimal ketat ^-?[0-9]+$, |v| <= 1e9. 1 = ok.
static int parse_intval(const char* s, int* out) {
    if (!s || !s[0]) return 0;
    int i = 0, neg = 0;
    if (s[0] == '-') { neg = 1; i = 1; if (!s[1]) return 0; }
    long v = 0;
    for (; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
        if (v > 1000000000L) return 0;
    }
    if (neg) v = -v;
    *out = (int)v;
    return 1;
}
// bool ketat: true/false/1/0.
static int parse_boolval(const char* s, int* out) {
    if (seq(s, "true") || seq(s, "1")) { *out = 1; return 1; }
    if (seq(s, "false") || seq(s, "0")) { *out = 0; return 1; }
    return 0;
}
// spacing: int 0..64 atau token xs/sm/md/lg/xl (= UI_SPACE_* Phase D).
static int parse_spacing(const char* s, int* out) {
    if (seq(s, "xs")) { *out = UI_SPACE_XS; return 1; }
    if (seq(s, "sm")) { *out = UI_SPACE_SM; return 1; }
    if (seq(s, "md")) { *out = UI_SPACE_MD; return 1; }
    if (seq(s, "lg")) { *out = UI_SPACE_LG; return 1; }
    if (seq(s, "xl")) { *out = UI_SPACE_XL; return 1; }
    int v = 0;
    if (!parse_intval(s, &v) || v < 0 || v > 64) return 0;
    *out = v;
    return 1;
}
static int parse_align(const char* s, int* out) {
    if (seq(s, "start")) { *out = UI_ALIGN_START; return 1; }
    if (seq(s, "center")) { *out = UI_ALIGN_CENTER; return 1; }
    if (seq(s, "end")) { *out = UI_ALIGN_END; return 1; }
    if (seq(s, "stretch")) { *out = UI_ALIGN_STRETCH; return 1; }
    return 0;
}
static int parse_variant(const char* s, int* out) {
    if (seq(s, "secondary")) { *out = UI_BUTTON_SECONDARY; return 1; }
    if (seq(s, "primary")) { *out = UI_BUTTON_PRIMARY; return 1; }
    if (seq(s, "danger")) { *out = UI_BUTTON_DANGER; return 1; }
    return 0;
}
static int parse_orient(const char* s, int* out) {
    if (seq(s, "horizontal")) { *out = UI_SEP_HORIZONTAL; return 1; }
    if (seq(s, "vertical")) { *out = UI_SEP_VERTICAL; return 1; }
    return 0;
}
static int parse_mode(const char* s, int* out) {
    if (seq(s, "dark")) { *out = UI_THEME_DARK; return 1; }
    if (seq(s, "light")) { *out = UI_THEME_LIGHT; return 1; }
    return 0;
}
static int parse_accent(const char* s, int* out) {
    if (seq(s, "neutral")) { *out = UI_ACCENT_NEUTRAL; return 1; }
    if (seq(s, "blue")) { *out = UI_ACCENT_BLUE; return 1; }
    if (seq(s, "purple")) { *out = UI_ACCENT_PURPLE; return 1; }
    if (seq(s, "green")) { *out = UI_ACCENT_GREEN; return 1; }
    if (seq(s, "orange")) { *out = UI_ACCENT_ORANGE; return 1; }
    if (seq(s, "red")) { *out = UI_ACCENT_RED; return 1; }
    if (seq(s, "custom")) { *out = UI_ACCENT_CUSTOM; return 1; }
    return 0;
}
// warna ("0x"/"0X" + tepat 6 hex) -> 0xRRGGBB. 1 = ok.
static int parse_hexcolor(const char* s, unsigned* out) {
    if (!s || (s[0] != '0' || (s[1] != 'x' && s[1] != 'X'))) return 0;
    unsigned v = 0;
    for (int i = 2; i < 8; i++) {
        char ch = s[i];
        if (!ch) return 0;
        v <<= 4;
        if (ch >= '0' && ch <= '9') v |= (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= (unsigned)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v |= (unsigned)(ch - 'A' + 10);
        else return 0;
    }
    if (s[8]) return 0;
    *out = v;
    return 1;
}

// --- konteks ---
Widget* find(Context* ctx, const char* id) {
    if (!ctx || !id) return 0;
    for (int i = 0; i < ctx->nids; i++)
        if (seq(ctx->ids[i].id, id)) return ctx->ids[i].w;
    return 0;
}

int add_id(Context* ctx, const char* id, Widget* w, Error* err) {
    if (find(ctx, id)) {
        set_error(err, DUP_ID, 0, 0, 0, 0);
        // element diisi pemanggil (tahu node-nya)
        return 0;
    }
    if (ctx->nids >= MAX_IDS) {
        set_error(err, LIMIT, 0, 0, 0, id);
        return 0;
    }
    scopy(ctx->ids[ctx->nids].id, id, (int)sizeof(ctx->ids[0].id));
    ctx->ids[ctx->nids].w = w;
    ctx->ids[ctx->nids].kind = 0;   // diisi pemanggil
    ctx->nids++;
    return 1;
}

RadioGroup* group(Context* ctx, const char* name, Error* err) {
    for (int i = 0; i < ctx->ngroups; i++)
        if (seq(ctx->groups[i].name, name)) return ctx->groups[i].g;
    if (ctx->ngroups >= MAX_GROUPS || slen(name) > 31) {
        set_error(err, LIMIT, 0, 0, 0, 0);
        return 0;
    }
    RadioGroup* g = new RadioGroup;
    if (!g) { set_error(err, OOM, 0, 0, 0, 0); return 0; }
    scopy(ctx->groups[ctx->ngroups].name, name, 32);
    ctx->groups[ctx->ngroups].g = g;
    ctx->ngroups++;
    return g;
}

// Buang roots detached (berjenjang) + grup. Aman dua arah: dtor radio
// keluar grup; dtor grup melepas anggota (yang ikut ter-delete di roots).
void rollback(Context* ctx) {
    if (!ctx) return;
    for (int i = 0; i < ctx->nroots; i++) {
        delete ctx->roots[i];
        ctx->roots[i] = 0;
    }
    ctx->nroots = 0;
    for (int i = 0; i < ctx->ngroups; i++) {
        delete ctx->groups[i].g;
        ctx->groups[i].g = 0;
    }
    ctx->ngroups = 0;
    ctx->nids = 0;
    ctx->has_theme = false;
}

void commit(Context* ctx) {
    if (ctx->has_theme) {
        ui_theme_config_t cfg;
        cfg.mode = (ui_theme_mode_t)ctx->theme_mode;
        cfg.accent = (ui_theme_accent_t)ctx->theme_accent;
        cfg.custom = color_hex((uint32_t)ctx->theme_custom);
        ctx->win->set_config(&cfg);
    }
    for (int i = 0; i < ctx->nroots; i++) ctx->win->add(ctx->roots[i]);
    ctx->committed = true;
}

// --- atribut generik ---
// id/enabled/visible/tooltip + width/height opsional (allow_wh).
// Atribut penempatan grid (row/col/rowspan/colspan) DIIZINKAN di semua widget
// dan diabaikan kecuali sebagai anak <grid> (milik penempatan grid, bukan
// widget — lihat make_grid). Return 1 = ok; tak dikenal -> UNKNOWN_ATTRIBUTE.
static int apply_generic(Context* ctx, Node* nd, Widget* w, int kind,
                         int allow_w, int allow_h, Error* err) {
    // Hanya memproses atribut generik; atribut spesifik-kind DILEWATI
    // (whitelist + penolakan unknown = tugas reject_extra per kind).
    const char* id = getattr(nd, "id");
    if (id) {
        if (slen(id) == 0 || slen(id) > MAX_ID) {
            set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "id");
            return 0;
        }
        if (!add_id(ctx, id, w, err)) {
            scopy(err->element, nd->name, (int)sizeof(err->element));
            scopy(err->attribute, "id", (int)sizeof(err->attribute));
            return 0;
        }
        ctx->ids[ctx->nids - 1].kind = kind;
    }
    const char* en = getattr(nd, "enabled");
    if (en) {
        int v = 0;
        if (!parse_boolval(en, &v)) {
            set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "enabled");
            return 0;
        }
        w->set_enabled(v != 0);
    }
    const char* vi = getattr(nd, "visible");
    if (vi) {
        int v = 0;
        if (!parse_boolval(vi, &v)) {
            set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "visible");
            return 0;
        }
        w->set_visible(v != 0);
    }
    const char* tt = getattr(nd, "tooltip");
    if (tt) w->set_tooltip(tt);
    // width/height eksplisit = pilihan caller (set_size). Kontainer VBox/HBox
    // menimpa di arrange() (perilaku native, terkunci test Phase D).
    // allow_* = 0 berarti SUDAH dikonsumsi+divalidasi pemanggil (need_size) —
    // lewati diam-diam (bukan error ganda).
    const char* sw = getattr(nd, "width");
    if (sw && allow_w) {
        int v = 0;
        if (!allow_w || !parse_intval(sw, &v) || v < 0 || v > 4096) {
            set_error(err, allow_w ? INVALID_VALUE : UNKNOWN_ATTRIBUTE,
                      nd->line, nd->column, nd->name, "width");
            return 0;
        }
        w->mark_dirty();
        w->w = v;
        w->mark_dirty();
    }
    const char* sh = getattr(nd, "height");
    if (sh && allow_h) {
        int v = 0;
        if (!allow_h || !parse_intval(sh, &v) || v < 0 || v > 4096) {
            set_error(err, allow_h ? INVALID_VALUE : UNKNOWN_ATTRIBUTE,
                      nd->line, nd->column, nd->name, "height");
            return 0;
        }
        w->mark_dirty();
        w->h = v;
        w->mark_dirty();
    }
    return 1;
}

struct Built { Widget* w; int kind; };
static Built fail_built() { Built b; b.w = 0; b.kind = 0; return b; }
static Built make_built(Widget* w, int kind) { Built b; b.w = w; b.kind = kind; return b; }

static Built inflate_widget(Context* ctx, Node* nd, Error* err);

// Whitelist atribut: generik + penempatan grid + spesifik-kind.
// Tak dikenal -> UNKNOWN_ATTRIBUTE (typo tak pernah lolos diam-diam).
static int reject_extra(Node* nd, const char* const* keep, Error* err) {
    static const char* gen[] = { "id", "enabled", "visible", "tooltip",
                                 "width", "height",
                                 "row", "col", "rowspan", "colspan", 0 };
    for (int i = 0; i < nd->nattr; i++) {
        const char* an = nd->attrs[i].name;
        int ok = 0;
        for (int g = 0; gen[g]; g++)
            if (seq(an, gen[g])) { ok = 1; break; }
        if (!ok && keep)
            for (int k = 0; keep[k]; k++)
                if (seq(an, keep[k])) { ok = 1; break; }
        if (!ok) {
            set_error(err, UNKNOWN_ATTRIBUTE, nd->line, nd->column, nd->name, an);
            return 0;
        }
    }
    return 1;
}

// Anak widget generik untuk kontainer list (vbox/hbox/root): attach via add.
static int inflate_list_children(Context* ctx, Node* nd, Layout* lay, Error* err) {
    for (int i = 0; i < nd->nchild; i++) {
        Built c = inflate_widget(ctx, nd->children[i], err);
        if (!c.w) return 0;
        if (lay->count >= Layout::MAX_CHILDREN) {
            delete c.w;
            set_error(err, LIMIT, nd->children[i]->line, nd->children[i]->column,
                      nd->children[i]->name, 0);
            return 0;
        }
        lay->add(c.w);
    }
    return 1;
}

// --- daun ---
static Built make_label(Context* ctx, Node* nd, Error* err) {
    const char* t = getattr(nd, "text");
    Label* w = new Label(t ? t : "");
    if (!w) { set_error(err, OOM, nd->line, nd->column, nd->name, 0); return fail_built(); }
    if (!apply_generic(ctx, nd, w, K_LABEL, 1, 1, err)) { delete w; return fail_built(); }
    static const char* keep[] = { "text", 0 };
    if (!reject_extra(nd, keep, err)) { delete w; return fail_built(); }
    if (nd->nchild > 0) {
        set_error(err, BAD_CHILD, nd->children[0]->line, nd->children[0]->column,
                  nd->children[0]->name, 0);
        delete w;
        return fail_built();
    }
    return make_built(w, K_LABEL);
}

static Built make_button(Context* ctx, Node* nd, Error* err) {
    const char* t = getattr(nd, "text");
    Button* w = new Button(t ? t : "");
    if (!w) { set_error(err, OOM, nd->line, nd->column, nd->name, 0); return fail_built(); }
    const char* v = getattr(nd, "variant");
    if (v) {
        int vv = 0;
        if (!parse_variant(v, &vv)) {
            set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "variant");
            delete w;
            return fail_built();
        }
        w->set_variant(vv);
    }
    if (!apply_generic(ctx, nd, w, K_BUTTON, 1, 1, err)) { delete w; return fail_built(); }
    static const char* bkeep[] = { "text", "variant", 0 };
    if (!reject_extra(nd, bkeep, err)) { delete w; return fail_built(); }
    if (nd->nchild > 0) {
        set_error(err, BAD_CHILD, nd->children[0]->line, nd->children[0]->column,
                  nd->children[0]->name, 0);
        delete w;
        return fail_built();
    }
    return make_built(w, K_BUTTON);
}

// Daun: tak boleh punya anak elemen.
static int no_children(Node* nd, Error* err) {
    if (nd->nchild > 0) {
        set_error(err, BAD_CHILD, nd->children[0]->line, nd->children[0]->column,
                  nd->children[0]->name, 0);
        return 0;
    }
    return 1;
}

// width/height wajib ctor (>0, <=4096).
static int need_size(Node* nd, const char* a_name, int* out, Error* err) {
    const char* s = getattr(nd, a_name);
    if (!s) {
        set_error(err, MISSING_ATTRIBUTE, nd->line, nd->column, nd->name, a_name);
        return 0;
    }
    int v = 0;
    if (!parse_intval(s, &v) || v <= 0 || v > 4096) {
        set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, a_name);
        return 0;
    }
    *out = v;
    return 1;
}

static Built make_textbox(Context* ctx, Node* nd, Error* err) {
    static const char* keep[] = { "text", "error", 0 };
    int w = 0;
    if (!need_size(nd, "width", &w, err)) return fail_built();
    TextBox* t = new TextBox(w);
    if (!t) { set_error(err, OOM, nd->line, nd->column, nd->name, 0); return fail_built(); }
    const char* tx = getattr(nd, "text");
    if (tx) t->set_text(tx);
    const char* er = getattr(nd, "error");
    if (er) {
        int v = 0;
        if (!parse_boolval(er, &v)) {
            set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "error");
            delete t;
            return fail_built();
        }
        t->set_error(v != 0);
    }
    if (!reject_extra(nd, keep, err)) { delete t; return fail_built(); }
    if (!apply_generic(ctx, nd, t, K_TEXTBOX, 0, 1, err)) { delete t; return fail_built(); }
    if (!no_children(nd, err)) { delete t; return fail_built(); }
    return make_built(t, K_TEXTBOX);
}

static Built make_checkbox(Context* ctx, Node* nd, Error* err) {
    static const char* keep[] = { "text", "checked", 0 };
    const char* t = getattr(nd, "text");
    CheckBox* c = new CheckBox(t ? t : "");
    if (!c) { set_error(err, OOM, nd->line, nd->column, nd->name, 0); return fail_built(); }
    const char* ch = getattr(nd, "checked");
    if (ch) {
        int v = 0;
        if (!parse_boolval(ch, &v)) {
            set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "checked");
            delete c;
            return fail_built();
        }
        c->set_checked(v != 0);
    }
    if (!reject_extra(nd, keep, err)) { delete c; return fail_built(); }
    if (!apply_generic(ctx, nd, c, K_CHECKBOX, 1, 1, err)) { delete c; return fail_built(); }
    if (!no_children(nd, err)) { delete c; return fail_built(); }
    return make_built(c, K_CHECKBOX);
}

static Built make_radio(Context* ctx, Node* nd, Error* err) {
    static const char* keep[] = { "text", "selected", "group", 0 };
    const char* t = getattr(nd, "text");
    Radio* r = new Radio(t ? t : "");
    if (!r) { set_error(err, OOM, nd->line, nd->column, nd->name, 0); return fail_built(); }
    const char* g = getattr(nd, "group");
    if (g) {
        if (slen(g) == 0 || slen(g) > 31) {
            set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "group");
            delete r;
            return fail_built();
        }
        RadioGroup* grp = group(ctx, g, err);
        if (!grp) {
            scopy(err->element, nd->name, (int)sizeof(err->element));
            scopy(err->attribute, "group", (int)sizeof(err->attribute));
            delete r;
            return fail_built();
        }
        if (grp->n >= RadioGroup::MAX_RADIOS) {
            set_error(err, LIMIT, nd->line, nd->column, nd->name, "group");
            delete r;
            return fail_built();
        }
        r->set_group(grp);
    }
    const char* se = getattr(nd, "selected");
    if (se) {
        int v = 0;
        if (!parse_boolval(se, &v)) {
            set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "selected");
            delete r;
            return fail_built();
        }
        if (v) r->set_selected(true);   // diam (konvensi programatik)
    }
    if (!reject_extra(nd, keep, err)) { delete r; return fail_built(); }
    if (!apply_generic(ctx, nd, r, K_RADIO, 1, 1, err)) { delete r; return fail_built(); }
    if (!no_children(nd, err)) { delete r; return fail_built(); }
    return make_built(r, K_RADIO);
}

static Built make_slider(Context* ctx, Node* nd, Error* err) {
    static const char* keep[] = { "min", "max", "value", 0 };
    int mn = 0, mx = 100;
    const char* smin = getattr(nd, "min");
    const char* smax = getattr(nd, "max");
    if (smin && (!parse_intval(smin, &mn))) {
        set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "min");
        return fail_built();
    }
    if (smax && (!parse_intval(smax, &mx))) {
        set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "max");
        return fail_built();
    }
    // max<=min = perilaku native (max = min+1); teruskan apa adanya.
    Slider* s = new Slider(mn, mx);
    if (!s) { set_error(err, OOM, nd->line, nd->column, nd->name, 0); return fail_built(); }
    const char* sv = getattr(nd, "value");
    if (sv) {
        int v = 0;
        if (!parse_intval(sv, &v)) {
            set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "value");
            delete s;
            return fail_built();
        }
        s->set_value(v);   // clamp native
    }
    if (!reject_extra(nd, keep, err)) { delete s; return fail_built(); }
    if (!apply_generic(ctx, nd, s, K_SLIDER, 1, 1, err)) { delete s; return fail_built(); }
    if (!no_children(nd, err)) { delete s; return fail_built(); }
    return make_built(s, K_SLIDER);
}

static Built make_progress(Context* ctx, Node* nd, Error* err) {
    static const char* keep[] = { "value", 0 };
    int w = 0;
    if (!need_size(nd, "width", &w, err)) return fail_built();
    ProgressBar* p = new ProgressBar(w);
    if (!p) { set_error(err, OOM, nd->line, nd->column, nd->name, 0); return fail_built(); }
    const char* sv = getattr(nd, "value");
    if (sv) {
        int v = 0;
        if (!parse_intval(sv, &v)) {
            set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "value");
            delete p;
            return fail_built();
        }
        p->set_value(v);   // clamp 0..100 native
    }
    if (!reject_extra(nd, keep, err)) { delete p; return fail_built(); }
    if (!apply_generic(ctx, nd, p, K_PROGRESS, 0, 1, err)) { delete p; return fail_built(); }
    if (!no_children(nd, err)) { delete p; return fail_built(); }
    return make_built(p, K_PROGRESS);
}

static Built make_separator(Context* ctx, Node* nd, Error* err) {
    static const char* keep[] = { "orientation", 0 };
    int o = UI_SEP_HORIZONTAL;
    const char* s = getattr(nd, "orientation");
    if (s && !parse_orient(s, &o)) {
        set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "orientation");
        return fail_built();
    }
    Separator* w = new Separator(o != UI_SEP_HORIZONTAL);
    if (!w) { set_error(err, OOM, nd->line, nd->column, nd->name, 0); return fail_built(); }
    if (!reject_extra(nd, keep, err)) { delete w; return fail_built(); }
    if (!apply_generic(ctx, nd, w, K_SEPARATOR, 1, 1, err)) { delete w; return fail_built(); }
    if (!no_children(nd, err)) { delete w; return fail_built(); }
    return make_built(w, K_SEPARATOR);
}

static Built make_image(Context* ctx, Node* nd, Error* err) {
    static const char* keep[] = { "src", 0 };
    const char* src = getattr(nd, "src");
    if (!src || !src[0]) {
        set_error(err, MISSING_ATTRIBUTE, nd->line, nd->column, nd->name, "src");
        return fail_built();
    }
    int w = 0, h = 0;
    if (!need_size(nd, "width", &w, err)) return fail_built();
    if (!need_size(nd, "height", &h, err)) return fail_built();
    // Kebijakan resource (§31): src diteruskan ke native apa adanya (pemuat
    // KyuzenFS milik Image, bukan parser — tanpa akses file di lapisan XML).
    Image* im = new Image(src, w, h);
    if (!im) { set_error(err, OOM, nd->line, nd->column, nd->name, 0); return fail_built(); }
    if (!reject_extra(nd, keep, err)) { delete im; return fail_built(); }
    if (!apply_generic(ctx, nd, im, K_IMAGE, 0, 0, err)) { delete im; return fail_built(); }
    if (!no_children(nd, err)) { delete im; return fail_built(); }
    return make_built(im, K_IMAGE);
}

static Built make_combo(Context* ctx, Node* nd, Error* err) {
    static const char* keep[] = { "selected", 0 };
    int w = 0;
    if (!need_size(nd, "width", &w, err)) return fail_built();
    ComboBox* c = new ComboBox;
    if (!c) { set_error(err, OOM, nd->line, nd->column, nd->name, 0); return fail_built(); }
    c->w = w;
    for (int i = 0; i < nd->nchild; i++) {
        Node* ch = nd->children[i];
        if (!seq(ch->name, "item")) {
            set_error(err, BAD_CHILD, ch->line, ch->column, ch->name, 0);
            delete c;
            return fail_built();
        }
        const char* t = getattr(ch, "text");
        if (!t) {
            set_error(err, MISSING_ATTRIBUTE, ch->line, ch->column, ch->name, "text");
            delete c;
            return fail_built();
        }
        // item = helper struktural: hanya text (tanpa id/generik).
        for (int a = 0; a < ch->nattr; a++) {
            if (seq(ch->attrs[a].name, "text")) continue;
            set_error(err, UNKNOWN_ATTRIBUTE, ch->line, ch->column, ch->name,
                      ch->attrs[a].name);
            delete c;
            return fail_built();
        }
        if (ch->nchild > 0) {
            set_error(err, BAD_CHILD, ch->children[0]->line, ch->children[0]->column,
                      ch->children[0]->name, 0);
            delete c;
            return fail_built();
        }
        if (c->n >= ComboBox::MAX_ITEMS) {
            set_error(err, LIMIT, ch->line, ch->column, ch->name, 0);
            delete c;
            return fail_built();
        }
        if (c->add_item(t) < 0) {
            set_error(err, OOM, ch->line, ch->column, ch->name, 0);
            delete c;
            return fail_built();
        }
    }
    const char* se = getattr(nd, "selected");
    if (se) {
        int v = 0;
        if (!parse_intval(se, &v) || v < -1 || v >= c->n) {
            set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "selected");
            delete c;
            return fail_built();
        }
        c->set_selected(v);   // diam
    }
    if (!reject_extra(nd, keep, err)) { delete c; return fail_built(); }
    if (!apply_generic(ctx, nd, c, K_COMBO, 0, 1, err)) { delete c; return fail_built(); }
    return make_built(c, K_COMBO);
}

static Built make_listview(Context* ctx, Node* nd, Error* err) {
    static const char* keep[] = { "selected", 0 };
    int w = 0, h = 0;
    if (!need_size(nd, "width", &w, err)) return fail_built();
    if (!need_size(nd, "height", &h, err)) return fail_built();
    ListView* l = new ListView(w, h);
    if (!l) { set_error(err, OOM, nd->line, nd->column, nd->name, 0); return fail_built(); }
    for (int i = 0; i < nd->nchild; i++) {
        Node* ch = nd->children[i];
        if (!seq(ch->name, "item")) {
            set_error(err, BAD_CHILD, ch->line, ch->column, ch->name, 0);
            delete l;
            return fail_built();
        }
        const char* t = getattr(ch, "text");
        if (!t) {
            set_error(err, MISSING_ATTRIBUTE, ch->line, ch->column, ch->name, "text");
            delete l;
            return fail_built();
        }
        for (int a = 0; a < ch->nattr; a++) {
            if (seq(ch->attrs[a].name, "text")) continue;
            set_error(err, UNKNOWN_ATTRIBUTE, ch->line, ch->column, ch->name,
                      ch->attrs[a].name);
            delete l;
            return fail_built();
        }
        if (ch->nchild > 0) {
            set_error(err, BAD_CHILD, ch->children[0]->line, ch->children[0]->column,
                      ch->children[0]->name, 0);
            delete l;
            return fail_built();
        }
        if (l->n >= ListView::MAX_ITEMS) {
            set_error(err, LIMIT, ch->line, ch->column, ch->name, 0);
            delete l;
            return fail_built();
        }
        l->add_item(t);
    }
    const char* se = getattr(nd, "selected");
    if (se) {
        int v = 0;
        if (!parse_intval(se, &v) || v < -1 || v >= l->n) {
            // -1 = tak ada (default native); di luar itu = error (tanpa efek
            // diam-diam seperti native void — XML harus eksplisit).
            if (!(v == -1)) {
                set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "selected");
                delete l;
                return fail_built();
            }
        } else if (v >= 0) {
            l->set_selected(v);
        }
    }
    if (!reject_extra(nd, keep, err)) { delete l; return fail_built(); }
    if (!apply_generic(ctx, nd, l, K_LISTVIEW, 0, 0, err)) { delete l; return fail_built(); }
    return make_built(l, K_LISTVIEW);
}

// --- kontainer ---
static Built make_vbox(Context* ctx, Node* nd, Error* err, int is_hbox) {
    static const char* keep[] = { "spacing", 0 };
    int sp = 8;
    const char* s = getattr(nd, "spacing");
    if (s && !parse_spacing(s, &sp)) {
        set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "spacing");
        return fail_built();
    }
    Layout* lay = is_hbox ? (Layout*)new HBox(sp) : (Layout*)new VBox(sp);
    if (!lay) { set_error(err, OOM, nd->line, nd->column, nd->name, 0); return fail_built(); }
    if (!reject_extra(nd, keep, err)) { delete lay; return fail_built(); }
    if (!apply_generic(ctx, nd, lay, is_hbox ? K_HBOX : K_VBOX, 1, 1, err)) {
        delete lay;
        return fail_built();
    }
    if (!inflate_list_children(ctx, nd, lay, err)) { delete lay; return fail_built(); }
    return make_built(lay, is_hbox ? K_HBOX : K_VBOX);
}

static Built make_grid(Context* ctx, Node* nd, Error* err) {
    static const char* keep[] = { "rows", "cols", "gap", "padding",
                                  "padding-left", "padding-top",
                                  "padding-right", "padding-bottom",
                                  "align", 0 };
    int rows = 0, cols = 0;
    const char* sr = getattr(nd, "rows");
    const char* sc = getattr(nd, "cols");
    if (!sr) { set_error(err, MISSING_ATTRIBUTE, nd->line, nd->column, nd->name, "rows"); return fail_built(); }
    if (!sc) { set_error(err, MISSING_ATTRIBUTE, nd->line, nd->column, nd->name, "cols"); return fail_built(); }
    if (!parse_intval(sr, &rows) || rows < 1) {
        set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "rows");
        return fail_built();
    }
    if (!parse_intval(sc, &cols) || cols < 1) {
        set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "cols");
        return fail_built();
    }
    // >8 = clamp native (terdokumentasi); tolak hanya yang absurd.
    if (rows > 64) {
        set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "rows");
        return fail_built();
    }
    if (cols > 64) {
        set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "cols");
        return fail_built();
    }
    int gap = 8;
    const char* sg = getattr(nd, "gap");
    if (sg && !parse_spacing(sg, &gap)) {
        set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "gap");
        return fail_built();
    }
    Grid* g = new Grid(rows, cols, gap);
    if (!g) { set_error(err, OOM, nd->line, nd->column, nd->name, 0); return fail_built(); }
    // padding: uniform ATAU per-sisi (campur = error).
    const char* pu = getattr(nd, "padding");
    const char* pl = getattr(nd, "padding-left");
    const char* pt = getattr(nd, "padding-top");
    const char* pr = getattr(nd, "padding-right");
    const char* pb = getattr(nd, "padding-bottom");
    if (pu && (pl || pt || pr || pb)) {
        set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "padding");
        delete g;
        return fail_built();
    }
    int il = 0, it = 0, ir = 0, ib = 0;
    if (pu) {
        int v = 0;
        if (!parse_intval(pu, &v) || v < 0 || v > 4096) {
            set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "padding");
            delete g;
            return fail_built();
        }
        il = it = ir = ib = v;
    } else {
        const char* ps[4] = { pl, pt, pr, pb };
        const char* pn[4] = { "padding-left", "padding-top", "padding-right", "padding-bottom" };
        int* pv[4] = { &il, &it, &ir, &ib };
        for (int k = 0; k < 4; k++) {
            if (!ps[k]) continue;
            int v = 0;
            if (!parse_intval(ps[k], &v) || v < 0 || v > 4096) {
                set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, pn[k]);
                delete g;
                return fail_built();
            }
            *pv[k] = v;
        }
    }
    g->set_padding(il, it, ir, ib);
    const char* sa = getattr(nd, "align");
    if (sa) {
        int av = 0;
        if (!parse_align(sa, &av)) {
            set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "align");
            delete g;
            return fail_built();
        }
        g->set_align(av);
    }
    if (!reject_extra(nd, keep, err)) { delete g; return fail_built(); }
    if (!apply_generic(ctx, nd, g, K_GRID, 1, 1, err)) { delete g; return fail_built(); }
    // anak: row+col wajib; penempatan via native put/put_span.
    for (int i = 0; i < nd->nchild; i++) {
        Node* ch = nd->children[i];
        Built c = inflate_widget(ctx, ch, err);
        if (!c.w) { delete g; return fail_built(); }
        const char* srw = getattr(ch, "row");
        const char* scl = getattr(ch, "col");
        int r = 0, cc = 0, rs = 1, cs = 1;
        if (!srw || !scl ||
            !parse_intval(srw, &r) || !parse_intval(scl, &cc)) {
            set_error(err, BAD_PLACEMENT, ch->line, ch->column, ch->name,
                      !srw ? "row" : "col");
            delete c.w;
            delete g;
            return fail_built();
        }
        const char* srs = getattr(ch, "rowspan");
        const char* scs = getattr(ch, "colspan");
        if (srs && !parse_intval(srs, &rs)) {
            set_error(err, INVALID_VALUE, ch->line, ch->column, ch->name, "rowspan");
            delete c.w;
            delete g;
            return fail_built();
        }
        if (scs && !parse_intval(scs, &cs)) {
            set_error(err, INVALID_VALUE, ch->line, ch->column, ch->name, "colspan");
            delete c.w;
            delete g;
            return fail_built();
        }
        // row/col/rowspan/colspan diizinkan di semua widget (whitelist generik)
        // dan dibaca di sini; put() native yang memutuskan (0 = tolak).
        if (!g->put(c.w, r, cc, rs, cs)) {
            set_error(err, BAD_PLACEMENT, ch->line, ch->column, ch->name, 0);
            delete c.w;
            delete g;
            return fail_built();
        }
    }
    // Auto-size: grid native TAK menentukan ukuran sendiri (caller-sized —
    // lihat Phase D), jadi VBox induk akan menumpuknya sebagai h=0 dan
    // anak grid overlap saudara. Inflater menutup celah ini TANPA menyentuh
    // native: settle bottom-up (kontainer bersarang hitung ukuran sendiri)
    // lalu ukur track. Eksplisit width/height selalu menang (termasuk untuk
    // track FILL yang butuh ruang caller).
    if (!getattr(nd, "width") || !getattr(nd, "height")) {
        for (int k = 0; k < g->ncell; k++)
            if (g->cells[k].w) g->cells[k].w->settle();
        int cw[8], ch[8];
        g->measure_tracks(cw, ch);
        int gw = g->pad_l + g->pad_r, gh = g->pad_t + g->pad_b;
        for (int i = 0; i < g->cols; i++) gw += cw[i];
        for (int i = 0; i < g->rows; i++) gh += ch[i];
        gw += g->gap * (g->cols - 1);
        gh += g->gap * (g->rows - 1);
        if (!getattr(nd, "width")) {
            g->mark_dirty();
            g->w = gw < 0 ? 0 : gw;
            g->mark_dirty();
        }
        if (!getattr(nd, "height")) {
            g->mark_dirty();
            g->h = gh < 0 ? 0 : gh;
            g->mark_dirty();
        }
    }
    return make_built(g, K_GRID);
}

static Built make_tab(Context* ctx, Node* nd, Error* err) {
    static const char* keep[] = { 0 };
    (void)keep;
    int w = 0, h = 0;
    if (!need_size(nd, "width", &w, err)) return fail_built();
    if (!need_size(nd, "height", &h, err)) return fail_built();
    Tab* t = new Tab(w, h);
    if (!t) { set_error(err, OOM, nd->line, nd->column, nd->name, 0); return fail_built(); }
    if (!reject_extra(nd, 0, err)) { delete t; return fail_built(); }
    if (!apply_generic(ctx, nd, t, K_TAB, 0, 0, err)) { delete t; return fail_built(); }
    for (int i = 0; i < nd->nchild; i++) {
        Node* ch = nd->children[i];
        if (!seq(ch->name, "page")) {
            set_error(err, BAD_CHILD, ch->line, ch->column, ch->name, 0);
            delete t;
            return fail_built();
        }
        const char* title = getattr(ch, "title");
        if (!title) {
            set_error(err, MISSING_ATTRIBUTE, ch->line, ch->column, ch->name, "title");
            delete t;
            return fail_built();
        }
        for (int a = 0; a < ch->nattr; a++) {
            const char* an = ch->attrs[a].name;
            if (seq(an, "title") || seq(an, "enabled")) continue;
            set_error(err, UNKNOWN_ATTRIBUTE, ch->line, ch->column, ch->name, an);
            delete t;
            return fail_built();
        }
        if (ch->nchild != 1) {
            // Panel tepat satu (Tab::add butuh satu panel; 0/>1 ambigu).
            if (ch->nchild == 0)
                set_error(err, BAD_CHILD, ch->line, ch->column, ch->name, 0);
            else
                set_error(err, BAD_CHILD, ch->children[1]->line, ch->children[1]->column,
                          ch->children[1]->name, 0);
            delete t;
            return fail_built();
        }
        if (t->n >= Tab::MAX_TABS) {
            set_error(err, LIMIT, ch->line, ch->column, ch->name, 0);
            delete t;
            return fail_built();
        }
        Built panel = inflate_widget(ctx, ch->children[0], err);
        if (!panel.w) { delete t; return fail_built(); }
        t->add(title, panel.w);
        const char* en = getattr(ch, "enabled");
        if (en) {
            int v = 0;
            if (!parse_boolval(en, &v)) {
                set_error(err, INVALID_VALUE, ch->line, ch->column, ch->name, "enabled");
                delete t;
                return fail_built();
            }
            t->set_tab_enabled(t->n - 1, v);
        }
    }
    return make_built(t, K_TAB);
}

static Built make_scrollview(Context* ctx, Node* nd, Error* err) {
    static const char* keep[] = { "pan", 0 };
    int w = 0, h = 0;
    if (!need_size(nd, "width", &w, err)) return fail_built();
    if (!need_size(nd, "height", &h, err)) return fail_built();
    ScrollView* s = new ScrollView(w, h);
    if (!s) { set_error(err, OOM, nd->line, nd->column, nd->name, 0); return fail_built(); }
    const char* pan = getattr(nd, "pan");
    if (pan) {
        int v = 0;
        if (!parse_boolval(pan, &v)) {
            set_error(err, INVALID_VALUE, nd->line, nd->column, nd->name, "pan");
            delete s;
            return fail_built();
        }
        s->set_pan(v);
    }
    if (!reject_extra(nd, keep, err)) { delete s; return fail_built(); }
    if (!apply_generic(ctx, nd, s, K_SCROLLVIEW, 0, 0, err)) { delete s; return fail_built(); }
    if (nd->nchild > 1) {
        set_error(err, BAD_CHILD, nd->children[1]->line, nd->children[1]->column,
                  nd->children[1]->name, 0);
        delete s;
        return fail_built();
    }
    if (nd->nchild == 1) {
        Built c = inflate_widget(ctx, nd->children[0], err);
        if (!c.w) { delete s; return fail_built(); }
        s->set_child(c.w);
    }
    return make_built(s, K_SCROLLVIEW);
}

// --- dispatcher ---
static Built inflate_widget(Context* ctx, Node* nd, Error* err) {
    const char* n = nd->name;
    if (seq(n, "label")) return make_label(ctx, nd, err);
    if (seq(n, "button")) return make_button(ctx, nd, err);
    if (seq(n, "textbox")) return make_textbox(ctx, nd, err);
    if (seq(n, "checkbox")) return make_checkbox(ctx, nd, err);
    if (seq(n, "radio")) return make_radio(ctx, nd, err);
    if (seq(n, "combobox")) return make_combo(ctx, nd, err);
    if (seq(n, "slider")) return make_slider(ctx, nd, err);
    if (seq(n, "progressbar")) return make_progress(ctx, nd, err);
    if (seq(n, "separator")) return make_separator(ctx, nd, err);
    if (seq(n, "image")) return make_image(ctx, nd, err);
    if (seq(n, "listview")) return make_listview(ctx, nd, err);
    if (seq(n, "vbox")) return make_vbox(ctx, nd, err, 0);
    if (seq(n, "hbox")) return make_vbox(ctx, nd, err, 1);
    if (seq(n, "grid")) return make_grid(ctx, nd, err);
    if (seq(n, "tab")) return make_tab(ctx, nd, err);
    if (seq(n, "scrollview")) return make_scrollview(ctx, nd, err);
    // item/page/window di posisi anak = BAD_CHILD (struktural, bukan widget).
    if (seq(n, "item") || seq(n, "page") || seq(n, "window"))
        set_error(err, BAD_CHILD, nd->line, nd->column, nd->name, 0);
    else
        set_error(err, UNKNOWN_ELEMENT, nd->line, nd->column, nd->name, 0);
    return fail_built();
}

// --- root: <window> ---
int inflate(Context* ctx, const Document* doc, Error* err) {
    if (!ctx || !ctx->win || !doc || !doc->root) {
        set_error(err, EMPTY, 0, 0, 0, 0);
        return 0;
    }
    if (ctx->committed || ctx->nroots > 0 || ctx->nids > 0 || ctx->ngroups > 0) {
        set_error(err, LIMIT, 0, 0, 0, 0);   // satu inflasi per konteks
        return 0;
    }
    Node* root = doc->root;
    if (!seq(root->name, "window")) {
        set_error(err, UNKNOWN_ELEMENT, root->line, root->column, root->name, 0);
        return 0;
    }
    // Atribut window: HANYA tema. id/enabled/size/tooltip = UNKNOWN_ATTRIBUTE.
    static const char* wkeep[] = { "theme-mode", "theme-accent", "theme-custom", 0 };
    for (int i = 0; i < root->nattr; i++) {
        const char* an = root->attrs[i].name;
        int ok = 0;
        for (int k = 0; wkeep[k]; k++)
            if (seq(an, wkeep[k])) { ok = 1; break; }
        if (!ok) {
            set_error(err, UNKNOWN_ATTRIBUTE, root->line, root->column, root->name, an);
            return 0;
        }
    }
    const char* sm = getattr(root, "theme-mode");
    const char* sa = getattr(root, "theme-accent");
    const char* sc = getattr(root, "theme-custom");
    if (sm || sa || sc) {
        int mode = UI_THEME_DARK, accent = UI_ACCENT_NEUTRAL;
        unsigned custom = 0;
        if (sm && !parse_mode(sm, &mode)) {
            set_error(err, INVALID_VALUE, root->line, root->column, root->name, "theme-mode");
            return 0;
        }
        if (sa && !parse_accent(sa, &accent)) {
            set_error(err, INVALID_VALUE, root->line, root->column, root->name, "theme-accent");
            return 0;
        }
        // theme-mode wajib bila ada atribut tema lain (tanpa default diam-diam
        // untuk mode; accent default neutral = default native).
        if (!sm) {
            set_error(err, MISSING_ATTRIBUTE, root->line, root->column, root->name, "theme-mode");
            return 0;
        }
        if (sc) {
            if (!parse_hexcolor(sc, &custom)) {
                set_error(err, INVALID_VALUE, root->line, root->column, root->name, "theme-custom");
                return 0;
            }
        }
        if (accent == UI_ACCENT_CUSTOM && !sc) {
            set_error(err, MISSING_ATTRIBUTE, root->line, root->column, root->name, "theme-custom");
            return 0;
        }
        ctx->has_theme = true;
        ctx->theme_mode = mode;
        ctx->theme_accent = accent;
        ctx->theme_custom = custom;
    }
    for (int i = 0; i < root->nchild; i++) {
        if (ctx->nroots >= MAX_ROOTS) {
            set_error(err, LIMIT, root->children[i]->line, root->children[i]->column,
                      root->children[i]->name, 0);
            rollback(ctx);
            return 0;
        }
        Built c = inflate_widget(ctx, root->children[i], err);
        if (!c.w) { rollback(ctx); return 0; }
        // Lacak SEGERA: saudara yang sudah sukses ikut terbuang saat
        // anak berikutnya gagal (tanpa ini = leak di jalur gagal).
        ctx->roots[ctx->nroots++] = c.w;
    }
    return 1;
}

} // namespace xml
} // namespace ui
