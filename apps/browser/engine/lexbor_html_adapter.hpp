#pragma once
// Lexbor 3.0.0 -> KyuBrowser DOM adapter (Stage B).
//
// This is the ONLY place in the browser engine that knows Lexbor exists.
// It converts a completed Lexbor HTML document into the existing KyuBrowser
// DOM (dom.hpp). The KyuBrowser DOM never holds a Lexbor pointer: every tag,
// text, attribute name and attribute value is copied into std::string, so the
// tree stays valid after the Lexbor document/parser are destroyed.
//
// The adapter is a *tree conversion* layer, not a parser: HTML tokenization,
// tree construction, malformed-markup recovery, nesting rules and entity
// decoding are all owned by Lexbor (see docs/design/browser/lexbor-stage-b.md).
//
// This header is deliberately C++-clean and does NOT include any Lexbor header
// (no Lexbor type leaks into the browser engine API).
#include <string>

#include "dom.hpp"

namespace browser {
namespace html {

// Parse `src` as an HTML document using Lexbor 3.0.0 and convert the result
// into `out`. Always leaves `out.root` as a valid Document node (the existing
// html::parse() contract: structural success even on empty/malformed input).
// Returns false only on a fatal Lexbor allocation/initialization failure; in
// that case `out.root` is an empty Document node.
bool parse_via_lexbor(const std::string& src, dom::Document& out);

}  // namespace html
}  // namespace browser
