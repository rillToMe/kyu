/**
 * @file kyuzen_netif.c
 * @brief lwIP ↔ Intel e1000 glue layer for Kyuzen OS.
 *
 * Kyuzen OS — Monolithic 64-bit Higher-Half Kernel
 * Toolchain: Clang/LLVM (x86_64-pc-none-elf), -ffreestanding, -mcmodel=kernel
 *
 * Alur data lengkap:
 *
 *   TX (Kernel/lwIP → NIC):
 *     lwIP TCP/IP stack
 *       └─ etharp_output()         ← set sebagai netif->output
 *            └─ ethernet_output()  ← bangun Ethernet header
 *                 └─ low_level_output()  ← kita implement di sini
 *                      └─ e1000_send()  ← driver e1000
 *
 *   RX (NIC → lwIP):
 *     PIT timer callback (setiap ~20ms)
 *       └─ e1000_poll()            ← driver e1000 cek RX ring
 *            └─ e1000_rx_callback() ← kita override di sini (weak symbol)
 *                 └─ kyuzen_netif_input()
 *                      └─ pbuf_alloc() + pbuf_take()
 *                           └─ netif->input() = ethernet_input()
 *                                └─ etharp_input() / ip4_input()
 */

#include <stdint.h>
#include <stddef.h>
#include "kprint.h"

/* Kyuzen OS freestanding string utilities */
#include "../../../include/string.h"

/* e1000 NIC driver
 * Path: drivers/net/port/ → ../ → drivers/net/ → e1000/e1000.h */
#include "../e1000/e1000.h"

/* lwIP core headers */
#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "lwip/snmp.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "netif/ethernet.h"

/* ===========================================================================
 * GLOBAL NETIF — accessible dari kernel_main() untuk net_init()
 * =========================================================================*/

/**
 * g_kyuzen_netif — satu-satunya network interface di Kyuzen OS.
 *
 * Diakses oleh:
 *   - net_init() di kernel.c untuk netif_add() dan dhcp_start()
 *   - e1000_rx_callback() untuk inject paket ke lwIP
 *   - kernel/net_ping.c untuk ICMP ping
 */
struct netif g_kyuzen_netif;

/* Kernel log output */

/* ===========================================================================
 * INTERNAL CONSTANTS
 * =========================================================================*/

#define KYUZEN_NETIF_MTU    1500U   /* Standard Ethernet MTU */
#define KYUZEN_MAX_FRAME    1518U   /* Max frame size: 1500 payload + 14 header + 4 VLAN */

/* ===========================================================================
 * LOW-LEVEL OUTPUT (lwIP → NIC)
 *
 * Dipanggil oleh lwIP setiap kali ada paket yang perlu dikirim.
 * lwIP sudah membangun Ethernet header lengkap — kita tinggal kirim ke NIC.
 * =========================================================================*/

static err_t low_level_output(struct netif *netif, struct pbuf *p) {
    (void)netif;

    /*
     * lwIP bisa memberikan pbuf CHAIN (beberapa segment).
     * Kita assemble dulu ke satu buffer linear karena e1000_send()
     * bekerja dengan pointer tunggal.
     *
     * Stack-allocated: 1518 byte aman di kernel stack (non-ISR context,
     * dan kita sudah -mno-red-zone).
     */
    uint8_t  frame[KYUZEN_MAX_FRAME];
    uint16_t total = 0;

    for (struct pbuf *q = p; q != NULL; q = q->next) {
        if ((uint32_t)total + q->len > KYUZEN_MAX_FRAME) {
            /* Frame terlalu besar — seharusnya tidak terjadi dengan MTU yang benar */
            LINK_STATS_INC(link.lenerr);
            return ERR_IF;
        }
        memcpy(frame + total, q->payload, q->len);
        total = (uint16_t)(total + q->len);
    }

    /* Pad ke minimum 60 byte (hardware e1000 juga melakukan ini via TCTL_PSP,
     * tapi kita lakukan di sini juga sebagai safety net) */
    if (total < 60U) {
        memset(frame + total, 0, 60U - total);
        total = 60U;
    }

    /* Kirim ke NIC via e1000 TX ring */
    if (e1000_send(frame, total) != 0) {
        MIB2_STATS_NETIF_INC(netif, ifouterrors);
        LINK_STATS_INC(link.drop);
        return ERR_IF;
    }

    /* Update lwIP stats */
    LINK_STATS_INC(link.xmit);
    MIB2_STATS_NETIF_ADD(netif, ifoutoctets, total);

    return ERR_OK;
}

/* ===========================================================================
 * PACKET INPUT  (NIC → lwIP)
 *
 * Dipanggil oleh e1000_rx_callback() di bawah setiap kali NIC menerima paket.
 * =========================================================================*/

