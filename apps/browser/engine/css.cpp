// Browser CSS subset: parser + cascade.
#include "css.hpp"

namespace browser {
namespace css {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
char lower_of(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

void trim(std::string& s) {
    size_t a = 0;
    while (a < s.size() && is_space(s[a])) a++;
    size_t b = s.size();
    while (b > a && is_space(s[b - 1])) b--;
    s = s.substr(a, b - a);
}

void lower_inplace(std::string& s) {
    for (size_t i = 0; i < s.size(); i++) s[i] = lower_of(s[i]);
}

// Strip /* ... */ comments (unterminated = drop rest).
std::string strip_comments(const std::string& src) {
    std::string out;
    size_t i = 0;
    while (i < src.size()) {
        size_t c = src.find("/*", i);
        if (c == std::string::npos) {
            out.append(src, i, src.size() - i);
            break;
        }
        out.append(src, i, c - i);
        size_t e = src.find("*/", c + 2);
        if (e == std::string::npos) break;
        i = e + 2;
    }
    return out;
}

bool valid_simple_selector(const std::string& s) {
    // tag | .class | #id : [A-Za-z][A-Za-z0-9_-]* with optional ./# prefix.
    if (s.empty()) return false;
    size_t i = (s[0] == '.' || s[0] == '#') ? 1 : 0;
    if (i >= s.size()) return false;
    char c0 = s[i];
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z'))) return false;
    for (; i < s.size(); i++) {
        char c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

void parse_decls(const std::string& body, std::vector<Declaration>& out) {
    size_t i = 0;
    while (i < body.size() && out.size() < MAX_DECLS_PER_RULE) {
        size_t semi = body.find(';', i);
        std::string one =
            (semi == std::string::npos) ? body.substr(i) : body.substr(i, semi - i);
        i = (semi == std::string::npos) ? body.size() : semi + 1;
        size_t colon = one.find(':');
        if (colon == std::string::npos) continue;  // junk declaration: skip
        Declaration d;
        d.prop = one.substr(0, colon);
        d.value = one.substr(colon + 1);
        trim(d.prop);
        trim(d.value);
        lower_inplace(d.prop);
        if (!d.prop.empty() && !d.value.empty()) out.push_back(d);
    }
}

// Property indexes for specificity tracking (fixed supported set).
int prop_index(const std::string& p) {
    if (p == "color") return 0;
    if (p == "background-color") return 1;
    if (p == "font-size") return 2;
    if (p == "font-weight") return 3;
    if (p == "font-style") return 4;
    if (p == "text-align") return 5;
    if (p == "margin") return 6;
    if (p == "margin-top") return 7;
    if (p == "margin-bottom") return 8;
    if (p == "margin-left") return 9;
    if (p == "margin-right") return 10;
    if (p == "padding") return 11;
    if (p == "display") return 12;
    return -1;
}

struct Applier {
    ComputedStyle& st;
    int best_spec[13];
    unsigned long best_order[13];
    Applier(ComputedStyle& s) : st(s) {
        for (int i = 0; i < 13; i++) {
            best_spec[i] = -1;
            best_order[i] = 0;
        }
    }
    void apply(const Declaration& d, int spec, unsigned long order) {
        int idx = prop_index(d.prop);
        if (idx < 0) return;  // unsupported property: ignore
        if (spec < best_spec[idx]) return;
        if (spec == best_spec[idx] && order < best_order[idx]) return;
        // Validate + set. Invalid values are ignored WITHOUT updating best
        // (a later valid declaration with lower precedence still applies).
        ComputedStyle tmp = st;
        if (!apply_value(tmp, d)) return;
        st = tmp;
        best_spec[idx] = spec;
        best_order[idx] = order;
    }
    static bool apply_value(ComputedStyle& st, const Declaration& d) {
        const std::string& v = d.value;
        switch (prop_index(d.prop)) {
            case 0: {
                uint32_t c;
                if (!parse_color(v, c)) return false;
                st.color = c;
                return true;
            }
            case 1: {
                uint32_t c;
                if (!parse_color(v, c)) return false;
                st.bg = c;
                st.has_bg = true;
                return true;
            }
            case 2: {
                int px;
                if (!parse_px(v, px)) return false;
                st.font_size = px;
                return true;
            }
            case 3: {
                std::string l = v;
                lower_inplace(l);
                if (l == "bold") st.bold = true;
                else if (l == "normal") st.bold = false;
                else return false;
                return true;
            }
            case 4: {
                std::string l = v;
                lower_inplace(l);
                if (l == "italic" || l == "oblique") st.italic = true;
                else if (l == "normal") st.italic = false;
                else return false;
                return true;
            }
            case 5: {
                std::string l = v;
                lower_inplace(l);
                if (l == "left") st.align = Align::Left;
                else if (l == "center") st.align = Align::Center;
                else if (l == "right") st.align = Align::Right;
                else return false;
                return true;
            }
            case 6: {  // margin: 1-4 Npx values (top right bottom left)
                int vals[4];
                if (!split_px_list(v, vals)) return false;
                st.margin_top = vals[0];
                st.margin_right = vals[1];
                st.margin_bottom = vals[2];
                st.margin_left = vals[3];
                return true;
            }
            case 7: {
                int px;
                if (!parse_px(v, px)) return false;
                st.margin_top = px;
                return true;
            }
            case 8: {
                int px;
                if (!parse_px(v, px)) return false;
                st.margin_bottom = px;
                return true;
            }
            case 9: {
                int px;
                if (!parse_px(v, px)) return false;
                st.margin_left = px;
                return true;
            }
            case 10: {
                int px;
                if (!parse_px(v, px)) return false;
                st.margin_right = px;
                return true;
            }
            case 11: {
                int px;
                if (!parse_px(v, px)) return false;
                st.padding = px;
                return true;
            }
            case 12: {
                std::string l = v;
                lower_inplace(l);
                if (l == "block" || l == "list-item") st.display = Display::Block;
                else if (l == "inline") st.display = Display::Inline;
                else if (l == "none") st.display = Display::None;
                else return false;
                return true;
            }
        }
        return false;
    }
    // "8px" | "8px 4px" | "8px 4px 8px" | "8px 4px 8px 4px" (CSS order).
    static bool split_px_list(const std::string& v, int vals[4]) {
        std::string parts[4];
        int n = 0;
        size_t i = 0;
        while (i < v.size() && n < 4) {
            while (i < v.size() && is_space(v[i])) i++;
            if (i >= v.size()) break;
            size_t j = i;
            while (j < v.size() && !is_space(v[j])) j++;
            parts[n++] = v.substr(i, j - i);
            i = j;
        }
        while (i < v.size())
            if (!is_space(v[i++])) return false;  // >4 values
        if (n == 0) return false;
        int px[4];
        for (int k = 0; k < n; k++)
            if (!parse_px(parts[k], px[k])) return false;
        if (n == 1) vals[0] = vals[1] = vals[2] = vals[3] = px[0];
        else if (n == 2) {
            vals[0] = vals[2] = px[0];
            vals[1] = vals[3] = px[1];
        } else if (n == 3) {
            vals[0] = px[0];
            vals[1] = vals[3] = px[1];
            vals[2] = px[2];
        } else {
            vals[0] = px[0];
            vals[1] = px[1];
            vals[2] = px[2];
            vals[3] = px[3];
        }
        return true;
    }
};

bool rule_matches(const Rule& r, const dom::Node* n) {
    if (r.sel == Rule::Sel::Tag) return n->tag == r.key;
    if (r.sel == Rule::Sel::Id) {
        const std::string* id = n->get_attr("id");
        if (!id) return false;
        std::string l = *id;
        lower_inplace(l);
        return l == r.key;
    }
    const std::string* cls = n->get_attr("class");
    if (!cls) return false;
    // Space-separated class list, case-insensitive compare.
    size_t i = 0;
    while (i < cls->size()) {
        while (i < cls->size() && is_space((*cls)[i])) i++;
        if (i >= cls->size()) break;
        size_t j = i;
        while (j < cls->size() && !is_space((*cls)[j])) j++;
        std::string one = cls->substr(i, j - i);
        lower_inplace(one);
        if (one == r.key) return true;
        i = j;
    }
    return false;
}

int rule_spec(const Rule& r) {
    if (r.sel == Rule::Sel::Id) return 3;
    if (r.sel == Rule::Sel::Class) return 2;
    return 1;
}

// UA defaults: display + base typography. display:none elements never lay out.
void ua_defaults(const std::string& tag, ComputedStyle& st) {
    st.display = Display::Inline;  // unknown tags default inline
    if (tag == "html" || tag == "body" || tag == "div" || tag == "p" ||
        tag == "blockquote" || tag == "pre" || tag == "ul" || tag == "ol" ||
        tag == "li" || tag == "form") {
        st.display = Display::Block;
    } else if (tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" ||
               tag == "h5" || tag == "h6") {
        st.display = Display::Block;
        st.bold = true;
    } else if (tag == "head" || tag == "title" || tag == "meta" || tag == "link" ||
               tag == "style" || tag == "script") {
        st.display = Display::None;
    }
    if (tag == "h1") st.font_size = 28;
    else if (tag == "h2") st.font_size = 22;
    else if (tag == "h3") st.font_size = 18;
    else if (tag == "h4") st.font_size = 16;
    if (tag == "h1" || tag == "h2" || tag == "h3") {
        st.margin_top = 12;
        st.margin_bottom = 8;
    }
    if (tag == "p") {
        st.margin_top = 8;
        st.margin_bottom = 8;
    }
    if (tag == "ul" || tag == "ol") {
        st.margin_top = 8;
        st.margin_bottom = 8;
        st.margin_left = 24;
    }
    if (tag == "body") {
        st.margin_top = 8;
        st.margin_bottom = 8;
        st.margin_left = 8;
        st.margin_right = 8;
    }
    if (tag == "a") st.color = 0xFF0000EEu;
    if (tag == "pre") st.pre = true;
}

}  // namespace

void parse_stylesheet(const std::string& src_in, Stylesheet& out) {
    std::string src = strip_comments(src_in);
    size_t i = 0;
    while (i < src.size() && out.rules.size() < MAX_RULES) {
        size_t ob = src.find('{', i);
        if (ob == std::string::npos) break;
        size_t cb = src.find('}', ob + 1);
        if (cb == std::string::npos) break;  // unterminated: drop rest
        std::string sels = src.substr(i, ob - i);
        std::string body = src.substr(ob + 1, cb - ob - 1);
        // Split selector list on ','; each must be simple or the RULE dies.
        size_t k = 0;
        std::vector<Rule::Sel> selt;
        std::vector<std::string> keys;
        bool ok = true;
        while (k <= sels.size()) {
            size_t comma = sels.find(',', k);
            if (comma == std::string::npos) comma = sels.size();
            std::string one = sels.substr(k, comma - k);
            trim(one);
            lower_inplace(one);
            if (!valid_simple_selector(one)) {
                ok = false;
                break;
            }
            Rule::Sel st = Rule::Sel::Tag;
            std::string key = one;
            if (one[0] == '.') {
                st = Rule::Sel::Class;
                key = one.substr(1);
            } else if (one[0] == '#') {
                st = Rule::Sel::Id;
                key = one.substr(1);
            }
            selt.push_back(st);
            keys.push_back(key);
            k = comma + 1;
        }
        if (ok && !selt.empty()) {
            std::vector<Declaration> decls;
            parse_decls(body, decls);
            for (size_t r = 0; r < selt.size() && out.rules.size() < MAX_RULES; r++) {
                Rule rule;
                rule.sel = selt[r];
                rule.key = keys[r];
                rule.decls = decls;
                out.rules.push_back(rule);
            }
        }
        i = cb + 1;
    }
}

void parse_inline(const std::string& src, std::vector<Declaration>& out) {
    parse_decls(src, out);
}

ComputedStyle compute_style(const dom::Node* n,
                            const std::vector<const Stylesheet*>& sheets,
                            const ComputedStyle& parent) {
    ComputedStyle st;
    // Inherit first (UA display/typography overrides below where set).
    st.color = parent.color;
    st.font_size = parent.font_size;
    st.bold = parent.bold;
    st.italic = parent.italic;
    st.align = parent.align;
    ua_defaults(n->tag, st);
    Applier ap(st);
    unsigned long order = 1;
    for (size_t si = 0; si < sheets.size(); si++) {
        const Stylesheet* sh = sheets[si];
        if (!sh) continue;
        for (size_t ri = 0; ri < sh->rules.size(); ri++) {
            const Rule& r = sh->rules[ri];
            if (!rule_matches(r, n)) continue;
            for (size_t di = 0; di < r.decls.size(); di++)
                ap.apply(r.decls[di], rule_spec(r), order++);
        }
    }
    const std::string* inline_style = n->get_attr("style");
    if (inline_style && !inline_style->empty()) {
        std::vector<Declaration> decls;
        parse_inline(*inline_style, decls);
        for (size_t di = 0; di < decls.size(); di++)
            ap.apply(decls[di], 4, order++);  // inline beats everything
    }
    // Clamp font size to a sane range (huge sizes = layout bombs).
    if (st.font_size < 8) st.font_size = 8;
    if (st.font_size > 72) st.font_size = 72;
    return st;
}

bool parse_px(const std::string& v_in, int& out) {
    std::string v = v_in;
    trim(v);
    lower_inplace(v);
    // Urutan penting: lower DULU, cek panjang SETELAHNYA. Versi lama
    // lower di cabang fallback lalu compare(v.size()-2) tanpa guard —
    // value 1-2 char ("0","1") underflow jadi npos (crash google.com).
    if (v.size() < 3 || v.compare(v.size() - 2, 2, "px") != 0) return false;
    std::string num = v.substr(0, v.size() - 2);
    trim(num);
    if (num.empty() || num.size() > 4) return false;
    int n = 0;
    for (size_t i = 0; i < num.size(); i++) {
        if (num[i] < '0' || num[i] > '9') return false;
        n = n * 10 + (num[i] - '0');
    }
    if (n > 4096) return false;
    out = n;
    return true;
}

bool parse_color(const std::string& v_in, uint32_t& out) {
    std::string v = v_in;
    trim(v);
    lower_inplace(v);
    if (!v.empty() && v[0] == '#') {
        std::string h = v.substr(1);
        if (h.size() != 3 && h.size() != 6) return false;
        for (size_t i = 0; i < h.size(); i++) {
            char c = h[i];
            bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            if (!ok) return false;
        }
        unsigned r, g, b;
        if (h.size() == 3) {
            r = (unsigned)(h[0] <= '9' ? h[0] - '0' : h[0] - 'a' + 10);
            g = (unsigned)(h[1] <= '9' ? h[1] - '0' : h[1] - 'a' + 10);
            b = (unsigned)(h[2] <= '9' ? h[2] - '0' : h[2] - 'a' + 10);
            r = r * 17;
            g = g * 17;
            b = b * 17;
        } else {
            auto hex = [](char c) -> unsigned {
                if (c >= '0' && c <= '9') return (unsigned)(c - '0');
                return (unsigned)(c - 'a' + 10);
            };
            r = hex(h[0]) * 16 + hex(h[1]);
            g = hex(h[2]) * 16 + hex(h[3]);
            b = hex(h[4]) * 16 + hex(h[5]);
        }
        out = 0xFF000000u | (r << 16) | (g << 8) | b;
        return true;
    }
    if (v == "black") out = 0xFF000000u;
    else if (v == "white") out = 0xFFFFFFFFu;
    else if (v == "red") out = 0xFFFF0000u;
    else if (v == "green") out = 0xFF008000u;
    else if (v == "blue") out = 0xFF0000FFu;
    else if (v == "yellow") out = 0xFFFFFF00u;
    else if (v == "gray" || v == "grey") out = 0xFF808080u;
    else if (v == "transparent") out = 0x00000000u;
    else return false;
    return true;
}

}  // namespace css
}  // namespace browser
