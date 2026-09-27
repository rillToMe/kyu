#pragma once
// Browser layout — block/inline engine over styled DOM. Pure C++17,
// freestanding-safe. This is NOT libui layout: boxes are browser-internal.
//
// Model: block containers stack children vertically (margins ADD — no
// collapsing, documented simplification). Inline content flows into lines
// with word wrapping via an injected TextMeasure (host tests use a fixed
// metric; the app wires FreeType). Images are replaced runs: intrinsic
// size from ImageSize, scaled DOWN to fit (never up), broken/missing ->
// deterministic 64x64 placeholder box (img_broken).
// Coordinates: content space, y grows down. Caller subtracts scroll for hit.
#include <memory>
#include <string>
#include <vector>

#include "css.hpp"
#include "dom.hpp"

namespace browser {
namespace layout {

struct TextMeasure {
    virtual ~TextMeasure() {}
    virtual int width(const std::string& text, int size, bool bold) const = 0;
    virtual int line_height(int size) const = 0;
};

struct ImageSize {
    virtual ~ImageSize() {}
    virtual bool size(const std::string& src, int& w, int& h) const = 0;
};

struct Run {
    const dom::Node* node = nullptr;  // text owner, or the <img>
    const dom::Node* link = nullptr;  // nearest <a> ancestor-or-self (or null)
    std::string href;                 // resolved? NO — raw href; app resolves
    std::string text;                 // empty for images
    bool is_image = false;
    std::string img_src;
    bool img_broken = false;
    css::ComputedStyle style;
    int x = 0, y = 0, w = 0, h = 0;
};

struct Line {
    int y = 0, h = 0;
    std::vector<Run> runs;
};

struct Box {
    const dom::Node* node = nullptr;
    css::ComputedStyle style;
    int x = 0, y = 0, w = 0, h = 0;  // border box (margin sits OUTSIDE)
    bool is_hr = false;
    std::vector<std::unique_ptr<Box>> kids;
    std::vector<Line> lines;  // inline content (empty when kids carry blocks)
};

struct Layout {
    std::unique_ptr<Box> root;
    int width = 0;
    int height = 0;
};

// Container = <body> if present, else the Document root. viewport_width is
// the content width in px (margins of container apply inside it).
void build_layout(const dom::Document& doc,
                  const std::vector<const css::Stylesheet*>& sheets, int viewport_width,
                  const TextMeasure& tm, const ImageSize& im, Layout& out);

// Deepest link run containing (x, y) in content coords. href is raw.
bool hit_link(const Layout& l, int x, int y, std::string& href_out);

}  // namespace layout
}  // namespace browser
