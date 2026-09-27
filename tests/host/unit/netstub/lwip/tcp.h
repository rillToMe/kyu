// HOST-TEST FAKE lwip/tcp.h — controllable PCB double for net_socket.c.
// The stub records callback registration + close/abort/recved so the test
// can fire callbacks deterministically like cb_network would.
#ifndef NETSTUB_TCP_H
#define NETSTUB_TCP_H

#include <stdint.h>
#include "err.h"
#include "ip_addr.h"

// File-scope forward declarations FIRST: without these, the struct tags
// inside the callback typedefs below would get function-prototype scope
// (a distinct, immediately-dying type per typedef) and every later use
// of `struct tcp_pcb *` would be an incompatible type.
struct tcp_pcb;
struct pbuf;

typedef err_t (*tcp_recv_fn)(void *arg, struct tcp_pcb *pcb,
                             struct pbuf *p, err_t err);
typedef err_t (*tcp_connected_fn)(void *arg, struct tcp_pcb *pcb, err_t err);
typedef void (*tcp_err_fn)(void *arg, err_t err);

struct tcp_pcb {
    void            *cb_arg;
    tcp_recv_fn      recv_cb;
    tcp_connected_fn connected_cb;
    tcp_err_fn       err_cb;
    uint32_t         sndbuf_space;   // stub-controlled tcp_sndbuf()
    uint32_t         recved_total;   // accumulated tcp_recved() bytes (= ACKs)
    uint32_t         sent_total;     // accumulated tcp_write() bytes
    int              closed;         // tcp_close called
    int              aborted;        // tcp_abort called
    int              freed;          // no more callbacks allowed
};

#define TCP_WRITE_FLAG_COPY 0x01

struct tcp_pcb *tcp_new(void);
err_t tcp_connect(struct tcp_pcb *pcb, const ip_addr_t *addr, uint16_t port,
                  tcp_connected_fn connected);
err_t tcp_write(struct tcp_pcb *pcb, const void *dataptr, uint16_t len,
                uint8_t apiflags);
err_t tcp_output(struct tcp_pcb *pcb);
uint32_t tcp_sndbuf(struct tcp_pcb *pcb);
void tcp_recved(struct tcp_pcb *pcb, uint16_t len);
void tcp_arg(struct tcp_pcb *pcb, void *arg);
void tcp_recv(struct tcp_pcb *pcb, tcp_recv_fn recv);
void tcp_err(struct tcp_pcb *pcb, tcp_err_fn err);
err_t tcp_close(struct tcp_pcb *pcb);
void tcp_abort(struct tcp_pcb *pcb);

#endif
