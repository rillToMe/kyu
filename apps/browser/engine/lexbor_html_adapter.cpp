// Lexbor 3.0.0 -> KyuBrowser DOM adapter (Stage B).
//
// Lifecycle (Lexbor 3.0.0, verified in Stage A / host probes):
//   lxb_html_parser_create -> lxb_html_parser_init -> lxb_html_parse
//   -> traverse Lexbor DOM -> copy into KyuBrowser DOM
//   -> lxb_html_document_destroy -> lxb_html_parser_destroy
//
// Everything below the conversion is *Lexbor's* job: tokenization, tree
// construction, malformed-markup recovery, nesting rules and entity decoding.
// This file only walks the finished tree and copies data out. It is NOT a
// parser and must never re-implement implicit html/head/body, tag balancing
// or entity recovery (see docs/design/browser/lexbor-stage-b.md).
//
// Ownership: every std::string below is a *copy*; no Lexbor pointer escapes.
// The KyuBrowser tree is therefore valid after the Lexbor document/parser are
// destroyed.
#include "lexbor_html_adapter.hpp"

#include "lexbor/html/parser.h"
#include "lexbor/html/interfaces/document.h"
#include "lexbor/dom/interface.h"
#include "lexbor/dom/interfaces/element.h"
#include "lexbor/dom/interfaces/text.h"
#include "lexbor/dom/interfaces/attr.h"

