// browser_url_http_test — host tests for the browser engine URL + HTTP.
// Pure C++17, no Kyuzen headers. Run: make test-browser-url-http
#include <cstdio>
#include <cstring>
#include <string>

#include "../../../apps/browser/engine/url.cpp"
#include "../../../apps/browser/engine/http.cpp"

using namespace browser;

static int failures;
static const char* cur_test;
#define CHECK(c) do { \
    if (!(c)) { printf("FAIL [%s] line %d: %s\n", cur_test, __LINE__, #c); failures++; } \
} while (0)
#define T(name) static void name(void)

// ---------- URL parse ----------

T(t_parse_basic) {
    cur_test = "parse_basic";
    Url u;
    CHECK(parse_url("http://example.com", u) == UrlError::Ok);
    CHECK(u.scheme == "http" && u.host == "example.com" && u.port == 80);
    CHECK(u.path == "/" && u.query.empty() && u.fragment.empty());
    CHECK(u.serialize() == "http://example.com/");
    CHECK(u.request_target() == "/");
}

T(t_parse_full) {
    cur_test = "parse_full";
    Url u;
    CHECK(parse_url("http://example.com:8080/a/b?q=kyu#frag", u) == UrlError::Ok);
    CHECK(u.port == 8080 && u.path == "/a/b" && u.query == "q=kyu" && u.fragment == "frag");
    CHECK(u.serialize() == "http://example.com:8080/a/b?q=kyu");
    CHECK(u.request_target() == "/a/b?q=kyu");
}

T(t_parse_case) {
    cur_test = "parse_case";
    Url u;
    CHECK(parse_url("HTTP://Example.COM/Path", u) == UrlError::Ok);
    CHECK(u.scheme == "http" && u.host == "example.com" && u.path == "/Path");
}

T(t_parse_https) {
    cur_test = "parse_https";
    Url u;
    CHECK(parse_url("https://example.com/", u) == UrlError::Ok);
    CHECK(u.scheme == "https" && u.port == 443 && !u.is_http());
}

T(t_parse_errors) {
    cur_test = "parse_errors";
    Url u;
    CHECK(parse_url("example.com", u) == UrlError::BadScheme);
    CHECK(parse_url("ftp://example.com/", u) == UrlError::BadScheme);
    CHECK(parse_url("http://", u) == UrlError::NoHost);
    CHECK(parse_url("http:///path", u) == UrlError::NoHost);
    CHECK(parse_url("http://h:0/", u) == UrlError::BadPort);
    CHECK(parse_url("http://h:99999/", u) == UrlError::BadPort);
    CHECK(parse_url("http://h:abc/", u) == UrlError::BadPort);
    CHECK(parse_url("http://h:/", u) == UrlError::BadPort);
    CHECK(parse_url("http://user@h/", u) == UrlError::HasUserinfo);
    CHECK(parse_url("http://ho st/", u) == UrlError::BadChar);
    CHECK(parse_url("http://h/[::1]/", u) == UrlError::Ok);  // brackets in PATH are legal
    CHECK(u.path == "/[::1]/");
    CHECK(parse_url("http://[::1]/", u) != UrlError::Ok);  // IPv6 literal: honestly rejected
    CHECK(parse_url(std::string(3000, 'a'), u) == UrlError::TooLong);
}

// ---------- URL resolve ----------

static Url base_doc() {
    Url b;
    CHECK(parse_url("http://example.com/docs/page.html", b) == UrlError::Ok);
    return b;
}

T(t_resolve_abs) {
    cur_test = "resolve_abs";
    Url o;
    CHECK(resolve_url(base_doc(), "/style.css", o) == UrlError::Ok);
    CHECK(o.serialize() == "http://example.com/style.css");
}

T(t_resolve_rel) {
    cur_test = "resolve_rel";
    Url o;
    CHECK(resolve_url(base_doc(), "other.html", o) == UrlError::Ok);
    CHECK(o.serialize() == "http://example.com/docs/other.html");
    CHECK(resolve_url(base_doc(), "./x.html", o) == UrlError::Ok);
    CHECK(o.serialize() == "http://example.com/docs/x.html");
}

T(t_resolve_dotdot) {
    cur_test = "resolve_dotdot";
    Url o;
    CHECK(resolve_url(base_doc(), "../image.png", o) == UrlError::Ok);
    CHECK(o.serialize() == "http://example.com/image.png");
    CHECK(resolve_url(base_doc(), "../../../../../../etc", o) == UrlError::Ok);
    CHECK(o.serialize() == "http://example.com/etc");  // clamped at root
}

T(t_resolve_frag_query) {
    cur_test = "resolve_frag_query";
    Url o;
    CHECK(resolve_url(base_doc(), "#s2", o) == UrlError::Ok);
    CHECK(o.path == "/docs/page.html" && o.fragment == "s2");
    CHECK(resolve_url(base_doc(), "?q=1", o) == UrlError::Ok);
    CHECK(o.path == "/docs/page.html" && o.query == "q=1");
    CHECK(resolve_url(base_doc(), "", o) == UrlError::Ok);
    CHECK(o.serialize() == "http://example.com/docs/page.html");
}

T(t_resolve_absolute) {
    cur_test = "resolve_absolute";
    Url o;
    CHECK(resolve_url(base_doc(), "http://other.com:8080/x", o) == UrlError::Ok);
    CHECK(o.host == "other.com" && o.port == 8080);
    CHECK(resolve_url(base_doc(), "//cdn.com/lib.css", o) == UrlError::Ok);
    CHECK(o.scheme == "http" && o.host == "cdn.com");
}

// ---------- HTTP ----------

using namespace browser::http;

struct FakeTransport : Transport {
    std::string script;   // bytes the peer sends
    size_t pos = 0;
    size_t chunk = 1 << 30;  // max bytes per read (tests incremental)
    OpenError open_rc = OpenError::Ok;
    bool write_ok = true;
    long read_err_at = -1;  // after this many reads, return -1
    long reads = 0;
    int opens = 0, closes = 0;
    std::string written;

    OpenError open(const std::string&, uint16_t) override {
        opens++;
        pos = 0;
        reads = 0;
        return open_rc;
    }
    bool write_all(const char* d, unsigned long n) override {
        written.append(d, (size_t)n);
        return write_ok;
    }
    long read(char* buf, unsigned long cap) override {
        if (read_err_at >= 0 && reads >= read_err_at) return -1;
        if (pos >= script.size()) return 0;
        unsigned long n = script.size() - pos;
        if (n > chunk) n = chunk;
        if (n > cap) n = cap;
        memcpy(buf, script.data() + pos, (size_t)n);
        pos += (size_t)n;
        reads++;
        return (long)n;
    }
    void close() override { closes++; }
};

static Url http_url(const char* s) {
    Url u;
    CHECK(parse_url(s, u) == UrlError::Ok);
    return u;
}

T(t_http_200_cl) {
    cur_test = "http_200_cl";
    FakeTransport t;
    t.script = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/html\r\n\r\nhello";
    t.chunk = 3;
    Response r;
    CHECK(fetch(t, http_url("http://h/index.html"), r) == FetchError::Ok);
    CHECK(r.status == 200 && r.body == "hello" && r.reason == "OK");
    CHECK(r.final_url == "http://h/index.html");
    std::string ct;
    CHECK(r.get("content-type", ct) && ct == "text/html");
    CHECK(t.written.find("GET /index.html HTTP/1.1\r\n") == 0);
    CHECK(t.written.find("Host: h\r\n") != std::string::npos);
    CHECK(t.closes == 1);
}

T(t_http_chunked) {
    cur_test = "http_chunked";
    FakeTransport t;
    t.script = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
    t.chunk = 4;
    Response r;
    CHECK(fetch(t, http_url("http://h/"), r) == FetchError::Ok);
    CHECK(r.body == "hello world");
}

T(t_http_chunked_ext_barelf) {
    cur_test = "http_chunked_ext_barelf";
    FakeTransport t;
    t.script = "HTTP/1.0 200 ok\nTransfer-Encoding: chunked\n\nb;ext=x\nhello world\n0\n\n";
    Response r;
    CHECK(fetch(t, http_url("http://h/"), r) == FetchError::Ok);
    CHECK(r.body == "hello world");
}

T(t_http_close_delimited) {
    cur_test = "http_close_delimited";
    FakeTransport t;
    t.script = "HTTP/1.0 200 OK\r\n\r\nabc";
    Response r;
    CHECK(fetch(t, http_url("http://h/"), r) == FetchError::Ok);
    CHECK(r.body == "abc");
}

T(t_http_redirect) {
    cur_test = "http_redirect";
    FakeTransport t;
    // First hop: redirect; FakeTransport replays same script per open, so
    // emulate two hops by switching script after first close is observed.
    // Simpler: single-hop relative redirect then final served by same script?
    // Use absolute redirect to a path served by the SAME script is wrong.
    // Instead: script1 then script2 via custom transport below.
    (void)t;
    struct TwoHop : FakeTransport {
        int hop = 0;
        OpenError open(const std::string&, uint16_t) override {
            opens++;
            hop++;
            pos = 0;
            reads = 0;
            script = (hop == 1)
                         ? "HTTP/1.1 302 Found\r\nLocation: /new\r\nContent-Length: 0\r\n\r\n"
                         : "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";
            return OpenError::Ok;
        }
    } t2;
    Response r;
    CHECK(fetch(t2, http_url("http://h/old"), r) == FetchError::Ok);
    CHECK(r.status == 200 && r.body == "hi");
    CHECK(r.final_url == "http://h/new");
    CHECK(t2.opens == 2);
}

T(t_http_redirect_loop) {
    cur_test = "http_redirect_loop";
    struct Loop : FakeTransport {
        OpenError open(const std::string&, uint16_t) override {
            opens++;
            pos = 0;
            reads = 0;
            script = "HTTP/1.1 301 Moved\r\nLocation: /a\r\nContent-Length: 0\r\n\r\n";
            return OpenError::Ok;
        }
    } t;
    Response r;
    CHECK(fetch(t, http_url("http://h/a"), r) == FetchError::TooManyRedirects);
    CHECK(t.opens == MAX_REDIRECTS + 1);
}

T(t_http_redirect_no_location) {
    cur_test = "http_redirect_no_location";
    FakeTransport t;
    t.script = "HTTP/1.1 302 Found\r\nContent-Length: 0\r\n\r\n";
    Response r;
    CHECK(fetch(t, http_url("http://h/"), r) == FetchError::Ok);
    CHECK(r.status == 302);  // delivered as-is
}

T(t_http_errors) {
    cur_test = "http_errors";
    {
        FakeTransport t;
        t.open_rc = OpenError::Dns;
        Response r;
        CHECK(fetch(t, http_url("http://h/"), r) == FetchError::Dns);
    }
    {
        FakeTransport t;
        t.open_rc = OpenError::Conn;
        Response r;
        CHECK(fetch(t, http_url("http://h/"), r) == FetchError::Conn);
    }
    {
        FakeTransport t;
        t.open_rc = OpenError::Tls;
        Response r;
        CHECK(fetch(t, http_url("https://h/"), r) == FetchError::Tls);
    }
    {
        FakeTransport t;
        t.write_ok = false;
        Response r;
        CHECK(fetch(t, http_url("http://h/"), r) == FetchError::Conn);
    }
    {
        FakeTransport t;
        t.script = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nabc";  // truncated
        Response r;
        CHECK(fetch(t, http_url("http://h/"), r) == FetchError::BadResponse);
    }
    {
        FakeTransport t;
        t.script = "GARBAGE\r\n\r\n";
        Response r;
        CHECK(fetch(t, http_url("http://h/"), r) == FetchError::BadResponse);
    }
    {
        FakeTransport t;
        t.script = "HTTP/1.1 200 OK\r\nNoColon\r\n\r\n";
        Response r;
        CHECK(fetch(t, http_url("http://h/"), r) == FetchError::BadResponse);
    }
    {
        FakeTransport t;
        t.script = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZZ\r\n";
        Response r;
        CHECK(fetch(t, http_url("http://h/"), r) == FetchError::BadResponse);
    }
    {
        FakeTransport t;
        t.script = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n";
        t.read_err_at = 1;
        Response r;
        CHECK(fetch(t, http_url("http://h/"), r) == FetchError::Timeout);
    }
    {
        // https speaks the same HTTP semantics at engine level; whether
        // bytes are encrypted is the Transport's contract (plaintext here).
        FakeTransport t;
        t.script = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";
        Response r;
        CHECK(fetch(t, http_url("https://h/"), r) == FetchError::Ok);
        CHECK(r.status == 200 && r.body == "hi");
        CHECK(t.opens == 1);
    }
    {
        FakeTransport t;  // 404 is a valid response, not a fetch error
        t.script = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found";
        Response r;
        CHECK(fetch(t, http_url("http://h/nope"), r) == FetchError::Ok);
        CHECK(r.status == 404 && r.body == "not found");
    }
}

T(t_http_limits) {
    cur_test = "http_limits";
    {
        FakeTransport t;  // header cap
        t.script = "HTTP/1.1 200 OK\r\nX-Pad: " + std::string(40000, 'a') + "\r\n\r\n";
        Response r;
        CHECK(fetch(t, http_url("http://h/"), r) == FetchError::TooLarge);
    }
    {
        FakeTransport t;  // body cap via lying Content-Length
        t.script = "HTTP/1.1 200 OK\r\nContent-Length: 99999999\r\n\r\n";
        Response r;
        CHECK(fetch(t, http_url("http://h/"), r) == FetchError::TooLarge);
    }
    {
        FakeTransport t;  // too many headers
        std::string s = "HTTP/1.1 200 OK\r\n";
        for (int i = 0; i < 80; i++) s += "X-A: b\r\n";
        s += "\r\n";
        t.script = s;
        Response r;
        CHECK(fetch(t, http_url("http://h/"), r) == FetchError::TooLarge);
    }
}

T(t_http_host_port) {
    cur_test = "http_host_port";
    FakeTransport t;
    t.script = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    Response r;
    CHECK(fetch(t, http_url("http://h:8080/x"), r) == FetchError::Ok);
    CHECK(t.written.find("Host: h:8080\r\n") != std::string::npos);
}

int main() {
    t_parse_basic();
    t_parse_full();
    t_parse_case();
    t_parse_https();
    t_parse_errors();
    t_resolve_abs();
    t_resolve_rel();
    t_resolve_dotdot();
    t_resolve_frag_query();
    t_resolve_absolute();
    t_http_200_cl();
    t_http_chunked();
    t_http_chunked_ext_barelf();
    t_http_close_delimited();
    t_http_redirect();
    t_http_redirect_loop();
    t_http_redirect_no_location();
    t_http_errors();
    t_http_limits();
    t_http_host_port();
    if (failures == 0)
        printf("browser-url-http: ALL PASS\n");
    else
        printf("browser-url-http: %d FAILURES\n", failures);
    return failures != 0;
}
