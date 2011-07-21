#ifndef OMPHALOS_NETLINK
#define OMPHALOS_NETLINK

#ifdef __cplusplus
extern "C" {
#endif

#include <pthread.h>

struct interface;
struct omphalos_ctx;
struct omphalos_iface;

int netlink_socket(const struct omphalos_iface *);

int iplink_modify(const struct omphalos_iface *,int,int,unsigned,unsigned);

void reap_thread(const struct omphalos_iface *,struct interface *);

int netlink_thread(const struct omphalos_iface *);

void cancellation_signal_handler(int);

// Loop on a netlink socket according to provided program parameters
int handle_netlink_socket(const struct omphalos_ctx *);

#ifdef __cplusplus
}
#endif

#endif
