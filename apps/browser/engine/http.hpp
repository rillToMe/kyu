#pragma once
// Browser engine HTTP/1.x client — pure C++17, freestanding-safe.
// Transport is injected (host tests script bytes; Kyuzen wires ksock).
// Only <string>/<vector>. No exceptions, no Kyuzen headers.
#include <cstdint>
#include <string>
#include <vector>

#include "url.hpp"

namespace browser {
namespace http {

inline constexpr unsigned long MAX_HEADER_BYTES = 32ul * 1024ul;
inline constexpr unsigned long MAX_HEADERS = 64;
inline constexpr unsigned long MAX_BODY_BYTES = 1024ul * 1024ul;
inline constexpr int MAX_REDIRECTS = 10;

enum class FetchError {
    Ok,
    UnsupportedScheme,  // neither http:// nor https://
    BadUrl,
    Dns,                // resolver said no
    Conn,               // TCP open/write failed
    Tls,                // TLS handshake failed (https only, see detail)
    Timeout,            // read timed out (transport-level)
    Closed,             // peer closed mid-headers (truncated response)
    BadResponse,        // malformed status line / headers / chunk
    TooLarge,           // over header/body caps
    TooManyRedirects,
};

struct Header {
    std::string name;   // lowercased at parse
    std::string value;  // trimmed
};

struct Response {
    int status = 0;
    std::string reason;
    std::vector<Header> headers;
    std::string body;
    std::string final_url;  // serialize() of the URL actually read

    bool get(const std::string& name, std::string& out) const;
};

enum class OpenError { Ok, Dns, Conn, Timeout, Tls };

class Transport {
public:
    virtual ~Transport() {}
    virtual OpenError open(const std::string& host, uint16_t port) = 0;
    virtual bool write_all(const char* data, unsigned long len) = 0;
    // >0 bytes, 0 = peer closed, <0 = timeout/error (mapped to Timeout).
    virtual long read(char* buf, unsigned long cap) = 0;
    virtual void close() = 0;
};

FetchError parse_response(const std::string& raw, Response& out);
FetchError fetch(Transport& t, const Url& url, Response& out);

const char* to_string(FetchError e);

}  // namespace http
}  // namespace browser