namespace browser {
namespace html {
namespace {

// Browser text-normalization: U+00A0 (UTF-8 C2 A0) -> ASCII space. The old
// handwritten parser decoded &nbsp; to ' '; Lexbor (correctly, per spec) keeps
// U+00A0. This is a deliberate *browser* text policy, not parser logic, so the
// existing KyuBrowser text model and tests are preserved.
void normalize_nbsp(std::string& s) {
    if (s.find('\xC2') == std::string::npos) return;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if ((unsigned char)s[i] == 0xC2 && i + 1 < s.size() &&
            (unsigned char)s[i + 1] == 0xA0) {
            out += ' ';
            i += 2;
        } else {
            out += s[i++];
        }
    }
    s.swap(out);
}

struct Ctx {
    dom::Document& out;
    unsigned long nodes = 1;  // root Document node counts as one
    bool truncated = false;
};

// Append a new element/text node under `parent`, enforcing MAX_NODES.
// Returns null (and sets truncated) once the node budget is exhausted.
dom::Node* new_node(Ctx& c, dom::Node* parent) {
    if (c.nodes >= dom::MAX_NODES) {
        c.truncated = true;
        return nullptr;
    }
    std::unique_ptr<dom::Node> n(new dom::Node());
    n->parent = parent;
    dom::Node* raw = n.get();
    parent->children.push_back(std::move(n));
    c.nodes++;
    return raw;
}

void add_text(Ctx& c, dom::Node* parent, const lxb_char_t* data, size_t len) {
    if (len == 0) return;
    if (c.nodes >= dom::MAX_NODES) {
        c.truncated = true;
        return;
    }
    std::string text(reinterpret_cast<const char*>(data), len);
    normalize_nbsp(text);
    if (text.empty()) return;
    // Merge into a previous text sibling (fewer nodes, identical semantics).
    if (!parent->children.empty()) {
        dom::Node* last = parent->children.back().get();
        if (last->type == dom::NodeType::Text) {
            last->text += text;
            return;
        }
    }
    std::unique_ptr<dom::Node> n(new dom::Node());
    n->type = dom::NodeType::Text;
    n->text = text;
    n->parent = parent;
    parent->children.push_back(std::move(n));
    c.nodes++;
}

// Concatenate raw descendant text (used for <style> bodies, kept verbatim).
// Iterative: the HTML tree may be arbitrarily deep (Lexbor has no depth cap).
void collect_raw_text(const lxb_dom_node_t* n, std::string& out) {
    std::vector<const lxb_dom_node_t*> stack;
    for (const lxb_dom_node_t* ch = n->first_child; ch; ch = ch->next)
        stack.push_back(ch);
    while (!stack.empty()) {
        const lxb_dom_node_t* cur = stack.back();
        stack.pop_back();
        if (cur->type == LXB_DOM_NODE_TYPE_TEXT) {
            lxb_dom_text_t* t = lxb_dom_interface_text(const_cast<lxb_dom_node_t*>(cur));
            out.append(reinterpret_cast<const char*>(t->char_data.data.data),
                       t->char_data.data.length);
        }
        for (const lxb_dom_node_t* ch = cur->first_child; ch; ch = ch->next)
            stack.push_back(ch);
    }
}

// Over the depth cap: drop element structure and attach every descendant text
// node to `parent` (text preserved, structure flattened). Iterative so a very
// deep document cannot blow the C++ stack.
void flatten_text(Ctx& c, const lxb_dom_node_t* src, dom::Node* parent) {
    std::vector<const lxb_dom_node_t*> stack;
    for (const lxb_dom_node_t* ch = src->first_child; ch; ch = ch->next)
        stack.push_back(ch);
    while (!stack.empty()) {
        const lxb_dom_node_t* cur = stack.back();
        stack.pop_back();
        if (cur->type == LXB_DOM_NODE_TYPE_TEXT) {
            lxb_dom_text_t* t = lxb_dom_interface_text(const_cast<lxb_dom_node_t*>(cur));
            add_text(c, parent, t->char_data.data.data, t->char_data.data.length);
        }
        for (const lxb_dom_node_t* ch = cur->first_child; ch; ch = ch->next)
            stack.push_back(ch);
    }
}

void convert_children(Ctx& c, const lxb_dom_node_t* src, dom::Node* parent, int depth);

// Convert one Lexbor element into a KyuBrowser element at `depth + 1`.
void convert_element(Ctx& c, const lxb_dom_node_t* src, dom::Node* parent, int depth) {
    lxb_dom_element_t* el =
        lxb_dom_interface_element(const_cast<lxb_dom_node_t*>(src));

    size_t tlen = 0;
    const lxb_char_t* tname = lxb_dom_element_qualified_name(el, &tlen);
    std::string tag(reinterpret_cast<const char*>(tname), tlen);  // Lexbor lowercases HTML tags

    dom::Node* node = new_node(c, parent);
    if (!node) return;
    node->type = dom::NodeType::Element;
    node->tag = tag;

    for (lxb_dom_attr_t* a = lxb_dom_element_first_attribute(el); a;
         a = lxb_dom_element_next_attribute(a)) {
        if (node->attrs.size() >= dom::MAX_ATTRS_PER_ELEM) break;
        size_t an = 0, av = 0;
        const lxb_char_t* anm = lxb_dom_attr_qualified_name(a, &an);
        const lxb_char_t* avp = lxb_dom_attr_value(a, &av);
        dom::Attribute at;
        at.name.assign(reinterpret_cast<const char*>(anm), an);
        if (avp) {  // boolean attributes have a NULL value -> empty string
            at.value.assign(reinterpret_cast<const char*>(avp), av);
            normalize_nbsp(at.value);
        }
        node->attrs.push_back(std::move(at));
    }

    // Browser document policy (not parser logic): <script>/<style> carry no
    // text children. <script> bodies are dropped (no JS); <style> bodies are
    // collected verbatim into Document::styles in document order.
    if (tag == "script") return;
    if (tag == "style") {
        std::string body;
        collect_raw_text(src, body);
        c.out.styles.push_back(std::move(body));
        return;
    }

    convert_children(c, src, node, depth + 1);
}

// `parent` sits at `depth`; element children would sit at `depth + 1`.
void convert_children(Ctx& c, const lxb_dom_node_t* src, dom::Node* parent, int depth) {
    for (const lxb_dom_node_t* ch = src->first_child; ch; ch = ch->next) {
        switch (ch->type) {
            case LXB_DOM_NODE_TYPE_ELEMENT:
                if (depth >= dom::MAX_DEPTH) {
                    // Depth cap: do not open a deeper element; flatten its
                    // descendants onto the deepest allowed node so text is
                    // preserved (same contract as the old parser's cap).
                    c.truncated = true;
                    flatten_text(c, ch, parent);
                } else {
                    convert_element(c, ch, parent, depth);
                }
                break;
            case LXB_DOM_NODE_TYPE_TEXT: {
                lxb_dom_text_t* t =
                    lxb_dom_interface_text(const_cast<lxb_dom_node_t*>(ch));
                add_text(c, parent, t->char_data.data.data, t->char_data.data.length);
                break;
            }
            default:
                // Comments, doctype, PI and any other node type have no
                // KyuBrowser DOM representation -> explicitly discarded.
                break;
        }
    }
}

}  // namespace

bool parse_via_lexbor(const std::string& src, dom::Document& out) {
    out.root.reset(new dom::Node());
    out.root->type = dom::NodeType::Document;

    Ctx c{out};

    lxb_html_parser_t* parser = lxb_html_parser_create();
    if (!parser) return false;
    if (lxb_html_parser_init(parser) != LXB_STATUS_OK) {
        lxb_html_parser_destroy(parser);
        return false;
    }

    lxb_html_document_t* doc =
        lxb_html_parse(parser, reinterpret_cast<const lxb_char_t*>(src.data()), src.size());
    if (!doc) {  // fatal (allocation) failure; out.root stays a valid Document
        lxb_html_parser_destroy(parser);
        return false;
    }

    // lxb_html_document_t begins with lxb_dom_document_t (which begins with
    // lxb_dom_node_t), so the document is the tree root node.
    const lxb_dom_node_t* droot = lxb_dom_interface_node(doc);
    convert_children(c, droot, out.root.get(), 0);

    lxb_html_document_destroy(doc);
    lxb_html_parser_destroy(parser);

    out.truncated = out.truncated || c.truncated;
    return true;
}

}  // namespace html
}  // namespace browser
