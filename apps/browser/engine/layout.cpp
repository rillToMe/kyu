// Browser layout engine: block stack + inline line builder.
#include "layout.hpp"

namespace browser {
namespace layout {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// Inline-context tags: everything element-like that is NOT a block break.
// display:none handled by caller. br/hr/img handled specially.
bool is_inline_tag(const css::ComputedStyle& st) {
    return st.display == css::Display::Inline;
}

struct Ctx {
    const TextMeasure& tm;
    const ImageSize& im;
    const std::vector<const css::Stylesheet*>& sheets;
    Ctx(const TextMeasure& t, const ImageSize& i,
        const std::vector<const css::Stylesheet*>& s)
        : tm(t), im(i), sheets(s) {}
};

// One inline sequence = text/images inside a block container, broken into
// lines by BR impor block child. Owns pending words until flushed.
struct Seq {
    Ctx& ctx;
    Box* box;              // container being filled
    int cx;                // content x (box.x + padding)
    int content_w;         // usable width
    int y;                 // next line top (content coords)
    Line cur;              // line under construction
    int cur_w = 0;         // used width in cur
    bool line_has_content = false;
    css::Align align = css::Align::Left;

    Seq(Ctx& c, Box* b, int content_x, int w, int y0) : ctx(c), box(b), cx(content_x), content_w(w), y(y0) {
        cur.y = y;
    }

    void finish_line() {
        if (!line_has_content) {
            // Empty line (e.g. leading BR): still advances by base height?
            // No: BR handling emits its own advance. Drop empties silently.
            cur.runs.clear();
            cur_w = 0;
            return;
        }
        // Line height = max run height.
        int lh = 0;
        for (size_t i = 0; i < cur.runs.size(); i++)
            if (cur.runs[i].h > lh) lh = cur.runs[i].h;
        if (lh <= 0) lh = ctx.tm.line_height(16);
        // Trim trailing space-only tail.
        while (!cur.runs.empty()) {
            Run& r = cur.runs.back();
            if (!r.is_image && !r.text.empty() && r.text[r.text.size() - 1] == ' ') {
                r.text.resize(r.text.size() - 1);
                r.w = ctx.tm.width(r.text, r.style.font_size, r.style.bold);
                if (r.text.empty()) {
                    cur.runs.pop_back();
                    continue;
                }
            }
            break;
        }
        cur.h = lh;
        cur.y = y;
        int ry = y;
        for (size_t i = 0; i < cur.runs.size(); i++) {
            cur.runs[i].y = ry + (lh - cur.runs[i].h);  // baseline-ish bottom align
        }
        // Horizontal alignment.
        int total = cur_w;
        // Recompute after trim.
        total = 0;
        for (size_t i = 0; i < cur.runs.size(); i++) total += cur.runs[i].w;
        int off = 0;
        if (align == css::Align::Center) off = (content_w - total) / 2;
        else if (align == css::Align::Right) off = content_w - total;
        if (off < 0) off = 0;
        int rx = cx + off;
        for (size_t i = 0; i < cur.runs.size(); i++) {
            cur.runs[i].x = rx;
            rx += cur.runs[i].w;
        }
        box->lines.push_back(cur);
        y += lh;
        cur.runs.clear();
        cur_w = 0;
        line_has_content = false;
        cur.y = y;
    }

    // Emit one word (no spaces inside) with a preceding-space flag.
    void word(const std::string& w, bool space_before, const css::ComputedStyle& st,
              const dom::Node* node, const dom::Node* link, const std::string& href) {
        if (w.empty()) return;
        int ww = ctx.tm.width(w, st.font_size, st.bold);
        int spw = space_before ? ctx.tm.width(" ", st.font_size, st.bold) : 0;
        int lh = ctx.tm.line_height(st.font_size);
        if (line_has_content && cur_w + spw + ww > content_w) {
            finish_line();
            space_before = false;  // no leading space on fresh line
            spw = 0;
        }
        // Over-long single word: still emits (overflows, documented).
        Run r;
        r.node = node;
        r.link = link;
        r.href = href;
        r.text = (space_before ? std::string(" ") : std::string()) + w;
        r.style = st;
        r.w = spw + ww;
        r.h = lh;
        cur.runs.push_back(r);
        cur_w += r.w;
        line_has_content = true;
    }

    void image_run(const dom::Node* node, const std::string& src, int iw, int ih, bool broken,
                   const css::ComputedStyle& st, const dom::Node* link, const std::string& href) {
        int lh_img = ih;
        if (line_has_content && cur_w + iw > content_w) finish_line();
        Run r;
        r.node = node;
        r.link = link;
        r.href = href;
        r.is_image = true;
        r.img_src = src;
        r.img_broken = broken;
        r.style = st;
        r.w = iw;
        r.h = lh_img;
        cur.runs.push_back(r);
        cur_w += iw;
        line_has_content = true;
    }

