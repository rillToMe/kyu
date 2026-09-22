#include "syscall.h"
#include <stdint.h>
#include "task.h"
#include "usercopy.h"
#include "net_socket.h"
#include "heap.h"

// kernel_ping (kernel/net/net_ping.c) tidak punya owner header (net_socket.h
// hanya TCP client) dan hanya dipakai satu TU di sini — tetap lokal; membuat
// header baru untuk satu function adalah overkill di phase ini.
extern int kernel_ping(const char *host);

int sys_net_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st) {
    (void)st;
    uint64_t syscall_num = r->rax;

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
    else if (syscall_num == 52) { // sys_socket() -> sockfd
        *ret = (uint64_t)(int64_t)ksock_socket();
    }
    else if (syscall_num == 53) { // sys_connect(sockfd, ip_be, port)
        *ret = (uint64_t)(int64_t)ksock_connect((int)r->rbx, (uint32_t)r->rcx, (uint16_t)r->rdx);
    }
    else if (syscall_num == 54) { // sys_sock_send(sockfd, buf, len)
        // Tahap 2: copy-in ke bounce — buffer lama dibaca berulang lintas
        // preemption sampai 5 detik oleh ksock_send (TOCTOU tertutup).
        uint32_t len = (uint32_t)r->rdx;
        int n = -1;
        if (len == 0) {
            n = ksock_send((int)r->rbx, NULL, 0);   // semantik lama: 0
        } else if (len <= UC_MAX_SOCK && user_range_ok(uc, r->rcx, len)) {
            uint8_t* bounce = (uint8_t*)kmalloc(len);
            if (bounce) {
                copy_from_user(uc, bounce, r->rcx, len); // range sudah valid
                n = ksock_send((int)r->rbx, bounce, len);
                kfree(bounce);
            }
        }
        *ret = (uint64_t)(int64_t)n;
    }
    else if (syscall_num == 55) { // sys_sock_recv(sockfd, buf, len)
        // Tahap 2: validasi out-range SEBELUM data ring dikonsumsi; terima
        // ke bounce kernel, copy-out di luar net_lock.
        uint32_t len = (uint32_t)r->rdx;
        int n = -1;
        if (len == 0) {
            n = ksock_recv((int)r->rbx, NULL, 0);   // semantik lama: 0
        } else {
            if (len > UC_MAX_SOCK) len = UC_MAX_SOCK;  // recv partial itu sah
            if (user_range_ok(uc, r->rcx, len)) {
                uint8_t* bounce = (uint8_t*)kmalloc(len);
                if (bounce) {
                    n = ksock_recv((int)r->rbx, bounce, len);
                    if (n > 0) copy_to_user(uc, r->rcx, bounce, (uint32_t)n);
                    kfree(bounce);
                }
            }
        }
        *ret = (uint64_t)(int64_t)n;
    }
    else if (syscall_num == 56) { // sys_sock_close(sockfd)
        *ret = (uint64_t)(int64_t)ksock_close((int)r->rbx);
    }

    return 0;
}
