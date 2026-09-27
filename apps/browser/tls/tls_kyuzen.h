// apps/browser/tls/tls_kyuzen.h — TLS client di atas BearSSL + ksock.
//
// C murni, Kyuzen-userspace-safe (hanya userlib.h + header BearSSL, tanpa
// malloc/stdio/time — lihat tls_kyuzen.c). Satu sesi global (browser fetch
// serial di satu thread GUI): struct 33KB tak boleh di stack (stack task
// 8KB) sehingga sesi hidup sebagai static di .c, diakses via tls_session().
#ifndef TLS_KYUZEN_H
#define TLS_KYUZEN_H

#include <stdint.h>

typedef enum {
    TLS_OK = 0,
    TLS_ERR_ARGS,       // argumen buruk
    TLS_ERR_ENTROPY,    // sys_entropy gagal / RDRAND tak ada (jangan fallback!)
    TLS_ERR_TIME,       // RTC tak waras (validasi X.509 butuh jam benar)
    TLS_ERR_HANDSHAKE,  // handshake gagal (lihat tls_engine_error)
    TLS_ERR_IO          // TCP putus/timeout di tengah sesi
} tls_err_t;

// Opaque bagi pemakai; definisi penuh di tls_kyuzen.c.
typedef struct tls_conn tls_conn_t;

// Satu-satunya sesi. BUKAN thread-safe (satu thread GUI, fetch serial).
tls_conn_t *tls_session(void);

// TCP sudah connect (ksock handle). hostname = nama DNS untuk SNI +
// validasi SAN (mis. "example.com"). Return TLS_*.
int tls_connect(tls_conn_t *c, int sock, const char *hostname);

// Tulis SEMUA byte (loop SENDAPP). 0 ok / TLS_ERR_*.
int tls_write_all(tls_conn_t *c, int sock, const uint8_t *data, uint32_t len);

// Baca: >0 byte, 0 = peer tutup bersih, TLS_ERR_* negatif.
// (Return negatif = -(tls_err_t); 0/positif = byte.)
int tls_read(tls_conn_t *c, int sock, uint8_t *buf, uint32_t cap);

// Kirim close_notify (best-effort). Soket ditutup pemanggil.
void tls_close_notify(tls_conn_t *c, int sock);

// Kode error terakhir engine BearSSL (0 = tak ada; 54 expired,
// 56 nama tak cocok, 62 tak dipercaya, ...). Untuk pesan jujur.
int tls_engine_error(const tls_conn_t *c);
const char *tls_error_string(int bearssl_code);

#endif // TLS_KYUZEN_H
