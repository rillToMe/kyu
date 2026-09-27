// apps/browser/tls/tls_kyuzen.c — implementasi tls_kyuzen.h.
//
// Profil: TLS 1.2 saja, ECDHE + AES-128-GCM + SHA256 (suite C02F RSA,
// C02B ECDSA). SENGAJA tanpa CBC/CCM/3DES/ChaCha/TLS1.0-1.1: tiap keluarga
// menambah file + permukaan serangan; GCM modern didukung semua server
// yang diuji (fixture, example.com, info.cern.ch).
//   Entropi: sys_entropy (RDRAND kernel) -> inject ke HMAC-DRBG. Gagal =
//     gagal jujur, TIDAK fallback ke PRNG tertebak.
//   Waktu: sys_get_time (RTC, WIB) -> Unix UTC -> hari/detik BearSSL.
//     RTC ngawur = tolak handshake (lebih baik dari validasi buta).
//   Trust: prebuilt `brssl ta` (tas_https.inc) — tanpa parse PEM runtime.
#include "tls_kyuzen.h"

#include "userlib.h"   // sys_socket/connect/send/recv/close/entropy/get_time
#include "entropy.h"   // ENTROPY_* (kode sys_entropy)

#include "bearssl.h"

#include "tas_https.inc"   // TAs[], TAs_NUM (digenerate: `brssl ta`)

#define TLS_SOCK_CHUNK 32768u   // di bawah UC_MAX_SOCK 64K
#define TLS_SEED_LEN   32u

struct tls_conn {
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    char host[128];
    int last_err;
};

static tls_conn_t g_tls;

tls_conn_t *tls_session(void) {
    return &g_tls;
}

int tls_engine_error(const tls_conn_t *c) {
    return c ? c->last_err : -1;
}

const char *tls_error_string(int code) {
    switch (code) {
    case 0: return "ok";
    case 8: return "no randomness (RDRAND gagal)";
    case 31: return "I/O error";
    case 53: return "waktu tak diketahui (RTC?)";
    case 54: return "sertifikat kedaluwarsa";
    case 56: return "nama server tak cocok SAN";
    case 62: return "rantai tak dipercaya";
    default: return "validasi gagal";
    }
}

// Profil klien minimal (adaptasi ssl_client_full.c BearSSL).
static void tls_profile_init(tls_conn_t *c) {
    static const uint16_t suites[] = {
        BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,    // 0xC02F
        BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,  // 0xC02B
    };

    br_ssl_client_zero(&c->sc);
    br_ssl_engine_set_versions(&c->sc.eng, BR_TLS12, BR_TLS12);
    br_x509_minimal_init(&c->xc, &br_sha256_vtable, TAs, TAs_NUM);
    br_ssl_engine_set_suites(&c->sc.eng, suites,
        (sizeof suites) / (sizeof suites[0]));
    br_ssl_engine_set_default_rsavrfy(&c->sc.eng);
    br_ssl_engine_set_default_ecdsa(&c->sc.eng);
    br_x509_minimal_set_rsa(&c->xc,
        br_ssl_engine_get_rsavrfy(&c->sc.eng));
    br_x509_minimal_set_ecdsa(&c->xc,
        br_ssl_engine_get_ec(&c->sc.eng),
        br_ssl_engine_get_ecdsa(&c->sc.eng));
    br_ssl_engine_set_hash(&c->sc.eng, br_sha256_ID, &br_sha256_vtable);
    br_x509_minimal_set_hash(&c->xc, br_sha256_ID, &br_sha256_vtable);
    br_ssl_engine_set_hash(&c->sc.eng, br_sha1_ID, &br_sha1_vtable);
    br_x509_minimal_set_hash(&c->xc, br_sha1_ID, &br_sha1_vtable);
    // SHA-384/512 HANYA untuk verifikasi tanda tangan sertifikat (rantai
    // ECC modern). PRF/suite tetap SHA-256 (tak perlu prf_sha384.c).
    br_ssl_engine_set_hash(&c->sc.eng, br_sha384_ID, &br_sha384_vtable);
    br_x509_minimal_set_hash(&c->xc, br_sha384_ID, &br_sha384_vtable);
    br_ssl_engine_set_hash(&c->sc.eng, br_sha512_ID, &br_sha512_vtable);
    br_x509_minimal_set_hash(&c->xc, br_sha512_ID, &br_sha512_vtable);
    br_ssl_engine_set_x509(&c->sc.eng, &c->xc.vtable);
    br_ssl_engine_set_prf_sha256(&c->sc.eng, &br_tls12_sha256_prf);
    br_ssl_engine_set_default_aes_gcm(&c->sc.eng);
}

