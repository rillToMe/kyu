// browser_css_layout_test — host tests for CSS cascade + layout + hit test.
// Run: make test-browser-css-layout
#include <cstdio>
#include <string>

#include "../../../apps/browser/engine/html.cpp"
#include "../../../apps/browser/engine/css.cpp"
#include "../../../apps/browser/engine/layout.cpp"

using namespace browser;

static int failures;
static const char* cur_test;
#define CHECK(c) do { \
    if (!(c)) { printf("FAIL [%s] line %d: %s\n", cur_test, __LINE__, #c); failures++; } \
} while (0)
#define T(name) static void name(void)

// Fixed metric: char = size/2 px wide, line = size+4 px.
struct FixedMeasure : layout::TextMeasure {
    int width(const std::string& t, int size, bool bold) const override {
        int cw = size / 2;
        if (cw < 1) cw = 1;
        int w = (int)t.size() * cw + (bold ? (int)t.size() : 0);
        return w;
    }
    int line_height(int size) const override { return size + 4; }
};

struct FixedImages : layout::ImageSize {
    bool size(const std::string& src, int& w, int& h) const override {
        if (src == "big.png") {
            w = 800;
            h = 100;
            return true;
        }
        if (src == "small.png") {
            w = 40;
            h = 20;
            return true;
        }
        return false;
    }
};

static dom::Document parse(const std::string& s) {
    dom::Document d;
    html::parse(s, d);
    return d;
}

static layout::Layout lay(const dom::Document& d, int vp, css::Stylesheet* extra = nullptr) {
    std::vector<const css::Stylesheet*> sheets;
    css::Stylesheet inline_sheet;
    for (size_t i = 0; i < d.styles.size(); i++) {
        css::parse_stylesheet(d.styles[i], inline_sheet);
    }
    sheets.push_back(&inline_sheet);
    if (extra) sheets.push_back(extra);
    FixedMeasure tm;
    FixedImages im;
    layout::Layout l;
    layout::build_layout(d, sheets, vp, tm, im, l);
    return l;
}

// ---------- CSS parse ----------

T(t_css_basic) {
    cur_test = "css_basic";
    css::Stylesheet sh;
    css::parse_stylesheet("p { color: #ff0000; font-size: 20px; } .a { color: blue; } "
                          "#b { display: none; }",
                          sh);
    CHECK(sh.rules.size() == 3);
    CHECK(sh.rules[0].sel == css::Rule::Sel::Tag && sh.rules[0].key == "p");
    CHECK(sh.rules[1].sel == css::Rule::Sel::Class && sh.rules[1].key == "a");
    CHECK(sh.rules[2].sel == css::Rule::Sel::Id && sh.rules[2].key == "b");
}

T(t_css_ignored) {
    cur_test = "css_ignored";
    css::Stylesheet sh;
    css::parse_stylesheet("div p { color: red; } a:hover { color: red; } "
                          "p > b { color: red; } /* c */ h1 { color: red; } "
                          "h2 { color: red; ",
                          sh);
    CHECK(sh.rules.size() == 1 && sh.rules[0].key == "h1");  // only simple survives
}

T(t_cascade) {
    cur_test = "cascade";
    dom::Document d = parse("<p class=\"a\" id=\"b\" style=\"font-size: 24px\">x</p>");
    css::Stylesheet sh;
    css::parse_stylesheet("p { color: red; font-size: 10px; } .a { color: green; } "
                          "#b { color: blue; font-size: 12px; }",
                          sh);
    std::vector<const css::Stylesheet*> sheets;
    sheets.push_back(&sh);
    css::ComputedStyle root;
    const dom::Node* p = dom::find_first(d.root.get(), "p");
    css::ComputedStyle st = css::compute_style(p, sheets, root);
    CHECK(st.color == 0xFF0000FFu);   // id beats class beats tag
    CHECK(st.font_size == 24);        // inline beats id
}

T(t_cascade_invalid) {
    cur_test = "cascade_invalid";
    dom::Document d = parse("<p>x</p>");
    css::Stylesheet sh;
    css::parse_stylesheet("p { color: notacolor; font-size: 20px; unknown-prop: 1; "
                          "font-size: huge; margin: 1px 2px 3px 4px; }",
                          sh);
    std::vector<const css::Stylesheet*> sheets;
    sheets.push_back(&sh);
    css::ComputedStyle root;
    root.color = 0xFF111111u;
    const dom::Node* p = dom::find_first(d.root.get(), "p");
    css::ComputedStyle st = css::compute_style(p, sheets, root);
    CHECK(st.color == 0xFF111111u);  // invalid falls back to inherited
    CHECK(st.font_size == 20);       // valid applies; later invalid ignored
    CHECK(st.margin_top == 1 && st.margin_right == 2 && st.margin_bottom == 3 &&
          st.margin_left == 4);
}

T(t_colors_px) {
    cur_test = "colors_px";
    uint32_t c;
    int px;
    CHECK(css::parse_color("#f00", c) && c == 0xFFFF0000u);
    CHECK(css::parse_color("#123456", c) && c == 0xFF123456u);
    CHECK(css::parse_color("Red", c) && c == 0xFFFF0000u);
    CHECK(!css::parse_color("#ff", c) && !css::parse_color("nope", c) &&
          !css::parse_color("#gggggg", c));
    CHECK(css::parse_px("16px", px) && px == 16);
    CHECK(!css::parse_px("16", px) && !css::parse_px("16em", px) &&
          !css::parse_px("99999px", px));
    // Regresi google.com: value 1-2 char ("0","1","px") underflow
    // size()-2 jadi npos di compare (host: throw; Kyuzen: OOB -> #UD).
    CHECK(!css::parse_px("", px) && !css::parse_px("0", px) &&
          !css::parse_px("1", px) && !css::parse_px("px", px) &&
          !css::parse_px("12", px));
    CHECK(css::parse_px("0px", px) && px == 0);
    CHECK(css::parse_px("12PX", px) && px == 12);  // case-insensitive tetap
}

// ---------- layout ----------

T(t_block_stack) {
    cur_test = "block_stack";
    dom::Document d = parse("<body><h1>T</h1><p>Hi</p></body>");
    layout::Layout l = lay(d, 200);
    CHECK(l.root && l.height > 0);
    // body y=8; h1 y=8+12=20 h=32 end=52+8=60; p y=60+8=68 h=20 end=88+8=96.
    // body h=96-8=88; layout height=8+88=96 (body's own bottom margin is
    // outside the box, not part of scroll height).
    CHECK(l.height == 96);
}

T(t_wrap) {
    cur_test = "wrap";
    // 16px font: char 8px. viewport 200, body margins 8+8 => content 184 = 23 chars.
    std::string words = "aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj kkkk";
    dom::Document d = parse("<body><p>" + words + "</p></body>");
    layout::Layout l = lay(d, 200);
    const layout::Box* body = l.root.get();
    CHECK(body->lines.empty());  // text lives in the <p> kid
    CHECK(body->kids.size() == 1);
    CHECK(body->kids[0]->lines.size() > 1);  // wrapped
    // every line fits
    for (size_t i = 0; i < body->kids[0]->lines.size(); i++) {
        int w = 0;
        for (size_t r = 0; r < body->kids[0]->lines[i].runs.size(); r++)
            w += body->kids[0]->lines[i].runs[r].w;
        CHECK(w <= 184);
    }
}

T(t_links_hit) {
    cur_test = "links_hit";
    dom::Document d = parse("<body><p>go <a href=\"/next\">here</a> now</p></body>");
    layout::Layout l = lay(d, 400);
    // find the link run, click its center
    const layout::Box* p = l.root->kids[0].get();
    int cx = -1, cy = -1;
    for (size_t i = 0; i < p->lines.size(); i++)
        for (size_t r = 0; r < p->lines[i].runs.size(); r++) {
            const layout::Run& run = p->lines[i].runs[r];
            if (run.link) {
                cx = run.x + run.w / 2;
                cy = run.y + run.h / 2;
            }
        }
    CHECK(cx >= 0);
    std::string href;
    CHECK(layout::hit_link(l, cx, cy, href) && href == "/next");
    CHECK(!layout::hit_link(l, 0, 0, href));      // margin area: no link
    CHECK(!layout::hit_link(l, 399, 9999, href));  // outside: no link
}

T(t_images) {
    cur_test = "images";
    dom::Document d = parse("<body><img src=\"small.png\"><img src=\"missing.png\">"
                            "<img src=\"big.png\"></body>");
    layout::Layout l = lay(d, 200);
    // content width = 200-16 = 184. big.png 800x100 scales to 184x23.
    const layout::Box* body = l.root.get();
    CHECK(!body->lines.empty());
    int found_small = 0, found_broken = 0, found_big = 0;
    for (size_t i = 0; i < body->lines.size(); i++)
        for (size_t r = 0; r < body->lines[i].runs.size(); r++) {
            const layout::Run& run = body->lines[i].runs[r];
            if (!run.is_image) continue;
            if (run.img_src == "small.png") {
                found_small = 1;
                CHECK(run.w == 40 && run.h == 20 && !run.img_broken);
            } else if (run.img_src == "missing.png") {
                found_broken = 1;
                CHECK(run.w == 64 && run.h == 64 && run.img_broken);
            } else if (run.img_src == "big.png") {
                found_big = 1;
                CHECK(run.w == 184 && run.h == 23 && !run.img_broken);
            }
        }
    CHECK(found_small && found_broken && found_big);
}

T(t_display_none) {
    cur_test = "display_none";
    dom::Document d = parse("<body><p>one</p><div style=\"display: none\">hidden</div><p>two</p></body>");
    layout::Layout l = lay(d, 200);
    CHECK(l.root->kids.size() == 2);  // hidden div gone
}

T(t_center) {
    cur_test = "center";
    dom::Document d = parse("<body><p style=\"text-align: center\">ab</p></body>");
    layout::Layout l = lay(d, 200);
    const layout::Box* p = l.root->kids[0].get();
    CHECK(!p->lines.empty());
    // "ab" = 2 chars * 8px = 16px in 184px content => offset (184-16)/2 = 84,
    // plus content x = body x(8) + ... p has no margin-l; content starts at 8.
    int rx = p->lines[0].runs[0].x;
    CHECK(rx == 8 + 84);
}

T(t_pre) {
    cur_test = "pre";
    dom::Document d = parse("<body><pre>a  b\nc</pre></body>");
    layout::Layout l = lay(d, 400);
    const layout::Box* pre = l.root->kids[0].get();
    CHECK(pre->lines.size() == 2);  // hard break kept
    CHECK(pre->lines[0].runs[0].text == "a  b");  // spaces preserved
}

T(t_style_element_applies) {
    cur_test = "style_element_applies";
    dom::Document d = parse("<head><style>p { color: #00ff00; }</style></head>"
                            "<body><p>x</p></body>");
    layout::Layout l = lay(d, 200);
    const layout::Box* p = l.root->kids[0].get();
    CHECK(p->style.color == 0xFF00FF00u);
}

int main() {
    t_css_basic();
    t_css_ignored();
    t_cascade();
    t_cascade_invalid();
    t_colors_px();
    t_block_stack();
    t_wrap();
    t_links_hit();
    t_images();
    t_display_none();
    t_center();
    t_pre();
    t_style_element_applies();
    if (failures == 0)
        printf("browser-css-layout: ALL PASS\n");
    else
        printf("browser-css-layout: %d FAILURES\n", failures);
    return failures != 0;
}
