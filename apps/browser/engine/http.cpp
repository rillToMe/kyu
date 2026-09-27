// Browser engine HTTP/1.x client.
#include "http.hpp"
#include "strutil.hpp"

namespace browser {
namespace http {
namespace {

char lower_of(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

bool eq_nocase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (lower_of(a[i]) != lower_of(b[i])) return false;
    return true;
}

void trim(std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) a++;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    s = s.substr(a, b - a);
}

// Split raw into lines (CRLF or bare LF). Returns false if a NUL byte
// appears (binary garbage in headers = malformed, never silently kept).
bool split_lines(const std::string& raw, size_t end, std::vector<std::string>& lines) {
    size_t i = 0;
    while (i < end) {
        size_t e = raw.find('\n', i);
        if (e == std::string::npos || e > end) e = end;
        std::string ln = raw.substr(i, e - i);
        if (!ln.empty() && ln[ln.size() - 1] == '\r') ln.resize(ln.size() - 1);
        for (size_t k = 0; k < ln.size(); k++)
            if (ln[k] == '\0') return false;
        lines.push_back(ln);
        i = e + 1;
    }
    return true;
}

// Parse chunked body starting at `p` (length raw.size()). On success sets
// body and returns true; any shape violation returns false.
bool dechunk(const std::string& raw, size_t p, std::string& body) {
    body.clear();
    for (;;) {
        size_t e = raw.find("\r\n", p);
        size_t lf_only = raw.find('\n', p);
        // Accept bare-LF chunk lines too (same tolerance as headers).
        size_t le;
        bool crlf;
        if (e != std::string::npos && (lf_only == std::string::npos || e <= lf_only)) {
            le = e;
            crlf = true;
        } else if (lf_only != std::string::npos) {
            le = lf_only;
            crlf = false;
            if (le > p && raw[le - 1] == '\r') { le--; }
        } else {
            return false;
        }
        std::string szline = raw.substr(p, le - p);
        size_t semi = szline.find(';');
        if (semi != std::string::npos) szline.resize(semi);
        trim(szline);
        if (szline.empty()) return false;
        unsigned long n = 0;
        for (size_t i = 0; i < szline.size(); i++) {
            char c = szline[i];
            unsigned v;
            if (c >= '0' && c <= '9') v = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = (unsigned)(c - 'A' + 10);
            else return false;
            n = n * 16 + v;
            if (n > MAX_BODY_BYTES) return false;  // cap before growing
        }
        p = le + (crlf ? 2 : 1);
        if (n == 0) return true;  // final chunk; trailers ignored
        if (body.size() + n > MAX_BODY_BYTES) return false;
        if (p + n > raw.size()) return false;  // truncated chunk data
        body.append(raw, p, n);
        p += n;
        // Chunk data must be followed by CRLF (tolerate bare LF).
        if (p + 1 < raw.size() && raw[p] == '\r' && raw[p + 1] == '\n') p += 2;
        else if (p < raw.size() && raw[p] == '\n') p += 1;
        else return false;
    }
}

bool is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

}  // namespace

bool Response::get(const std::string& name, std::string& out) const {
    for (size_t i = 0; i < headers.size(); i++)
        if (eq_nocase(headers[i].name, name)) {
            out = headers[i].value;
            return true;
        }
    return false;
}

FetchError parse_response(const std::string& raw, Response& out) {
    Response r;
    size_t hend = raw.find("\r\n\r\n");
    size_t hend_len = 4;
    if (hend == std::string::npos) {
        hend = raw.find("\n\n");
        hend_len = 2;
    }
    if (hend == std::string::npos) return FetchError::Closed;
    if (hend > MAX_HEADER_BYTES) return FetchError::TooLarge;

    std::vector<std::string> lines;
    if (!split_lines(raw, hend, lines) || lines.empty()) return FetchError::BadResponse;

    // Status line: "HTTP/<ver> SP <3 digits> [SP reason]".
    const std::string& sl = lines[0];
    if (sl.size() < 5 || sl[0] != 'H' || sl[1] != 'T' || sl[2] != 'T' || sl[3] != 'P' ||
        sl[4] != '/')
        return FetchError::BadResponse;
    size_t sp1 = sl.find(' ');
    if (sp1 == std::string::npos) return FetchError::BadResponse;
    size_t code_at = sp1 + 1;
    if (code_at + 3 > sl.size()) return FetchError::BadResponse;
    int code = 0;
    for (int i = 0; i < 3; i++) {
        char c = sl[code_at + i];
        if (c < '0' || c > '9') return FetchError::BadResponse;
        code = code * 10 + (c - '0');
    }
    r.status = code;
    size_t rs = code_at + 3;
    if (rs < sl.size() && sl[rs] == ' ') rs++;
    r.reason = (rs < sl.size()) ? sl.substr(rs) : "";

    // Headers.
    for (size_t i = 1; i < lines.size(); i++) {
        const std::string& ln = lines[i];
        if (!ln.empty() && (ln[0] == ' ' || ln[0] == '\t')) {
            // obs-fold continuation: append to previous value.
            if (r.headers.empty()) return FetchError::BadResponse;
            std::string cont = ln;
            trim(cont);
            if (!cont.empty() && r.headers.back().value.size() + cont.size() + 1 <= MAX_HEADER_BYTES) {
                r.headers.back().value += ' ';
                r.headers.back().value += cont;
            }
            continue;
        }
        size_t colon = ln.find(':');
        if (colon == std::string::npos || colon == 0) return FetchError::BadResponse;
        std::string name = ln.substr(0, colon);
        std::string value = ln.substr(colon + 1);
        trim(value);
        for (size_t k = 0; k < name.size(); k++) name[k] = lower_of(name[k]);
        if (r.headers.size() >= MAX_HEADERS) return FetchError::TooLarge;
        Header h;
        h.name = name;
        h.value = value;
        r.headers.push_back(h);
    }

    size_t body_at = hend + hend_len;
    std::string te, cl;
    bool chunked = r.get("transfer-encoding", te) && eq_nocase(te, "chunked");
    // Tolerate "chunked" among multiple codings ("gzip, chunked")? No:
    // only exact token, documented. Anything else with a body = length/close.
    bool has_cl = r.get("content-length", cl);
    if (chunked) {
        if (!dechunk(raw, body_at, r.body)) return FetchError::BadResponse;
    } else if (has_cl) {
        std::string num = cl;
        trim(num);
        if (num.empty()) return FetchError::BadResponse;
        unsigned long n = 0;
        for (size_t i = 0; i < num.size(); i++) {
            if (num[i] < '0' || num[i] > '9') return FetchError::BadResponse;
            n = n * 10 + (unsigned long)(num[i] - '0');
            if (n > MAX_BODY_BYTES) return FetchError::TooLarge;
        }
        if (body_at + n > raw.size()) return FetchError::BadResponse;  // truncated
        r.body.assign(raw, body_at, n);
    } else {
        // No length, no chunked: body = rest (bounded by fetch's read cap).
        if (raw.size() - body_at > MAX_BODY_BYTES) return FetchError::TooLarge;
        r.body.assign(raw, body_at, raw.size() - body_at);
    }
    out = r;
    return FetchError::Ok;
}

FetchError fetch(Transport& t, const Url& url, Response& out) {
    if (!url.is_fetchable()) return FetchError::UnsupportedScheme;
    Url cur = url;
    for (int redir = 0; ; redir++) {
        OpenError oe = t.open(cur.host, cur.port);
        if (oe == OpenError::Dns) {
            t.close();
            return FetchError::Dns;
        }
        if (oe == OpenError::Timeout) {
            t.close();
            return FetchError::Timeout;
        }
        if (oe == OpenError::Tls) {
            t.close();
            return FetchError::Tls;
        }
        if (oe == OpenError::Conn) {
            t.close();
            return FetchError::Conn;
        }
        std::string req = detail::lit_plus("GET ", cur.request_target());
        req += " HTTP/1.1\r\nHost: ";
        req += cur.host;
        if (!((cur.scheme == "http" && cur.port == 80) ||
              (cur.scheme == "https" && cur.port == 443))) {
            req += ':';
            detail::append_ulong(req, cur.port);
        }
        req += "\r\nConnection: close\r\n\r\n";
        if (!t.write_all(req.data(), req.size())) {
            t.close();
            return FetchError::Conn;
        }
        std::string raw;
        char buf[2048];
        bool peer_closed = false;
        FetchError read_err = FetchError::Ok;
        for (;;) {
            long n = t.read(buf, sizeof(buf));
            if (n > 0) {
                if (raw.size() + (unsigned long)n > MAX_HEADER_BYTES + MAX_BODY_BYTES) {
                    read_err = FetchError::TooLarge;
                    break;
                }
                raw.append(buf, (size_t)n);
                // Early stop: headers complete AND body complete per framing?
                // Simpler: read until close (we sent Connection: close).
                // Bound total; parse decides truncation vs completeness.
            } else if (n == 0) {
                peer_closed = true;
                break;
            } else {
                read_err = FetchError::Timeout;
                break;
            }
        }
        t.close();
        if (read_err != FetchError::Ok) return read_err;
        if (!peer_closed) return FetchError::Closed;  // transport ended oddly

        Response r;
        FetchError pe = parse_response(raw, r);
        if (pe != FetchError::Ok) return pe;
        r.final_url = cur.serialize();
        if (!is_redirect(r.status)) {
            out = r;
            return FetchError::Ok;
        }
        if (redir >= MAX_REDIRECTS) return FetchError::TooManyRedirects;
        std::string loc;
        if (!r.get("location", loc) || loc.empty()) {
            out = r;  // redirect status without Location: deliver as-is
            return FetchError::Ok;
        }
        Url next;
        if (resolve_url(cur, loc, next) != UrlError::Ok) return FetchError::BadUrl;
        if (!next.is_fetchable()) return FetchError::UnsupportedScheme;
        cur = next;
    }
}

const char* to_string(FetchError e) {
    switch (e) {
        case FetchError::Ok: return "ok";
        case FetchError::UnsupportedScheme: return "unsupported scheme";
        case FetchError::BadUrl: return "bad redirect URL";
        case FetchError::Dns: return "DNS lookup failed";
        case FetchError::Conn: return "connection failed";
        case FetchError::Tls: return "TLS handshake failed";
        case FetchError::Timeout: return "connection timed out";
        case FetchError::Closed: return "connection closed unexpectedly";
        case FetchError::BadResponse: return "malformed HTTP response";
        case FetchError::TooLarge: return "response too large";
        case FetchError::TooManyRedirects: return "too many redirects";
    }
    return "?";
}

}  // namespace http
}  // namespace browser
