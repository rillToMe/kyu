#pragma once
// Browser CSS subset — parser + cascade. Pure C++17, freestanding-safe.
//
// Supported properties: color, background-color, font-size (Npx),
// font-weight (bold/normal), font-style (italic/normal), text-align
// (left/center/right), margin (Npx, 1-4 values), padding (Npx, single),
// display (block/inline/none/list-item). Selectors: tag, .class, #id
// (comma groups allowed; combinators/descendants/pseudo-classes cause the
// whole rule to be IGNORED, documented). Unknown properties ignored.
// Cascade: UA defaults < author sheets (specificity id>class>tag, later
// order wins ties) < inline style. color/font/bold/italic/align inherit;
// background does not.
#include <cstdint>
#include <string>
#include <vector>

#include "dom.hpp"

namespace browser {
namespace css {

inline constexpr unsigned long MAX_RULES = 512;
inline constexpr unsigned long MAX_DECLS_PER_RULE = 32;

struct Declaration {
    std::string prop;   // lowercase, trimmed
    std::string value;  // trimmed (case preserved; color names lowered at use)
};

struct Rule {
    enum class Sel { Tag, Class, Id };
    Sel sel = Sel::Tag;
    std::string key;  // lowercase
    std::vector<Declaration> decls;
};

struct Stylesheet {
    std::vector<Rule> rules;
};

// Appends parsed rules (stops at MAX_RULES, rest dropped silently).
void parse_stylesheet(const std::string& src, Stylesheet& out);
// Parses a style="" attribute body into declarations.
void parse_inline(const std::string& src, std::vector<Declaration>& out);

enum class Display { Block, Inline, None };
enum class Align { Left, Center, Right };

struct ComputedStyle {
    Display display = Display::Block;
    uint32_t color = 0xFF222222u;
    uint32_t bg = 0x00000000u;
    bool has_bg = false;
    int font_size = 16;
    bool bold = false;
    bool italic = false;
    Align align = Align::Left;
    int margin_top = 0;
    int margin_bottom = 0;
    int margin_left = 0;
    int margin_right = 0;
    int padding = 0;
    bool pre = false;
};

// parent = computed style of the parent element (or UA root for top).
ComputedStyle compute_style(const dom::Node* n,
                            const std::vector<const Stylesheet*>& sheets,
                            const ComputedStyle& parent);

bool parse_color(const std::string& v, uint32_t& out);  // #RGB/#RRGGBB + names
bool parse_px(const std::string& v, int& out);          // "Npx" only, 0..4096

}  // namespace css
}  // namespace browser
