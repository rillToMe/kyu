// Browser HTML parser + DOM helpers.
#include "html.hpp"

namespace browser {

namespace dom {

const Node* find_first(const Node* root, const std::string& tag) {
    if (!root) return nullptr;
    if (root->type == NodeType::Element && root->tag == tag) return root;
    for (size_t i = 0; i < root->children.size(); i++) {
        const Node* f = find_first(root->children[i].get(), tag);
        if (f) return f;
    }
    return nullptr;
}

void find_all(const Node* root, const std::string& tag, std::vector<const Node*>& out) {
    if (!root) return;
    if (root->type == NodeType::Element && root->tag == tag) out.push_back(root);
    for (size_t i = 0; i < root->children.size(); i++)
        find_all(root->children[i].get(), tag, out);
}

void inner_text(const Node* root, std::string& out) {
    if (!root) return;
    if (root->type == NodeType::Text) {
        out += root->text;
        return;
    }
    for (size_t i = 0; i < root->children.size(); i++)
        inner_text(root->children[i].get(), out);
}

}  // namespace dom

namespace html {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool is_alnum(char c) { return is_alpha(c) || (c >= '0' && c <= '9'); }
char lower_of(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

bool is_void(const std::string& tag) {
    return tag == "br" || tag == "hr" || tag == "img" || tag == "meta" ||
           tag == "link" || tag == "input";
}

// Decode one entity starting at `p` (points AT '&'). On success appends the
// decoded text and returns the index AFTER ';'. Unknown/incomplete entities
// return npos (caller keeps '&' literally).
size_t decode_entity(const std::string& s, size_t p, std::string& out) {
    size_t semi = s.find(';', p + 1);
    if (semi == std::string::npos || semi - p > 12) return std::string::npos;
    std::string name = s.substr(p + 1, semi - p - 1);
    if (name == "amp") out += '&';
    else if (name == "lt") out += '<';
    else if (name == "gt") out += '>';
    else if (name == "quot") out += '"';
    else if (name == "apos") out += '\'';
    else if (name == "nbsp") out += ' ';
    else if (name.size() > 1 && name[0] == '#') {
        unsigned long v = 0;
        if (name[1] == 'x' || name[1] == 'X') {
            if (name.size() == 2) return std::string::npos;
            for (size_t i = 2; i < name.size(); i++) {
                char c = name[i];
                unsigned d;
                if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
                else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
                else return std::string::npos;
                v = v * 16 + d;
                if (v > 0x10FFFF) return std::string::npos;
            }
        } else {
            for (size_t i = 1; i < name.size(); i++) {
                if (name[i] < '0' || name[i] > '9') return std::string::npos;
                v = v * 10 + (unsigned long)(name[i] - '0');
                if (v > 0x10FFFF) return std::string::npos;
            }
        }
        if (v == 0) return std::string::npos;
        // UTF-8 encode (ASCII fast path; rest as proper 2-4 byte seq).
        if (v < 0x80) out += (char)v;
        else if (v < 0x800) {
            out += (char)(0xC0 | (v >> 6));
            out += (char)(0x80 | (v & 0x3F));
        } else if (v < 0x10000) {
            out += (char)(0xE0 | (v >> 12));
            out += (char)(0x80 | ((v >> 6) & 0x3F));
            out += (char)(0x80 | (v & 0x3F));
        } else {
            out += (char)(0xF0 | (v >> 18));
            out += (char)(0x80 | ((v >> 12) & 0x3F));
            out += (char)(0x80 | ((v >> 6) & 0x3F));
            out += (char)(0x80 | (v & 0x3F));
        }
    } else {
        return std::string::npos;
    }
    return semi + 1;
}

void decode_entities_into(const std::string& s, std::string& out) {
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            size_t after = decode_entity(s, i, out);
            if (after != std::string::npos) {
                i = after;
                continue;
            }
        }
        out += s[i++];
    }
}

struct Builder {
    dom::Document& doc;
    dom::Node* stack[dom::MAX_DEPTH + 1];
    int depth = 0;  // stack[0] = root; open elements 1..depth
    unsigned long nodes = 1;

    explicit Builder(dom::Document& d) : doc(d) {
        doc.root.reset(new dom::Node());
        doc.root->type = dom::NodeType::Document;
        stack[0] = doc.root.get();
    }

    dom::Node* top() { return stack[depth]; }

    bool full() const { return nodes >= dom::MAX_NODES || depth >= dom::MAX_DEPTH; }

    void add_text(const std::string& raw) {
        if (raw.empty()) return;
        // Depth cap blocks open() only: text still attaches to the deepest
        // open element (content preserved, structure flattened). Only the
        // node-count cap drops text.
        if (nodes >= dom::MAX_NODES) {
            doc.truncated = true;
            return;
        }
        std::string decoded;
        decode_entities_into(raw, decoded);
        if (decoded.empty()) return;
        dom::Node* t = top();
        // Merge into previous text sibling (fewer nodes, same semantics).
        if (!t->children.empty()) {
            dom::Node* last = t->children.back().get();
            if (last->type == dom::NodeType::Text) {
                last->text += decoded;
                return;
            }
        }
        std::unique_ptr<dom::Node> n(new dom::Node());
        n->type = dom::NodeType::Text;
        n->text = decoded;
        n->parent = t;
        t->children.push_back(std::move(n));
        nodes++;
    }