// RTC (WIB) -> detik Unix UTC. days_from_civil ala Hinnant (public domain).
static long tls_civil_days(long y, unsigned m, unsigned d) {
    long era;
    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    {
        unsigned yoe = (unsigned)(y - era * 400);
        unsigned doy = (153u * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + (long)doe - 719468;
    }
}

static int tls_set_time(tls_conn_t *c) {
    uint32_t t[6];   // [year, month, day, hour, min, sec] WIB
    long days, unix;
    unsigned long secs;

    sys_get_time(t);
    if (t[0] < 2020u || t[0] > 2100u || t[1] < 1u || t[1] > 12u ||
        t[2] < 1u || t[2] > 31u || t[3] > 23u || t[4] > 59u || t[5] > 60u)
        return TLS_ERR_TIME;
    days = tls_civil_days((long)t[0], t[1], t[2]);
    unix = days * 86400L + (long)t[3] * 3600L + (long)t[4] * 60L +
        (long)t[5] - 7L * 3600L;   // WIB -> UTC
    if (unix < 0)
        return TLS_ERR_TIME;
    secs = (unsigned long)unix;
    br_x509_minimal_set_time(&c->xc,
        (uint32_t)(secs / 86400UL) + 719528UL, (uint32_t)(secs % 86400UL));
    return TLS_OK;
}

static int tls_seed(tls_conn_t *c) {
    uint8_t seed[TLS_SEED_LEN];
    int n = sys_entropy(seed, sizeof seed);
    if (n != (int)sizeof seed)
        return TLS_ERR_ENTROPY;   // termasuk ENTROPY_ENOHW: tanpa RDRAND
    br_ssl_engine_inject_entropy(&c->sc.eng, seed, sizeof seed);
    return TLS_OK;
}

// Kirim semua record yang menunggu. 0 ok / TLS_ERR_IO.
static int tls_pump_out(tls_conn_t *c, int sock) {
    br_ssl_engine_context *eng = &c->sc.eng;
    for (;;) {
        unsigned st = br_ssl_engine_current_state(eng);
        size_t len;
        unsigned char *b;
        uint32_t off;

        if (st & BR_SSL_CLOSED) {
            c->last_err = (int)br_ssl_engine_last_error(eng);
            return c->last_err ? TLS_ERR_IO : TLS_OK;
        }
        if (!(st & BR_SSL_SENDREC))
            return TLS_OK;
        b = br_ssl_engine_sendrec_buf(eng, &len);
        off = 0;
        while (off < len) {
            uint32_t chunk = (uint32_t)len - off;
            int n;
            if (chunk > TLS_SOCK_CHUNK)
                chunk = TLS_SOCK_CHUNK;
            n = sys_send(sock, b + off, chunk);
            if (n <= 0)
                return TLS_ERR_IO;
            off += (uint32_t)n;
        }
        br_ssl_engine_sendrec_ack(eng, len);
    }
}

int tls_connect(tls_conn_t *c, int sock, const char *hostname) {
    br_ssl_engine_context *eng;
    size_t hn;
    size_t i;
    int r;

    if (!c || sock < 0 || !hostname)
        return TLS_ERR_ARGS;
    hn = strlen(hostname);
    if (hn == 0 || hn >= sizeof c->host)
        return TLS_ERR_ARGS;
    for (i = 0; i <= hn; i++)
        c->host[i] = hostname[i];
    c->last_err = 0;
    tls_profile_init(c);
    br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof c->iobuf, 1);
    r = tls_seed(c);
    if (r != TLS_OK)
        return r;
    r = tls_set_time(c);
    if (r != TLS_OK)
        return r;
    eng = &c->sc.eng;
    br_ssl_client_reset(&c->sc, c->host, 0);
    c->last_err = (int)br_ssl_engine_last_error(eng);
    if (c->last_err)
        return TLS_ERR_HANDSHAKE;
    // Loop ala samples/client_basic.c: kirim bila SENDREC, terima bila
    // bukan; selesai saat SENDAPP (siap data aplikasi).
    for (;;) {
        unsigned st = br_ssl_engine_current_state(eng);
        size_t len;
        unsigned char *b;
        int n;

        if (st & BR_SSL_CLOSED) {
            c->last_err = (int)br_ssl_engine_last_error(eng);
            return TLS_ERR_HANDSHAKE;
        }
        if (st & BR_SSL_SENDAPP)
            return TLS_OK;
        if (st & BR_SSL_SENDREC) {
            r = tls_pump_out(c, sock);
            if (r != TLS_OK)
                return r;
            continue;
        }
        if (!(st & BR_SSL_RECVREC))
            return TLS_ERR_HANDSHAKE;  // state asing saat handshake
        b = br_ssl_engine_recvrec_buf(eng, &len);
        if (len > TLS_SOCK_CHUNK)
            len = TLS_SOCK_CHUNK;
        n = sys_recv(sock, (char *)b, (uint32_t)len);
        if (n <= 0)
            return TLS_ERR_HANDSHAKE;  // tutup/timeout saat handshake
        br_ssl_engine_recvrec_ack(eng, (size_t)n);
    }
}

