#ifndef NETUTIL_H
#define NETUTIL_H

#include <stdint.h>

// netutil — reusable userspace network client helpers (C, freestanding).
// dipakai browser + app lain: tanpa malloc, tanpa global state, tanpa thread.
// Error: 0 sukses; negatif = KSOCK_* dari syscall (net_socket.h) atau -1 generik.

// IPv4 dotted-quad ketat ("10.0.2.2" -> ip_be).
// Konvensi ip_be KyuzenOS (lihat shell parse_ipv4_): LAYOUT MEMORI (LE) =
// urutan byte dotted-quad (o[0] | o[1]<<8 | o[2]<<16 | o[3]<<24), BUKAN
// nilai big-endian. out_be siap untuk sys_connect/lwIP apa adanya.
// Menolak: NULL/kosong/segmen kosong, non-digit, >3 digit, nilai >255,
// jumlah segmen != 4, trailing/leading aneh ("1.2.3.4." ditolak).
// 0 ok (*out_be diisi) / -1 bukan literal valid.
int net_parse_ipv4(const char *s, uint32_t *out_be);

// hostname -> IPv4. Literal dulu (tanpa syscall), lalu sys_resolve(86).
// host NULL/kosong/>128 char -> -1. Return 0 / negatif KSOCK_*.
int net_resolve(const char *host, uint32_t *out_be);

// Dial lengkap: resolve + socket + connect.
// Return sockfd (>=0) / negatif (resolve/socket/connect gagal; socket
// yang sempat terbuka SELALU ditutup — tidak bocor). out_ip boleh NULL.
int net_dial(const char *host, uint16_t port, uint32_t *out_ip_be);

#endif // NETUTIL_H