    void br(const css::ComputedStyle& st) {
        if (!line_has_content) {
            // Bare BR on empty line: advance one line height.
            y += ctx.tm.line_height(st.font_size);
            cur.y = y;
            return;
        }
        finish_line();
    }
};

// Nearest ancestor-or-self <a href>. Returns null when none.
const dom::Node* link_of(const dom::Node* n, std::string& href) {
    const dom::Node* p = n;
    while (p) {
        if (p->type == dom::NodeType::Element && p->tag == "a") {
            const std::string* h = p->get_attr("href");
            if (h && !h->empty()) {
                href = *h;
                return p;
            }
        }
        p = p->parent;
    }
    return nullptr;
}

int parse_int_attr(const dom::Node* n, const char* name, bool& ok) {
    ok = false;
    const std::string* v = n->get_attr(name);
    if (!v || v->empty() || v->size() > 4) return 0;
    int out = 0;
    for (size_t i = 0; i < v->size(); i++) {
        if ((*v)[i] < '0' || (*v)[i] > '9') return 0;
        out = out * 10 + ((*v)[i] - '0');
    }
    ok = true;
    return out;
}

struct Builder {
    Ctx& ctx;
    explicit Builder(Ctx& c) : ctx(c) {}

    // Lay out a block-level element. Returns the box (owned by caller).
    // x = margin-edge x, y = margin-edge y (cursor), w = available width.
    std::unique_ptr<Box> block(const dom::Node* n, const css::ComputedStyle& parent_st, int x,
                               int y, int w) {
        css::ComputedStyle st = css::compute_style(n, ctx.sheets, parent_st);
        std::unique_ptr<Box> b(new Box());
        b->node = n;
        b->style = st;
        if (n->tag == "hr") {
            b->is_hr = true;
            b->x = x + st.margin_left;
            b->w = w - st.margin_left - st.margin_right;
            if (b->w < 0) b->w = 0;
            b->y = y + st.margin_top;
            b->h = 2;
            b->y += 0;
            return b;
        }
        b->x = x + st.margin_left;
        b->w = w - st.margin_left - st.margin_right;
        if (b->w < 0) b->w = 0;
        b->y = y + st.margin_top;
        int content_x = b->x + st.padding;
        int content_w = b->w - 2 * st.padding;
        if (content_w < 0) content_w = 0;
        int cy = b->y + st.padding;

        Seq seq(ctx, b.get(), content_x, content_w, cy);
        seq.align = st.align;
        // Inline style of the container applies to its direct text.
        for (size_t i = 0; i < n->children.size(); i++)
            child(n->children[i].get(), st, seq, content_x, content_w);
        seq.finish_line();
        cy = seq.y;
        // Append block kids' heights: kids were positioned absolutely during
        // child(); cy must cover them.
        for (size_t i = 0; i < b->kids.size(); i++) {
            Box* k = b->kids[i].get();
            int kb = k->y + k->h + k->style.margin_bottom;
            if (kb > cy) cy = kb;
        }
        b->h = (cy - b->y) + st.padding;
        return b;
    }

    // One child node of a block container.
    void child(const dom::Node* n, const css::ComputedStyle& parent_st, Seq& seq, int content_x,
               int content_w) {
        if (n->type == dom::NodeType::Text) {
            emit_text(n->text, parent_st, n, seq);
            return;
        }
        css::ComputedStyle st = css::compute_style(n, ctx.sheets, parent_st);
        if (st.display == css::Display::None) return;
        const std::string& tag = n->tag;
        if (tag == "br") {
            seq.br(st);
            return;
        }
        if (tag == "hr") {
            seq.finish_line();
            std::unique_ptr<Box> hb = block(n, parent_st, seq.box->x, seq.y, seq.box->w);
            int hb_end = hb->y + hb->h + hb->style.margin_bottom;
            seq.box->kids.push_back(std::move(hb));
            seq.y = hb_end;
            seq.cur.y = seq.y;
            return;
        }
        if (tag == "img") {
            emit_image(n, st, seq);
            return;
        }
        if (is_inline_tag(st)) {
            // Inline element: recurse into children with own style.
            for (size_t i = 0; i < n->children.size(); i++)
                child(n->children[i].get(), st, seq, content_x, content_w);
            return;
        }
        // Block child: flush line, lay out, resume after it.
        seq.finish_line();
        std::unique_ptr<Box> kb = block(n, parent_st, seq.box->x, seq.y, seq.box->w);
        int kb_end = kb->y + kb->h + kb->style.margin_bottom;
        seq.box->kids.push_back(std::move(kb));
        seq.y = kb_end;
        seq.cur.y = seq.y;
        // Alignment for following runs comes from THIS container (seq.align
        // fixed at construction). Nested block align handled in its own seq.
    }

