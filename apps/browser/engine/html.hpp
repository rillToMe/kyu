#pragma once
// Browser HTML parser — forgiving subset. Pure C++17, freestanding-safe.
//
// Supported elements: html head body title meta div span p h1-h6 br hr
// strong em b i u a img ul ol li pre code blockquote button input.
// Unknown tags: kept as elements, children rendered (inline by default).
//
// RECOVERY RULES (documented, deterministic):
// 1. Unclosed tags at EOF are auto-closed (no error, no crash).
// 2. Stray close tags with no open match are ignored.
// 3. `<p>` auto-closes an open `<p>`; `<li>` auto-closes an open `<li>`.
// 4. Void elements (br hr img meta link input) never take children, with
//    or without trailing "/".
// 5. `<script>`/`<style>` content is raw text until the matching close tag
//    (markup inside is NOT parsed). <script> bodies are DROPPED (no JS).
// 6. `<!--` without `-->` drops the rest of input (safe truncation).
// 7. A `<` not followed by letter / ! / / / ? is literal text.
// 8. Unknown entities are kept literally ("&foo;" stays "&foo;").
// 9. Nesting deeper than MAX_DEPTH closes inner content at the cap;
//    node count beyond MAX_NODES stops parsing (Document::truncated).
// 10. `<title>` keeps only the FIRST one; extra titles become dropping text.
#include <string>

#include "dom.hpp"

namespace browser {
namespace html {

// Parses `src` (max MAX_DOC_BYTES) into `out`. Always succeeds structurally
// (never crashes on malformed input); `out.truncated` reports cap hits.
inline constexpr unsigned long MAX_DOC_BYTES = 512ul * 1024ul;

void parse(const std::string& src, dom::Document& out);

}  // namespace html
}  // namespace browser