    void open(const std::string& tag, std::vector<dom::Attribute>& attrs) {
        // Auto-close rules (recovery 3).
        if ((tag == "p" && top()->type == dom::NodeType::Element && top()->tag == "p") ||
            (tag == "li" && top()->type == dom::NodeType::Element && top()->tag == "li"))
            depth--;
        if (full()) {
            doc.truncated = true;
            return;
        }
        std::unique_ptr<dom::Node> n(new dom::Node());
        n->type = dom::NodeType::Element;
        n->tag = tag;
        n->attrs = attrs;
        n->parent = top();
        dom::Node* raw = n.get();
        top()->children.push_back(std::move(n));
        nodes++;
        if (!is_void(tag)) {
            depth++;
            stack[depth] = raw;
        }
    }

    void close(const std::string& tag) {
        // Pop until match; no match = stray closer, ignored (recovery 2).
        for (int i = depth; i >= 1; i--) {
            if (stack[i]->type == dom::NodeType::Element && stack[i]->tag == tag) {
                depth = i - 1;
                return;
            }
        }
    }
};

// Parse one open tag starting at `p` (s[p] == '<', s[p+1] is alpha).
// Sets tag/attrs/selfclose, returns index after '>'. npos = malformed
// (caller treats '<' as literal text).
size_t parse_open_tag(const std::string& s, size_t p, std::string& tag,
                      std::vector<dom::Attribute>& attrs, bool& selfclose) {
    size_t i = p + 1;
    size_t ne = i;
    while (ne < s.size() && (is_alnum(s[ne]) || s[ne] == '-' || s[ne] == ':')) ne++;
    tag.clear();
    for (size_t k = i; k < ne; k++) tag += lower_of(s[k]);
    i = ne;
    selfclose = false;
    for (;;) {
        while (i < s.size() && is_space(s[i])) i++;
        if (i >= s.size()) return std::string::npos;
        if (s[i] == '>') return i + 1;
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '>') {
            selfclose = true;
            return i + 2;
        }
        if (!is_alpha(s[i]) && s[i] != '_' && s[i] != ':') return std::string::npos;
        size_t an = i;
        while (i < s.size() && (is_alnum(s[i]) || s[i] == '-' || s[i] == '_' || s[i] == ':'))
            i++;
        std::string aname;
        for (size_t k = an; k < i; k++) aname += lower_of(s[k]);
        while (i < s.size() && is_space(s[i])) i++;
        std::string aval;
        if (i < s.size() && s[i] == '=') {
            i++;
            while (i < s.size() && is_space(s[i])) i++;
            if (i < s.size() && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i++];
                size_t ve = s.find(q, i);
                if (ve == std::string::npos) return std::string::npos;
                decode_entities_into(s.substr(i, ve - i), aval);
                i = ve + 1;
            } else {
                size_t ve = i;
                while (ve < s.size() && !is_space(s[ve]) && s[ve] != '>' && s[ve] != '/')
                    ve++;
                decode_entities_into(s.substr(i, ve - i), aval);
                i = ve;
            }
        }
        if (attrs.size() < dom::MAX_ATTRS_PER_ELEM) {
            // First occurrence wins (duplicate attribute recovery).
            bool dup = false;
            for (size_t k = 0; k < attrs.size(); k++)
                if (attrs[k].name == aname) {
                    dup = true;
                    break;
                }
            if (!dup) {
                dom::Attribute a;
                a.name = aname;
                a.value = aval;
                attrs.push_back(a);
            }
        }
    }
}

}  // namespace