static void kyuzen_netif_input(struct netif *netif,
                                const uint8_t *data, uint16_t length) {
    if (data == NULL || length == 0U || length > KYUZEN_MAX_FRAME) {
        LINK_STATS_INC(link.lenerr);
        return;
    }

    /*
     * Alokasikan pbuf dari pool.
     * PBUF_POOL: zero-copy friendly, cocok untuk RX path.
     * PBUF_RAW: tidak ada header room (kita berikan raw Ethernet frame).
     */
    struct pbuf *p = pbuf_alloc(PBUF_RAW, length, PBUF_POOL);
    if (p == NULL) {
        /* Pool habis — drop paket */
        LINK_STATS_INC(link.drop);
        MIB2_STATS_NETIF_INC(netif, ifindiscards);
        return;
    }

    /*
     * Salin data dari DMA buffer e1000 ke pbuf.
     * pbuf_take() handle pbuf chain secara otomatis.
     */
    if (pbuf_take(p, data, length) != ERR_OK) {
        pbuf_free(p);
        LINK_STATS_INC(link.drop);
        return;
    }

    /* Update stats */
    LINK_STATS_INC(link.recv);
    MIB2_STATS_NETIF_ADD(netif, ifinoctets, length);

    /*
     * Inject ke lwIP.
     * netif->input = ethernet_input (di-set oleh netif_add di net_init).
     * ethernet_input() free pbuf setelah selesai — jangan free lagi di sini.
     */
    if (netif->input(p, netif) != ERR_OK) {
        LINK_STATS_INC(link.drop);
        /* Tidak perlu pbuf_free() — ethernet_input() sudah handle ini */
    }
}

/* ===========================================================================
 * e1000_rx_callback() — Override weak symbol dari e1000.c
 *
 * e1000.c mendefinisikan:
 *   void e1000_rx_callback(void *data, uint16_t len) __attribute__((weak));
 *
 * Kita override di sini dengan definisi non-weak yang menghubungkan
 * paket dari NIC langsung ke kyuzen_netif_input() → lwIP stack.
 *
 * Dipanggil dari e1000_poll() (via cb_network timer callback, ~setiap 20ms).
 * =========================================================================*/

void e1000_rx_callback(void *data, uint16_t len) {
    /*
     * g_kyuzen_netif harus sudah diinisialisasi via net_init() sebelum
     * paket pertama tiba. Cek flags sebagai safety guard.
     */
    if (!(g_kyuzen_netif.flags & NETIF_FLAG_UP)) {
        /* netif belum up — drop paket diam-diam */
        return;
    }
    kyuzen_netif_input(&g_kyuzen_netif, (const uint8_t *)data, len);
}

/* ===========================================================================
 * LOW-LEVEL INIT — dipanggil saat netif_add()
 * =========================================================================*/

static err_t low_level_init(struct netif *netif) {
    /* 1. Inisialisasi hardware e1000 */
    if (e1000_init() != 0) {
        kprint("[kyuzen_netif] ERROR: e1000_init() failed!\n");
        return ERR_IF;
    }

    /* 2. Baca MAC address dari e1000 EEPROM */
    netif->hwaddr_len = ETH_HWADDR_LEN;  /* 6 */
    e1000_get_mac(netif->hwaddr);

    /* 3. Set MTU */
    netif->mtu = KYUZEN_NETIF_MTU;

    /* 4. Capabilities flags:
     *   BROADCAST: mendukung broadcast (wajib untuk ARP)
     *   ETHARP:    ARP over Ethernet
     *   ETHERNET:  ini adalah Ethernet interface
     *   LINK_UP:   link dianggap up setelah e1000_init() sukses
     *              (link state sebenarnya bisa dicek via e1000_link_up())
     */
    netif->flags = NETIF_FLAG_BROADCAST |
                   NETIF_FLAG_ETHARP    |
                   NETIF_FLAG_ETHERNET  |
                   NETIF_FLAG_LINK_UP;

    kprint("[kyuzen_netif] Hardware initialized, interface 'kz' ready\n");
    return ERR_OK;
}

/* ===========================================================================
 * NETIF INIT CALLBACK — entry point, dipanggil oleh netif_add()
 * =========================================================================*/

/**
 * kyuzen_netif_init() — callback yang diberikan ke netif_add().
 *
 * Cara penggunaan dari net_init() di kernel.c:
 *
 *   ip4_addr_t ip = {0}, mask = {0}, gw = {0};  // DHCP: semua 0
 *   netif_add(&g_kyuzen_netif, &ip, &mask, &gw,
 *             NULL, kyuzen_netif_init, ethernet_input);
 *   netif_set_default(&g_kyuzen_netif);
 *   netif_set_up(&g_kyuzen_netif);
 *   dhcp_start(&g_kyuzen_netif);
 */
err_t kyuzen_netif_init(struct netif *netif) {
    /* Nama interface: "kz" — terlihat di LWIP_DEBUGF output */
    netif->name[0] = 'k';
    netif->name[1] = 'z';

    /*
     * Dua output callback:
     *   output:     dipanggil lwIP IP layer, membangun ARP jika perlu
     *   linkoutput: dipanggil setelah ARP/Ethernet header selesai,
     *               menyerahkan frame ke hardware
     */
    netif->output     = etharp_output;
    netif->linkoutput = low_level_output;

    /* Inisialisasi hardware */
    return low_level_init(netif);
}