int tls_write_all(tls_conn_t *c, int sock, const uint8_t *data,
    uint32_t len) {
    br_ssl_engine_context *eng;
    uint32_t off;

    if (!c || sock < 0 || (!data && len > 0))
        return TLS_ERR_ARGS;
    eng = &c->sc.eng;
    off = 0;
    while (off < len) {
        size_t blen;
        unsigned char *b = br_ssl_engine_sendapp_buf(eng, &blen);
        size_t chunk;
        int r;

        if (b == 0) {
            c->last_err = (int)br_ssl_engine_last_error(eng);
            return TLS_ERR_IO;
        }
        chunk = len - off;
        if (chunk > blen)
            chunk = blen;
        {
            size_t i;
            for (i = 0; i < chunk; i++)
                b[i] = data[off + i];
        }
        br_ssl_engine_sendapp_ack(eng, chunk);
        off += (uint32_t)chunk;
        br_ssl_engine_flush(eng, 0);
        r = tls_pump_out(c, sock);
        if (r != TLS_OK)
            return r;
    }
    return TLS_OK;
}

// >0 byte, 0 tutup bersih, negatif -(tls_err_t).
int tls_read(tls_conn_t *c, int sock, uint8_t *buf, uint32_t cap) {
    br_ssl_engine_context *eng;
    int r;

    if (!c || sock < 0 || !buf || cap == 0)
        return -TLS_ERR_ARGS;
    eng = &c->sc.eng;
    for (;;) {
        unsigned st = br_ssl_engine_current_state(eng);
        size_t len;
        unsigned char *b;
        int n;
        size_t i;

        if (st & BR_SSL_CLOSED) {
            c->last_err = (int)br_ssl_engine_last_error(eng);
            return c->last_err ? -TLS_ERR_IO : 0;
        }
        if (st & BR_SSL_RECVAPP) {
            b = br_ssl_engine_recvapp_buf(eng, &len);
            if (len > cap)
                len = cap;
            for (i = 0; i < len; i++)
                buf[i] = b[i];
            br_ssl_engine_recvapp_ack(eng, len);
            return (int)len;
        }
        if (st & BR_SSL_SENDREC) {
            r = tls_pump_out(c, sock);
            if (r != TLS_OK)
                return -r;
            continue;
        }
        if (!(st & BR_SSL_RECVREC)) {
            c->last_err = (int)br_ssl_engine_last_error(eng);
            return c->last_err ? -TLS_ERR_IO : 0;
        }
        b = br_ssl_engine_recvrec_buf(eng, &len);
        if (len > TLS_SOCK_CHUNK)
            len = TLS_SOCK_CHUNK;
        n = sys_recv(sock, (char *)b, (uint32_t)len);
        if (n == 0)
            continue;   // FIN + buffer kering: engine yang menilai
        if (n < 0)
            return -TLS_ERR_IO;
        br_ssl_engine_recvrec_ack(eng, (size_t)n);
    }
}

void tls_close_notify(tls_conn_t *c, int sock) {
    if (!c || sock < 0)
        return;
    br_ssl_engine_close(&c->sc.eng);
    (void)tls_pump_out(c, sock);   // best-effort
}
