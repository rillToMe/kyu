// libs/core/netutil.c — reusable userspace network client helpers.
//
// Lapisan tipis di atas sys_* (userlib): parsing literal + resolve + dial.
// Disengaja tanpa malloc/global: aman dipakai app GUI/CLI mana pun.
// Di-link seperti userutil.o (lihat apps/Makefile NETUTIL_OBJ).

#include "netutil.h"
#include "userlib.h"

#define NETUTIL_MAX_HOST 128u

static uint32_t net_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

int net_parse_ipv4(const char *s, uint32_t *out_be) {
    if (!s || !out_be) return -1;
    if (s[0] == '\0') return -1;
    uint32_t parts[4] = { 0, 0, 0, 0 };
    int seg = 0;            // segmen berjalan 0..3
    int digits = 0;         // digit di segmen berjalan
    uint32_t val = 0;
    for (uint32_t i = 0; ; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            if (digits >= 3) return -1;         // >3 digit pasti >255
            val = val * 10u + (uint32_t)(c - '0');
            if (val > 255u) return -1;
            digits++;
        } else if (c == '.' || c == '\0') {
            if (digits == 0) return -1;         // segmen kosong
            if (seg >= 4) return -1;
            parts[seg++] = val;
            val = 0; digits = 0;
            if (c == '\0') break;
        } else {
            return -1;                          // non-digit, non-dot
        }
    }
    if (seg != 4) return -1;
    // Konvensi ip_be KyuzenOS (lihat shell parse_ipv4_): LAYOUT MEMORI (LE)
    // = urutan byte dotted-quad, yaitu o[0] | o[1]<<8 | o[2]<<16 | o[3]<<24.
    // BUKAN nilai big-endian (a<<24|...) — itu tampil terbalik di wire.
    // lwIP ipaddr->addr (sys_resolve) sudah dalam bentuk ini.
    *out_be = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
    return 0;
}

int net_resolve(const char *host, uint32_t *out_be) {
    if (!host || !out_be) return -1;
    if (host[0] == '\0') return -1;
    if (net_strlen(host) > NETUTIL_MAX_HOST) return -1;
    if (net_parse_ipv4(host, out_be) == 0) return 0;   // literal: gratis
    // localhost = loopback, tanpa syscall (RFC 6761; semua OS memetakan
    // ini secara lokal — juga jalan saat link net down).
    if (strcmp(host, "localhost") == 0)
        return net_parse_ipv4("127.0.0.1", out_be);
    return sys_resolve(host, out_be);
}

int net_dial(const char *host, uint16_t port, uint32_t *out_ip_be) {
    uint32_t ip = 0;
    if (net_resolve(host, &ip) != 0) return -1;
    int s = sys_socket();
    if (s < 0) return s;
    if (sys_connect(s, ip, port) != 0) {
        sys_sock_close(s);
        return -1;
    }
    if (out_ip_be) *out_ip_be = ip;
    return s;
}
