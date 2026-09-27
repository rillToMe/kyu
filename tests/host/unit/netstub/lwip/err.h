// HOST-TEST FAKE lwip/err.h — shadows the real header via -I netstub.
// Only the subset kernel/net/net_socket.c uses.
#ifndef NETSTUB_ERR_H
#define NETSTUB_ERR_H

typedef int err_t;
#define ERR_OK   0
#define ERR_MEM -1
#define ERR_CONN -6
#define ERR_ABRT -13
#define ERR_RST -14
#define ERR_INPROGRESS -5   // real lwIP value; used by net_dns.c

#endif
