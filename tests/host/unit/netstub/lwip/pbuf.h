// HOST-TEST FAKE lwip/pbuf.h.
#ifndef NETSTUB_PBUF_H
#define NETSTUB_PBUF_H

#include <stdint.h>

struct pbuf {
    struct pbuf *next;
    void        *payload;
    uint16_t     len;       // this segment
    uint16_t     tot_len;   // whole chain from here
};

void pbuf_free(struct pbuf *p);

#endif
