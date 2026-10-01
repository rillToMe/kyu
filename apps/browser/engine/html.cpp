// Browser HTML parsing entry point + DOM helpers.
//
// Stage B: HTML parsing is delegated to Lexbor 3.0.0 via the dedicated
// adapter (lexbor_html_adapter.{hpp,cpp}). The old handwritten parser was
// removed here; the dom:: helper functions below (find_first/find_all/
// inner_text) are still used by page.cpp/layout/tests and stay.
#include "html.hpp"

#include "lexbor_html_adapter.hpp"

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
bool is_trim_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
}  // namespace

void parse(const std::string& src_in, dom::Document& out) {
    // Cap input bytes before handing to Lexbor (unchanged browser policy).
    std::string src = src_in.size() > MAX_DOC_BYTES ? src_in.substr(0, MAX_DOC_BYTES) : src_in;
    if (src_in.size() > MAX_DOC_BYTES) out.truncated = true;

    // Lexbor owns tokenization, tree construction, malformed-markup recovery,
    // nesting rules and entity decoding. The adapter copies the resulting
    // tree into `out` (KyuBrowser-owned strings only).
    parse_via_lexbor(src, out);

    // Post-pass over the KyuBrowser DOM: title (first) + stylesheet links.
    const dom::Node* title = dom::find_first(out.root.get(), "title");
    if (title) {
        dom::inner_text(title, out.title);
        size_t a = 0;
        while (a < out.title.size() && is_trim_space(out.title[a])) a++;
        size_t b = out.title.size();
        while (b > a && is_trim_space(out.title[b - 1])) b--;
        out.title = out.title.substr(a, b - a);
    }
    std::vector<const dom::Node*> links;
    dom::find_all(out.root.get(), "link", links);
    for (size_t li = 0; li < links.size(); li++) {
        const std::string* rel = links[li]->get_attr("rel");
        const std::string* href = links[li]->get_attr("href");
        if (!rel || !href || href->empty()) continue;
        std::string rl = *rel;
        for (size_t k = 0; k < rl.size(); k++) {
            char ch = rl[k];
            if (ch >= 'A' && ch <= 'Z') rl[k] = (char)(ch + 32);
        }
        if (rl == "stylesheet") out.style_hrefs.push_back(*href);
    }
}

}  // namespace html
}  // namespace browser
