#pragma once
// Browser engine URL — pure C++17, freestanding-safe.
// Only <string>/<vector>. No exceptions, no Kyuzen headers.
#include <cstdint>
#include <string>
#include <vector>

namespace browser {

inline constexpr unsigned long MAX_URL_LEN = 2048;

enum class UrlError {
    Ok,
    TooLong,      // > MAX_URL_LEN
    BadScheme,    // missing "://" or scheme not http/https
    NoHost,       // empty authority/host
    BadPort,      // non-digit, 0, or > 65535
    BadChar,      // control / space in URL
    HasUserinfo,  // "user@host" — honestly rejected, not silently stripped
};

struct Url {
    std::string scheme = "http";  // lowercase, "http" or "https"
    std::string host;             // lowercase, no port, no brackets
    uint16_t port = 80;
    std::string path = "/";       // always starts with '/'
    std::string query;
    std::string fragment;

    std::string serialize() const;       // full URL without fragment
    std::string request_target() const;  // path + optional ?query
    bool is_http() const { return scheme == "http"; }
    bool is_https() const { return scheme == "https"; }
    // fetch() speaks HTTP semantics for both; whether bytes are encrypted
    // is the Transport's contract (plaintext vs TLS), not the URL's.
    bool is_fetchable() const { return is_http() || is_https(); }
};

UrlError parse_url(const std::string& s, Url& out);

// RFC 3986 section 5, simplified: supports absolute URLs, "/abs",
// "rel", "./rel", "../rel", "?query", "#frag", "" (same document).
// Scheme-relative ("//host/...") inherits base scheme. Anything else
// that parse_url rejects is rejected here too.
UrlError resolve_url(const Url& base, const std::string& ref, Url& out);

const char* to_string(UrlError e);

}  // namespace browser