void parse(const std::string& src_in, dom::Document& out) {
    Builder b(out);
    std::string src = src_in.size() > MAX_DOC_BYTES ? src_in.substr(0, MAX_DOC_BYTES) : src_in;
    if (src_in.size() > MAX_DOC_BYTES) out.truncated = true;

    size_t i = 0;
    size_t text_from = 0;
    auto flush_text = [&](size_t to) {
        if (to > text_from) b.add_text(src.substr(text_from, to - text_from));
    };

    while (i < src.size()) {
        if (src[i] != '<') {
            i++;
            continue;
        }
        // Comment?
        if (i + 4 <= src.size() && src.compare(i, 4, "<!--") == 0) {
            size_t ce = src.find("-->", i + 4);
            flush_text(i);
            if (ce == std::string::npos) break;  // recovery 6: drop rest
            i = ce + 3;
            text_from = i;
            continue;
        }
        // Doctype / bogus <!...>: skip to '>' (recovery: never a node).
        if (i + 2 <= src.size() && src[i + 1] == '!') {
            size_t ge = src.find('>', i + 2);
            flush_text(i);
            i = (ge == std::string::npos) ? src.size() : ge + 1;
            text_from = i;
            continue;
        }
        // Close tag?
        if (i + 2 <= src.size() && src[i + 1] == '/' && is_alpha(src[i + 2])) {
            size_t ne = i + 2;
            while (ne < src.size() && (is_alnum(src[ne]) || src[ne] == '-')) ne++;
            std::string tag;
            for (size_t k = i + 2; k < ne; k++) tag += lower_of(src[k]);
            size_t k = ne;
            while (k < src.size() && is_space(src[k])) k++;
            if (k < src.size() && src[k] == '>') {
                flush_text(i);
                // Raw-text fast-forward is handled at open; a stray
                // </script> here just closes normally.
                b.close(tag);
                i = k + 1;
                text_from = i;
                continue;
            }
            i++;  // malformed closer: '<' becomes text, rescan from i
            continue;
        }
        // Processing instruction <?...>: skip (recovery).
        if (i + 2 <= src.size() && src[i + 1] == '?') {
            size_t ge = src.find("?>", i + 2);
            flush_text(i);
            i = (ge == std::string::npos) ? src.size() : ge + 2;
            text_from = i;
            continue;
        }
        // Open tag?
        if (i + 1 < src.size() && is_alpha(src[i + 1])) {
            std::string tag;
            std::vector<dom::Attribute> attrs;
            bool selfclose = false;
            size_t after = parse_open_tag(src, i, tag, attrs, selfclose);
            if (after == std::string::npos) {
                i++;  // recovery 7 path: literal '<', keep scanning
                continue;
            }
            flush_text(i);
            if (tag == "script") {
                // Raw text until </script>; body DROPPED (no JS, rule 5).
                std::string close = "</script";
                size_t ce = after;
                bool found = false;
                for (;;) {
                    ce = src.find('<', ce);
                    if (ce == std::string::npos) break;
                    if (src.compare(ce, 8, "</script") == 0) {
                        size_t k = ce + 8;
                        while (k < src.size() && is_space(src[k])) k++;
                        if (k < src.size() && src[k] == '>') {
                            found = true;
                            break;
                        }
                    }
                    ce++;
                }
                // Still create the element node (faithful tree), no children.
                b.open(tag, attrs);
                b.close(tag);
                i = found ? (src.find('>', ce) + 1) : src.size();
                text_from = i;
                (void)close;
                continue;
            }
            if (tag == "style") {
                // Raw text until </style>; body KEPT in Document::styles.
                size_t cs = after;
                size_t ce = cs;
                bool found = false;
                for (;;) {
                    ce = src.find('<', ce);
                    if (ce == std::string::npos) break;
                    if (src.compare(ce, 7, "</style") == 0) {
                        size_t k = ce + 7;
                        while (k < src.size() && is_space(src[k])) k++;
                        if (k < src.size() && src[k] == '>') {
                            found = true;
                            break;
                        }
                    }
                    ce++;
                }
                size_t body_end = found ? ce : src.size();
                out.styles.push_back(src.substr(cs, body_end - cs));
                b.open(tag, attrs);
                b.close(tag);
                i = found ? (src.find('>', ce) + 1) : src.size();
                text_from = i;
                continue;
            }
            b.open(tag, attrs);
            if (selfclose) b.close(tag);  // harmless for voids; closes non-void too
            i = after;
            text_from = i;
            continue;
        }
        i++;  // recovery 7: bare '<' is text
    }
    flush_text(src.size());

    // Post-pass: title (first), stylesheet links.
    const dom::Node* title = dom::find_first(out.root.get(), "title");
    if (title) {
        // Only direct text counts; nested markup in title is flattened.
        dom::inner_text(title, out.title);
        // Trim surrounding whitespace (tabs/newlines around titles are noise).
        size_t a = 0;
        while (a < out.title.size() && (out.title[a] == ' ' || out.title[a] == '\t' ||
                                        out.title[a] == '\n' || out.title[a] == '\r'))
            a++;
        size_t bb = out.title.size();
        while (bb > a && (out.title[bb - 1] == ' ' || out.title[bb - 1] == '\t' ||
                          out.title[bb - 1] == '\n' || out.title[bb - 1] == '\r'))
            bb--;
        out.title = out.title.substr(a, bb - a);
    }
    std::vector<const dom::Node*> links;
    dom::find_all(out.root.get(), "link", links);
    for (size_t li = 0; li < links.size(); li++) {
        const std::string* rel = links[li]->get_attr("rel");
        const std::string* href = links[li]->get_attr("href");
        if (!rel || !href || href->empty()) continue;
        std::string rl = *rel;
        for (size_t k = 0; k < rl.size(); k++) rl[k] = lower_of(rl[k]);
        if (rl == "stylesheet") out.style_hrefs.push_back(*href);
    }
}

}  // namespace html
}  // namespace browser
