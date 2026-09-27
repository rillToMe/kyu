// Browser engine URL parser + relative resolution.
#include "url.hpp"
#include "strutil.hpp"

namespace browser {
namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

void to_lower(std::string& s) {
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] + 32);
}

// Split path on '/', resolve "." / "..", rejoin. Never escapes root:
// excess ".." is absorbed (browser policy, documented).
std::string remove_dot_segments(const std::string& path) {
    std::vector<std::string> segs;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string s = path.substr(i, j - i);
        if (s == "..") {
            if (segs.size() > 1) segs.pop_back();  // never pop root ""
        } else if (s != "." && !(s.empty() && !segs.empty())) {
            // keep real segments and the leading "" (root slash)
            segs.push_back(s);
        }
        i = j + 1;
    }
    if (segs.empty()) return "/";
    std::string out;
    for (size_t k = 0; k < segs.size(); k++) {
        out += segs[k];
        if (k + 1 < segs.size()) out += '/';
    }
    // Preserve trailing slash when the input had one (or ended in . / ..).
    if (!out.empty() && out[out.size() - 1] != '/' &&
        !path.empty() && path[path.size() - 1] == '/')
        out += '/';
    if (out.empty()) out = "/";  // input was "/" (or all dots at root)
    return out;
}

// Parse [host][:port] authority. Returns false on any violation.
bool parse_authority(const std::string& auth, std::string& host, uint16_t& port) {
    if (auth.empty()) return false;
    if (auth.find('@') != std::string::npos) return false;  // userinfo: reject
    if (auth[0] == '[') return false;  // IPv6: honestly unsupported
    size_t colon = auth.find(':');
    std::string h = (colon == std::string::npos) ? auth : auth.substr(0, colon);
    if (h.empty()) return false;
    host = h;
    to_lower(host);
    if (colon == std::string::npos) {
        port = 80;  // fixed up by caller per scheme
        return true;
    }
    std::string ps = auth.substr(colon + 1);
    if (ps.empty() || ps.size() > 5) return false;
    unsigned long v = 0;
    for (size_t i = 0; i < ps.size(); i++) {
        if (!is_digit(ps[i])) return false;
        v = v * 10 + (unsigned long)(ps[i] - '0');
    }
    if (v == 0 || v > 65535) return false;
    port = (uint16_t)v;
    return true;
}

}  // namespace

std::string Url::serialize() const {
    std::string s = scheme + "://" + host;
    bool def = (scheme == "http" && port == 80) || (scheme == "https" && port == 443);
    if (!def) {
        s += ':';
        detail::append_ulong(s, port);
    }
    s += path.empty() ? "/" : path;
    if (!query.empty()) {
        s += '?';
        s += query;
    }
    return s;
}

std::string Url::request_target() const {
    std::string t = path.empty() ? "/" : path;
    if (!query.empty()) {
        t += '?';
        t += query;
    }
    return t;
}

UrlError parse_url(const std::string& s, Url& out) {
    if (s.size() > MAX_URL_LEN) return UrlError::TooLong;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 32 || c == 127) return UrlError::BadChar;
    }
    size_t scheme_end = s.find("://");
    if (scheme_end == std::string::npos || scheme_end == 0) return UrlError::BadScheme;
    std::string scheme = s.substr(0, scheme_end);
    to_lower(scheme);
    if (scheme != "http" && scheme != "https") return UrlError::BadScheme;

    size_t rest = scheme_end + 3;
    size_t auth_end = s.find_first_of("/?#", rest);
    std::string auth = (auth_end == std::string::npos) ? s.substr(rest)
                                                       : s.substr(rest, auth_end - rest);
    std::string host;
    uint16_t port = 0;
    if (!parse_authority(auth, host, port)) {
        if (auth.empty()) return UrlError::NoHost;
        if (auth.find('@') != std::string::npos) return UrlError::HasUserinfo;
        return UrlError::BadPort;
    }
    if (port == 80 && auth.find(':') == std::string::npos)
        port = (scheme == "https") ? 443 : 80;

    Url u;
    u.scheme = scheme;
    u.host = host;
    u.port = port;
    u.path = "/";
    size_t i = (auth_end == std::string::npos) ? s.size() : auth_end;
    // path
    if (i < s.size() && s[i] == '/') {
        size_t pe = s.find_first_of("?#", i);
        u.path = (pe == std::string::npos) ? s.substr(i) : s.substr(i, pe - i);
        if (u.path.empty()) u.path = "/";
        i = (pe == std::string::npos) ? s.size() : pe;
    }
    // query
    if (i < s.size() && s[i] == '?') {
        size_t qe = s.find('#', i);
        u.query = (qe == std::string::npos) ? s.substr(i + 1) : s.substr(i + 1, qe - i - 1);
        i = (qe == std::string::npos) ? s.size() : qe;
    }
    // fragment
    if (i < s.size() && s[i] == '#') u.fragment = s.substr(i + 1);
    out = u;
    return UrlError::Ok;
}

UrlError resolve_url(const Url& base, const std::string& ref, Url& out) {
    if (ref.size() > MAX_URL_LEN) return UrlError::TooLong;
    for (size_t i = 0; i < ref.size(); i++) {
        unsigned char c = (unsigned char)ref[i];
        if (c <= 32 || c == 127) return UrlError::BadChar;
    }
    if (ref.empty()) {
        out = base;  // same document
        return UrlError::Ok;
    }
    if (ref[0] == '#') {
        out = base;
        out.fragment = ref.substr(1);
        return UrlError::Ok;
    }
    // Absolute URL (has scheme://) or scheme-relative (//host/...).
    size_t se = ref.find("://");
    if (se != std::string::npos) return parse_url(ref, out);
    if (ref.size() >= 2 && ref[0] == '/' && ref[1] == '/')
        return parse_url(base.scheme + ":" + ref, out);

    // Split ref into path / query / fragment.
    std::string rpath = ref, rquery, rfrag;
    size_t h = rpath.find('#');
    if (h != std::string::npos) {
        rfrag = rpath.substr(h + 1);
        rpath = rpath.substr(0, h);
    }
    size_t q = rpath.find('?');
    if (q != std::string::npos) {
        rquery = rpath.substr(q + 1);
        rpath = rpath.substr(0, q);
    }

    Url u = base;
    u.fragment = rfrag;
    if (!rpath.empty()) {
        if (rpath[0] == '/') {
            u.path = remove_dot_segments(rpath);
        } else {
            // Merge with base directory, then normalize.
            std::string dir = base.path;
            size_t slash = dir.rfind('/');
            dir = (slash == std::string::npos) ? "/" : dir.substr(0, slash + 1);
            u.path = remove_dot_segments(dir + rpath);
        }
        u.query = rquery;
    } else {
        u.query = rquery;  // "?q" keeps base path, replaces query
    }
    out = u;
    return UrlError::Ok;
}

const char* to_string(UrlError e) {
    switch (e) {
        case UrlError::Ok: return "ok";
        case UrlError::TooLong: return "URL too long";
        case UrlError::BadScheme: return "unsupported scheme (need http:// or https://)";
        case UrlError::NoHost: return "missing host";
        case UrlError::BadPort: return "bad port";
        case UrlError::BadChar: return "bad character in URL";
        case UrlError::HasUserinfo: return "user@host not supported";
    }
    return "?";
}

}  // namespace browser
