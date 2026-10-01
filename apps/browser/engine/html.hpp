#pragma once
// Browser HTML parsing entry point. Pure C++17, freestanding-safe.
//
// Stage B: HTML tokenization, tree construction, malformed-markup recovery,
// nesting rules and entity decoding are delegated to Lexbor 3.0.0 (see
// lexbor_html_adapter.hpp). This header exposes only the stable browser API:
// callers keep calling html::parse() and never see a Lexbor type.
//
// Browser document policy applied on top of Lexbor's tree (see dom.hpp):
//  - Document node is the root; comments/doctype are discarded.
//  - `<script>`/`<style>` carry no text children: <script> bodies are dropped
//    (no JS) and <style> bodies are collected verbatim into Document::styles.
//  - `<title>` text (first one) -> Document::title (trimmed).
//  - `<link rel=stylesheet href>` -> Document::style_hrefs (in order).
//  - MAX_NODES / MAX_DEPTH are enforced during conversion; hitting either sets
//    Document::truncated (text beyond the depth cap is flattened, not lost).
//  - U+00A0 (&nbsp;) is normalized to ASCII space (browser text policy).
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
