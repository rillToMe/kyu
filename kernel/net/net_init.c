/**
 * @file net_init.c
 * @brief Kernel network stack initialization — Kyuzen OS.
 *
 * Menginisialisasi seluruh network subsystem:
 *   1. lwIP core (lwip_init)
 *   2. Network interface (netif_add → kyuzen_netif_init → e1000_init)
 *   3. DHCP client (dhcp_start)
 *   4. DNS resolver setup
 *
 * Dipanggil dari kernel_main() SETELAH `sti` (interrupts enabled),
 * karena DHCP timer membutuhkan PIT tick yang berjalan.
 *
 * Kompilasi dengan LWIP_CFLAGS (butuh lwIP + port headers):
 *   clang -target x86_64-pc-none-elf -ffreestanding -mcmodel=kernel
 *         -Idrivers/net/lwip/src/include -Idrivers/net/lwip/port
 *         -std=c11 -O2 -c kernel/net_init.c
 */

/* lwIP headers */
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

/* kyuzen_netif glue layer + g_kyuzen_netif */
#include "kyuzen_netif.h"

/* Kernel log */
extern void kprint(const char *str);
extern void kprint_num(uint64_t num);

/* Timer — untuk DHCP wait loop */
extern uint64_t timer_get_ms(void);

/* ===========================================================================
 * INTERNAL HELPERS
 * =========================================================================*/

/**
 * net_print_ip() — cetak IPv4 address dalam format x.x.x.x ke kprint.
 * Manual karena tidak ada printf/snprintf di freestanding kernel.
 */
static void net_print_ip(uint32_t addr_le) {
    /* lwIP menyimpan IP dalam network byte order (big-endian).
     * Di x86 (little-endian), byte pertama di memori = oktet pertama IP. */
    uint8_t a = (uint8_t)(addr_le & 0xFF);
    uint8_t b = (uint8_t)((addr_le >> 8) & 0xFF);
    uint8_t c = (uint8_t)((addr_le >> 16) & 0xFF);
    uint8_t d = (uint8_t)((addr_le >> 24) & 0xFF);
    kprint_num(a); kprint(".");
    kprint_num(b); kprint(".");
    kprint_num(c); kprint(".");
    kprint_num(d);
}

/* ===========================================================================
 * Boot-console summary state.
 *
 * net_init() sendiri tidak boleh mencetak verbose ke console (kernel_main
 * menahan kprint ke serial selama boot). State ringkas ini dibaca kernel_main
 * untuk menampilkan satu baris status yang jujur: down / DHCP / static.
 *   return: 0 = NIC gagal, 1 = DHCP, 2 = static fallback
 * =========================================================================*/
static int      g_boot_net_state = 0;
static uint32_t g_boot_ip = 0;

int net_boot_summary(uint32_t* ip_out) {
    if (ip_out) *ip_out = g_boot_ip;
    return g_boot_net_state;
}

/* ===========================================================================
 * net_init() — Entry Point
 *
 * Dipanggil dari kernel_main() setelah `sti`.
 * =========================================================================*/

