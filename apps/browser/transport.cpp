// apps/browser/transport.cpp — implementasi Transport via netutil/sys_*.
#include "transport.hpp"

#include "platform.hpp"

namespace browser {

KyuzenTransport::KyuzenTransport() : sock_(-1) {}

http::OpenError KyuzenTransport::open(const std::string& host, std::uint16_t port) {
    close();
    std::uint32_t ip = 0;
    int rc = net_resolve(host.c_str(), &ip);
    if (rc != 0) {
        if (rc == KSOCK_ETIMEOUT) return http::OpenError::Timeout;
        // ECONN (jawaban negatif) + lainnya = nama tak terresolve.
        return http::OpenError::Dns;
    }
    int s = sys_socket();
    if (s < 0) return http::OpenError::Conn;
    if (sys_connect(s, ip, port) != 0) {
        sys_sock_close(s);
        return http::OpenError::Conn;
    }
    sock_ = s;
    return http::OpenError::Ok;
}

bool KyuzenTransport::write_all(const char* data, unsigned long len) {
    if (sock_ < 0 || !data) return false;
    unsigned long off = 0;
    while (off < len) {
        // ksock partial-ok: ulangi sampai habis. Cap per send 32K (di bawah
        // UC_MAX_SOCK 64K, aman untuk bounce kernel).
        unsigned long chunk = len - off;
        if (chunk > 32768) chunk = 32768;
        int n = sys_send(sock_, data + off, (std::uint32_t)chunk);
        if (n <= 0) return false;
        off += (unsigned long)n;
    }
    return true;
}

long KyuzenTransport::read(char* buf, unsigned long cap) {
    if (sock_ < 0 || !buf || cap == 0) return -1;
    if (cap > 32768) cap = 32768;
    int n = sys_recv(sock_, buf, (std::uint32_t)cap);
    if (n == 0) return 0;
    if (n == KSOCK_ECLOSED) return 0;  // peer tutup, buffer kering
    if (n < 0) return -1;
    return (long)n;
}

void KyuzenTransport::close() {
    if (sock_ >= 0) {
        sys_sock_close(sock_);
        sock_ = -1;
    }
}

}  // namespace browser
