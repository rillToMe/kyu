// apps/browser/transport_tls.cpp — http::Transport di atas TLS (BearSSL).
#include "transport.hpp"

#include "platform.hpp"
#include "engine/strutil.hpp"

extern "C" {
#include "tls/tls_kyuzen.h"
}

namespace browser {

TlsTransport::TlsTransport() : sock_(-1) {}

http::OpenError TlsTransport::open(const std::string& host, std::uint16_t port) {
    close();
    detail_.clear();
    std::uint32_t ip = 0;
    int rc = net_resolve(host.c_str(), &ip);
    if (rc != 0) {
        if (rc == KSOCK_ETIMEOUT) return http::OpenError::Timeout;
        return http::OpenError::Dns;
    }
    int s = sys_socket();
    if (s < 0) return http::OpenError::Conn;
    if (sys_connect(s, ip, port) != 0) {
        sys_sock_close(s);
        return http::OpenError::Conn;
    }
    int tr = tls_connect(tls_session(), s, host.c_str());
    if (tr != TLS_OK) {
        // Kode numerik BearSSL selalu dicantumkan (mudah dipetakan ke
        // bearssl_ssl.h: 54 expired, 56 nama, 62 tak dipercaya, ...).
        int ec = tls_engine_error(tls_session());
        detail_ = "TLS handshake failed (code ";
        if (ec < 0) {
            detail_ += '-';
            detail::append_ulong(detail_, (unsigned long)(-(long)ec));
        } else {
            detail::append_ulong(detail_, (unsigned long)ec);
        }
        detail_ += ": ";
        detail_ += tls_error_string(ec);
        detail_ += ")";
        if (tr == TLS_ERR_ENTROPY)
            detail_ = "TLS aborted: no entropy (CPU lacks RDRAND)";
        else if (tr == TLS_ERR_TIME)
            detail_ = "TLS aborted: RTC clock not sane";
        sys_sock_close(s);
        return http::OpenError::Tls;
    }
    sock_ = s;
    return http::OpenError::Ok;
}

bool TlsTransport::write_all(const char* data, unsigned long len) {
    if (sock_ < 0 || !data) return false;
    unsigned long off = 0;
    while (off < len) {
        // tls_write_all menulis semua; loop di sini untuk cap uint32_t.
        unsigned long chunk = len - off;
        if (chunk > 32768) chunk = 32768;
        int r = tls_write_all(tls_session(), sock_,
            reinterpret_cast<const std::uint8_t*>(data + off),
            (std::uint32_t)chunk);
        if (r != TLS_OK) return false;
        off += chunk;
    }
    return true;
}

long TlsTransport::read(char* buf, unsigned long cap) {
    if (sock_ < 0 || !buf || cap == 0) return -1;
    if (cap > 32768) cap = 32768;
    int n = tls_read(tls_session(), sock_, reinterpret_cast<std::uint8_t*>(buf),
        (std::uint32_t)cap);
    if (n == 0) return 0;   // close_notify / tutup bersih
    if (n < 0) return -1;   // fetch: Timeout
    return (long)n;
}

void TlsTransport::close() {
    if (sock_ >= 0) {
        tls_close_notify(tls_session(), sock_);
        sys_sock_close(sock_);
        sock_ = -1;
    }
}

}  // namespace browser
