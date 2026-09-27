// apps/browser/transport.hpp — http::Transport di atas syscalls KyuzenOS.
#ifndef BROWSER_TRANSPORT_HPP
#define BROWSER_TRANSPORT_HPP

#include "engine/http.hpp"

namespace browser {

// Satu koneksi TCP per fetch-hop (Connection: close). Pemetaan error jujur:
// resolve ECONN -> Dns, ETIMEOUT -> Timeout; connect gagal -> Conn;
// recv ECLOSED (peer tutup, buffer kering) -> closed(0), negatif lain -> -1.
class KyuzenTransport : public http::Transport {
public:
    KyuzenTransport();
    http::OpenError open(const std::string& host, std::uint16_t port) override;
    bool write_all(const char* data, unsigned long len) override;
    long read(char* buf, unsigned long cap) override;
    void close() override;

private:
    int sock_ = -1;
};

// Varian TLS dari KyuzenTransport: TCP sama, lalu handshake BearSSL
// (profil TLS1.2 ECDHE+AES-128-GCM, validasi X.509 penuh, SNI = host).
// open() gagal handshake -> OpenError::Tls + detail jujur via tls_detail()
// (alasan X.509: kedaluwarsa/nama tak cocok/tak dipercaya/...).
// Kegagalan transport SETELAH handshake = Conn (kontrak http::Transport).
class TlsTransport : public http::Transport {
public:
    TlsTransport();
    http::OpenError open(const std::string& host, std::uint16_t port) override;
    bool write_all(const char* data, unsigned long len) override;
    long read(char* buf, unsigned long cap) override;
    void close() override;
    const std::string& tls_detail() const { return detail_; }

private:
    int sock_ = -1;
    std::string detail_;
};

}  // namespace browser

#endif  // BROWSER_TRANSPORT_HPP
