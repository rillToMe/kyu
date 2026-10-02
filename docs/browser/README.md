# Browser

KyuzenOS includes a from-scratch web browser (`apps/browser/`) with an HTML
parser, a CSS engine, a layout engine, an HTTP client, and TLS support via
BearSSL. It is a userspace application built with the C++ SDK.

## Overview

| Component | Location | Responsibility |
| --- | --- | --- |
| URL | `engine/url.{hpp,cpp}` | URL parsing and normalization |
| HTTP | `engine/http.{hpp,cpp}` | HTTP request/response framing |
| HTML | `engine/html.{hpp,cpp}` | HTML parsing into a DOM |
| DOM | `engine/dom.hpp` | Document tree |
| CSS | `engine/css.{hpp,cpp}` | Stylesheet parsing and cascade |
| Layout | `engine/layout.{hpp,cpp}` | Block/inline layout and hit-testing |
| Transport | `transport.{hpp,cpp}` | Socket abstraction |
| TLS | `transport_tls.cpp`, `tls/tls_kyuzen.h` | BearSSL transport |
| Application | `browser_app.{hpp,cpp}`, `main.cpp` | UI and event loop |
| Rendering | `page.{hpp,cpp}`, `font.{hpp,cpp}` | Page painting and fonts |

## Pipeline

```text
URL  →  HTTP request  →  response body
                            │
                            ▼
                    HTML parse → DOM
                            │
                            ▼
                    CSS parse + cascade
                            │
                            ▼
                    Layout (block/inline)
                            │
                            ▼
                    Paint into a canvas
```

## Supported Subset

### HTML

The HTML parser is recovery-oriented: it tolerates malformed markup and builds
a best-effort DOM. It supports common structural and text elements and inline
formatting.

### CSS

The CSS engine parses stylesheets and applies the cascade. Layout supports
block and inline flow with the supported box properties.

### HTTP

The HTTP client performs requests and parses responses. It supports plain HTTP
and HTTPS (via the TLS transport).

### TLS

TLS is provided by **BearSSL** (`third_party/bearssl/`), running entirely in
userspace. Trust anchors are loaded from a prebuilt `brssl ta` bundle; entropy
comes from the kernel's `RDRAND`-backed entropy syscall (87).

| Property | Value |
| --- | --- |
| Library | BearSSL |
| Trust anchors | Prebuilt `brssl ta` bundle |
| Entropy source | Syscall 87 (`RDRAND`), no fallback |

## Testing

The browser engine has host-side tests that run without QEMU:

| Test | Coverage |
| --- | --- |
| `test-browser-url-http` | URL parsing and HTTP framing |
| `test-browser-html` | HTML parsing and DOM recovery |
| `test-browser-css-layout` | CSS cascade + block/inline layout |
| `test-tls` | BearSSL with the prebuilt trust anchors |

Run one with `ctest --test-dir build/host -R <name>`, or all of them with
`./build.sh test`.

See [Testing](../development/testing.md).

## Current Limitations

- A **subset** of HTML and CSS: no JavaScript, no full CSS cascade/selector
  set, no images beyond the supported decode formats, no forms/submission.
- No cookies, no caching layer, no redirects beyond basic handling.
- TLS trust anchors are a fixed bundle.
- No HTTP/2, no WebSocket.
- The engine is a pure C++17 component tested on the host; full end-to-end page
  rendering is exercised via QEMU probes.

## Related Documentation

- [Networking](../networking/README.md) — the socket layer the transport uses
- [C++ SDK](../libraries/cpp-sdk.md) — the build environment
- [Testing](../development/testing.md)
- [Entropy](../networking/stack.md#entropy)