    void emit_text(const std::string& text, const css::ComputedStyle& st, const dom::Node* node,
                   Seq& seq) {
        std::string href;
        const dom::Node* link = link_of(node, href);
        if (st.pre) {
            // Preserve: split on '\n' only, keep spaces verbatim.
            size_t i = 0;
            while (i <= text.size()) {
                size_t e = text.find('\n', i);
                if (e == std::string::npos) e = text.size();
                std::string part = text.substr(i, e - i);
                if (!part.empty()) {
                    // No wrapping inside pre except at width overflow?
                    // Simple: emit whole (may overflow, documented).
                    seq.word(part, false, st, node, link, href);
                }
                if (e < text.size()) seq.br(st);  // '\n' = hard break
                i = e + 1;
            }
            return;
        }
        // Collapse whitespace, split words.
        size_t i = 0;
        bool pending_space = false;
        // Leading whitespace = space before first word (dropped at line start
        // by word()).
        while (i < text.size()) {
            while (i < text.size() && is_space(text[i])) {
                pending_space = true;
                i++;
            }
            if (i >= text.size()) break;
            size_t j = i;
            while (j < text.size() && !is_space(text[j])) j++;
            seq.word(text.substr(i, j - i), pending_space, st, node, link, href);
            pending_space = true;
            i = j;
        }
    }

    void emit_image(const dom::Node* n, const css::ComputedStyle& st, Seq& seq) {
        const std::string* src = n->get_attr("src");
        std::string s = src ? *src : "";
        std::string href;
        const dom::Node* link = link_of(n, href);
        int iw = 0, ih = 0;
        bool have = !s.empty() && ctx.im.size(s, iw, ih);
        bool b = false;
        // width=/height= attrs override when both valid.
        bool wok, hok;
        int aw = parse_int_attr(n, "width", wok);
        int ah = parse_int_attr(n, "height", hok);
        if (wok && hok && aw > 0 && ah > 0 && aw <= 4096 && ah <= 4096) {
            iw = aw;
            ih = ah;
        }
        if (!have) {
            iw = 64;
            ih = 64;
            b = true;
        } else {
            // Scale DOWN to content width, keep ratio (never upscale).
            int maxw = seq.content_w;
            if (maxw > 0 && iw > maxw) {
                ih = ih * maxw / iw;
                if (ih < 1) ih = 1;
                iw = maxw;
            }
            if (ih > 4096) {
                iw = iw * 4096 / ih;
                ih = 4096;
            }
        }
        seq.image_run(n, s, iw, ih, b, st, link, href);
    }
};

bool hit_box(const Box* b, int x, int y, std::string& href) {
    // Kids later in order paint later: search back-to-front for deepest.
    for (size_t i = b->kids.size(); i-- > 0;) {
        if (hit_box(b->kids[i].get(), x, y, href)) return true;
    }
    for (size_t li = 0; li < b->lines.size(); li++) {
        const Line& ln = b->lines[li];
        for (size_t ri = 0; ri < ln.runs.size(); ri++) {
            const Run& r = ln.runs[ri];
            if (!r.link) continue;
            if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
                href = r.href;
                return true;
            }
        }
    }
    return false;
}

}  // namespace

void build_layout(const dom::Document& doc,
                  const std::vector<const css::Stylesheet*>& sheets, int viewport_width,
                  const TextMeasure& tm, const ImageSize& im, Layout& out) {
    if (viewport_width < 64) viewport_width = 64;
    if (viewport_width > 4096) viewport_width = 4096;
    Ctx ctx(tm, im, sheets);
    Builder bd(ctx);
    const dom::Node* container = dom::find_first(doc.root.get(), "body");
    if (!container) container = doc.root.get();
    css::ComputedStyle root_st;  // UA root defaults (white page, dark text)
    root_st.color = 0xFF222222u;
    root_st.font_size = 16;
    root_st.display = css::Display::Block;
    out.root = bd.block(container, root_st, 0, 0, viewport_width);
    out.width = viewport_width;
    // Height = bottom of last kid/line.
    Box* r = out.root.get();
    int bottom = r->y + r->h;
    out.height = bottom;
    if (out.height < 0) out.height = 0;
}

bool hit_link(const Layout& l, int x, int y, std::string& href_out) {
    if (!l.root) return false;
    return hit_box(l.root.get(), x, y, href_out);
}

}  // namespace layout
}  // namespace browser
