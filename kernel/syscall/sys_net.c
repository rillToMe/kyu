#include "syscall.h"
#include <stdint.h>
#include "task.h"
#include "usercopy.h"
#include "net_socket.h"
#include "net_dns.h"
#include "heap.h"

// kernel_ping (kernel/net/net_ping.c) tidak punya owner header (net_socket.h
// hanya TCP client) dan hanya dipakai satu TU di sini — tetap lokal; membuat
// header baru untuk satu function adalah overkill di phase ini.
extern int kernel_ping(const char *host);

int sys_net_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st) {
    uint64_t syscall_num = r->rax;

    // Socket ownership identity: the trapping task's (id, cookie). NULL st
    // (idle/early-boot, no task context) owns nothing -> socket ops fail
    // with KSOCK_EOWNER instead of creating owner=nobody sockets.
    int caller_pid = st ? (int)st->id : -1;
    uint32_t caller_cookie = st ? st->cookie : 0;

    if (syscall_num == 41) { // sys_ping
        // RBX = const char* host (user-space pointer ke string hostname/IP)
        // Return: RTT dalam ms (>=0) jika berhasil, -1 jika timeout/error
        // Tahap 2: copy-in dulu — string lama dipakai lintas preemption
        // berdetik-detik oleh kernel_ping (TOCTOU tertutup).
        char khost[UC_MAX_HOST];
        int rtt = -1;
        if (strncpy_from_user(uc, khost, r->rbx, sizeof(khost)) > 0) {
            rtt = kernel_ping(khost);
        }
        *ret = (uint64_t)(int64_t)rtt; // sign-extend -1 dengan benar
    }
    else if (syscall_num == 52) { // sys_socket() -> handle or negative KSOCK_* err
        *ret = (uint64_t)(int64_t)ksock_socket(caller_pid, caller_cookie);
    }
    else if (syscall_num == 53) { // sys_connect(handle, ip_be, port)
        *ret = (uint64_t)(int64_t)ksock_connect((int)r->rbx, (uint32_t)r->rcx, (uint16_t)r->rdx,
                                               caller_pid, caller_cookie);
    }
    else if (syscall_num == 54) { // sys_sock_send(handle, buf, len)
        // Tahap 2: copy-in ke bounce — buffer lama dibaca berulang lintas
        // preemption sampai 5 detik oleh ksock_send (TOCTOU tertutup).
        uint32_t len = (uint32_t)r->rdx;
        int n = KSOCK_ERR;
        if (len == 0) {
            n = ksock_send((int)r->rbx, NULL, 0, caller_pid, caller_cookie);   // semantik lama: 0
        } else if (len <= UC_MAX_SOCK && user_range_ok(uc, r->rcx, len)) {
            uint8_t* bounce = (uint8_t*)kmalloc(len);
            if (bounce) {
                copy_from_user(uc, bounce, r->rcx, len); // range sudah valid
                n = ksock_send((int)r->rbx, bounce, len, caller_pid, caller_cookie);
                kfree(bounce);
            } else {
                n = KSOCK_ENOMEM;
            }
        }
        *ret = (uint64_t)(int64_t)n;
    }
    else if (syscall_num == 55) { // sys_sock_recv(handle, buf, len)
        // Tahap 2: validasi out-range SEBELUM data ring dikonsumsi; terima
        // ke bounce kernel, copy-out di luar net_lock.
        uint32_t len = (uint32_t)r->rdx;
        int n = KSOCK_ERR;
        if (len == 0) {
            n = ksock_recv((int)r->rbx, NULL, 0, caller_pid, caller_cookie);   // semantik lama: 0
        } else {
            if (len > UC_MAX_SOCK) len = UC_MAX_SOCK;  // recv partial itu sah
            if (user_range_ok(uc, r->rcx, len)) {
                uint8_t* bounce = (uint8_t*)kmalloc(len);
                if (bounce) {
                    n = ksock_recv((int)r->rbx, bounce, len, caller_pid, caller_cookie);
                    if (n > 0) copy_to_user(uc, r->rcx, bounce, (uint32_t)n);
                    kfree(bounce);
                } else {
                    n = KSOCK_ENOMEM;
                }
            }
        }
        *ret = (uint64_t)(int64_t)n;
    }
    else if (syscall_num == 56) { // sys_sock_close(handle)
        *ret = (uint64_t)(int64_t)ksock_close((int)r->rbx, caller_pid, caller_cookie);
    }
    else if (syscall_num == SYS_RESOLVE) { // sys_resolve(host*, out_ip_be*)
        // Tahap 2: copy-in hostname (bounded UC_MAX_HOST), validasi out-range
        // SEBELUM resolve; hasil ke kernel var, copy-out sesudahnya.
        int n = KSOCK_ERR;
        char khost[UC_MAX_HOST];
        if (strncpy_from_user(uc, khost, r->rbx, sizeof(khost)) > 0 &&
            user_range_ok(uc, r->rcx, sizeof(uint32_t))) {
            uint32_t ip = 0;
            n = net_dns_resolve(khost, &ip);
            if (n == KSOCK_OK) copy_to_user(uc, r->rcx, &ip, sizeof(ip));
        }
        *ret = (uint64_t)(int64_t)n;
    }

    return 0;
}