void net_init(void) {
    kprint("\n[net] Initializing network stack...\n");

    /* -----------------------------------------------------------------------
     * 1. Inisialisasi lwIP core.
     *    lwip_init() setup semua internal pool (memp, mem, pbuf), dan
     *    mendaftarkan semua timer internal (ARP, TCP, DHCP, DNS).
     * ----------------------------------------------------------------------- */
    lwip_init();
    kprint("[net] lwIP initialized\n");

    /* -----------------------------------------------------------------------
     * 2. Daftarkan network interface ke lwIP.
     *
     * IP 0.0.0.0 / mask 0.0.0.0 / gw 0.0.0.0 → minta DHCP assign semuanya.
     *
     * netif_add() memanggil kyuzen_netif_init() yang di dalamnya:
     *   - e1000_init()     : scan PCI, init MMIO, TX/RX ring
     *   - e1000_get_mac()  : baca MAC dari EEPROM → isi netif->hwaddr
     *   - set flags        : BROADCAST | ETHARP | ETHERNET | LINK_UP
     *   - set callbacks    : output=etharp_output, linkoutput=low_level_output
     * ----------------------------------------------------------------------- */
    ip4_addr_t ip, mask, gw;
    ip4_addr_set_zero(&ip);
    ip4_addr_set_zero(&mask);
    ip4_addr_set_zero(&gw);

    struct netif *netif = netif_add(
        &g_kyuzen_netif,        /* netif struct (global di kyuzen_netif.c) */
        &ip, &mask, &gw,        /* IP/mask/gw semua 0 → DHCP akan isi ini */
        NULL,                   /* state — tidak dipakai */
        kyuzen_netif_init,      /* init callback → inisialisasi e1000 + MAC */
        ethernet_input          /* input callback → dipanggil saat RX */
    );

    if (netif == NULL) {
        kprint("[net] ERROR: netif_add() failed! NIC not found or init error.\n");
        g_boot_net_state = 0;   /* NIC gagal — boot console harus melaporkan FAIL */
        return;
    }

    /* Jadikan interface ini sebagai default (untuk routing) */
    netif_set_default(&g_kyuzen_netif);

    /* Tandai interface sebagai UP — mulai menerima paket */
    netif_set_up(&g_kyuzen_netif);

    /* Set hostname (tampil di DHCP request option 12) */
    netif_set_hostname(&g_kyuzen_netif, "kyuzen");

    /* Interface terdaftar; default static sampai DHCP membuktikan sebaliknya. */
    g_boot_net_state = 2;

    kprint("[net] Network interface 'kz' registered and up\n");

    /* -----------------------------------------------------------------------
     * 3. Start DHCP client.
     *
     * dhcp_start() mengirim DHCP DISCOVER broadcast ke jaringan.
     * QEMU -nic user menjawab dengan DHCP OFFER → DHCP REQUEST → DHCP ACK.
     * Setelah ACK, lwIP mengisi netif->ip_addr, netif->netmask, netif->gw.
     *
     * Proses ini ASYNCHRONOUS — dhcp_start() langsung return.
     * DHCP selesai dalam beberapa detik ketika:
     *   - e1000_poll() menerima paket DHCP OFFER/ACK dari NIC
     *   - sys_check_timeouts() memproses DHCP state machine
     * ----------------------------------------------------------------------- */
    err_t dhcp_err = dhcp_start(&g_kyuzen_netif);
    if (dhcp_err != ERR_OK) {
        kprint("[net] WARNING: dhcp_start() failed (err=");
        kprint_num((uint64_t)(uint8_t)dhcp_err);
        kprint("), falling back to static IP 10.0.2.15\n");

        /* Fallback: static IP untuk QEMU -nic user
         * QEMU selalu assign 10.0.2.15 untuk guest pertama */
        IP4_ADDR(&ip,   10, 0, 2, 15);
        IP4_ADDR(&mask, 255, 255, 255, 0);
        IP4_ADDR(&gw,   10, 0, 2,  2);
        netif_set_addr(&g_kyuzen_netif, &ip, &mask, &gw);
        netif_set_link_up(&g_kyuzen_netif);
    } else {
        kprint("[net] DHCP started — waiting for IP assignment...\n");

        /* Tunggu DHCP selesai dengan polling loop.
         * Batas waktu: 8 detik (QEMU DHCP biasanya < 500ms).
         *
         * Loop ini aman karena:
         *   - `sti` sudah dipanggil → PIT IRQ berjalan
         *   - cb_network dipanggil dari timer → e1000_poll() + sys_check_timeouts()
         *     berjalan di dalam interrupt context
         *   - `hlt` hemat CPU saat menunggu interrupt berikutnya
         */
        uint64_t deadline = timer_get_ms() + 8000; /* 8 detik timeout */
        while (timer_get_ms() < deadline) {
            /* Cek apakah DHCP sudah berhasil mendapatkan IP */
            if (!ip4_addr_isany_val(g_kyuzen_netif.ip_addr)) {
                kprint("[net] DHCP complete!\n");
                g_boot_net_state = 1;   /* DHCP sukses */
                break;
            }
            /* Hemat CPU: tidur hingga interrupt berikutnya.
             * sti WAJIB sebelum hlt — loop ini dipanggil dari kernel_main
             * (IF sudah enabled), tapi kita jaga konsisten dengan pola di
             * net_ping.c: tanpa sti, jika IF sempat 0 loop bisa deadlock. */
            __asm__ volatile("sti\n\t hlt" ::: "memory");
        }

        if (ip4_addr_isany_val(g_kyuzen_netif.ip_addr)) {
            kprint("[net] DHCP timeout! Menggunakan static IP fallback...\n");
            kprint("[net] (QEMU -nic user selalu assign 10.0.2.15 ke guest)\n");

            /* Stop DHCP state machine agar tidak konflik dengan static IP */
            dhcp_stop(&g_kyuzen_netif);

            /* Fallback: static IP sesuai QEMU user networking default
             *   Guest : 10.0.2.15
             *   Gateway: 10.0.2.2  (juga DHCP server, DNS forwarder)
             *   Mask   : 255.255.255.0
             *
             * Dengan IP ini, kita bisa ping 10.0.2.2 (gateway) dan
             * host internet via slirp NAT QEMU.
             */
            ip4_addr_t fallback_ip, fallback_mask, fallback_gw;
            IP4_ADDR(&fallback_ip,   10, 0, 2, 15);
            IP4_ADDR(&fallback_mask, 255, 255, 255, 0);
            IP4_ADDR(&fallback_gw,   10, 0, 2, 2);

            netif_set_addr(&g_kyuzen_netif,
                           &fallback_ip, &fallback_mask, &fallback_gw);
        }
    }

    /* -----------------------------------------------------------------------
     * 4. Setup DNS.
     *
     * QEMU -nic user secara otomatis memforward DNS ke 8.8.8.8.
     * DNS server biasanya di-set otomatis oleh DHCP OPTION 6 (DNS).
     * Tapi kita set manual juga sebagai fallback.
     * ----------------------------------------------------------------------- */
#if LWIP_DNS
    ip4_addr_t dns1, dns2;
    /* Primary: Google DNS (QEMU forward ke ini) */
    IP4_ADDR(&dns1, 8, 8, 8, 8);
    /* Secondary: Cloudflare DNS */
    IP4_ADDR(&dns2, 1, 1, 1, 1);
    dns_setserver(0, &dns1);
    dns_setserver(1, &dns2);
    kprint("[net] DNS configured: 8.8.8.8 (primary), 1.1.1.1 (secondary)\n");
#endif

    /* -----------------------------------------------------------------------
     * 5. Print network configuration summary
     * ----------------------------------------------------------------------- */
    kprint("[net] === Network Configuration ===\n");

    kprint("[net] IP   : ");
    net_print_ip(g_kyuzen_netif.ip_addr.addr);
    kprint("\n");

    kprint("[net] Mask : ");
    net_print_ip(g_kyuzen_netif.netmask.addr);
    kprint("\n");

    kprint("[net] GW   : ");
    net_print_ip(g_kyuzen_netif.gw.addr);
    kprint("\n");

    kprint("[net] MAC  : ");
    for (int i = 0; i < 6; i++) {
        uint8_t b_mac = g_kyuzen_netif.hwaddr[i];
        uint8_t hi = (b_mac >> 4) & 0xF;
        uint8_t lo =  b_mac       & 0xF;
        char hbuf[2] = { (char)(hi < 10 ? '0'+hi : 'a'+hi-10), '\0' };
        char lbuf[2] = { (char)(lo < 10 ? '0'+lo : 'a'+lo-10), '\0' };
        kprint(hbuf); kprint(lbuf);
        if (i < 5) kprint(":");
    }
    kprint("\n");
    kprint("[net] ================================\n\n");

    /* Ringkasan untuk boot console (kernel_main mencetak satu baris status). */
    g_boot_ip = g_kyuzen_netif.ip_addr.addr;
    if (g_boot_net_state == 0) g_boot_net_state = 2;   /* netif up, IP static */
}
