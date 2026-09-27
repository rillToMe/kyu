#pragma once
// Browser DOM — lightweight, owns its data. Pure C++17, freestanding-safe.
// Only <string>/<vector>/<memory>. No libui, no parser buffers referenced:
// text/attrs are copied into std::string at build time.
#include <memory>
#include <string>
#include <vector>

namespace browser {
namespace dom {

inline constexpr unsigned long MAX_NODES = 4096;
inline constexpr unsigned long MAX_ATTRS_PER_ELEM = 32;
inline constexpr int MAX_DEPTH = 128;

enum class NodeType { Document, Element, Text };

struct Attribute {
    std::string name;   // lowercase
    std::string value;  // entities decoded
};

struct Node {
    NodeType type = NodeType::Element;
    std::string tag;    // lowercase, elements only
    std::string text;   // text nodes only (decoded, uncollapsed)
    std::vector<Attribute> attrs;
    std::vector<std::unique_ptr<Node>> children;
    Node* parent = nullptr;

    const std::string* get_attr(const std::string& name) const {
        for (size_t i = 0; i < attrs.size(); i++)
            if (attrs[i].name == name) return &attrs[i].value;
        return nullptr;
    }
};

struct Document {
    std::unique_ptr<Node> root;          // always present (Document node)
    std::string title;                   // first <title> text, entities decoded
    std::vector<std::string> styles;     // raw <style> bodies, in order
    std::vector<std::string> style_hrefs;  // <link rel=stylesheet href>
    bool truncated = false;              // hit a cap; tree is prefix-safe
};

// Depth-first find: first element with tag (tag already lowercase).
const Node* find_first(const Node* root, const std::string& tag);
// Collect all elements with tag, in document order.
void find_all(const Node* root, const std::string& tag, std::vector<const Node*>& out);
// Concatenated descendant text (for <title>/<style> extraction).
void inner_text(const Node* root, std::string& out);

}  // namespace dom
}  // namespace browser
