// HOST-TEST FAKE lwip/ip_addr.h.
#ifndef NETSTUB_IP_ADDR_H
#define NETSTUB_IP_ADDR_H

#include <stdint.h>

typedef struct ip_addr {
    uint32_t addr;
} ip_addr_t;

#endif
