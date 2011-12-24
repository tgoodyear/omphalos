#ifndef OMPHALOS_INTERFACE
#define OMPHALOS_INTERFACE

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <net/if.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <omphalos/128.h>
#include <linux/ethtool.h>
#include <linux/if_packet.h>
#include <omphalos/timing.h>
#include <omphalos/nl80211.h>
#include <omphalos/hwaddrs.h>

// Taken from linux/if.h as of 3.1-rc6
#define IFF_LOWER_UP	0x10000		// driver signals L1 up
#define IFF_DORMANT	0x20000		// driver signals dormant

#define IFF_ECHO	0x40000		// echo sent packets

#define IFF_VOLATILE	(IFF_LOOPBACK|IFF_POINTOPOINT|IFF_BROADCAST|IFF_ECHO|\
		IFF_MASTER|IFF_SLAVE|IFF_RUNNING|IFF_LOWER_UP|IFF_DORMANT)
// end linux/if.h copies */

struct l2host;
struct l3host;
struct in_addr;
struct in6_addr;
struct psocket_marsh;
struct omphalos_packet;

// bitmasks for the routes' 'addrs' field
#define ROUTE_HAS_SRC	0x1
#define ROUTE_HAS_VIA	0x2

typedef struct ip4route {
	uint32_t dst,via,src;
	unsigned addrs;
	unsigned maskbits;		// 0..31
	struct ip4route *next;
} ip4route;

typedef struct ip6route {
	uint128_t dst,via,src;
	unsigned addrs;
	unsigned maskbits;		// 0..127
	struct ip6route *next;
} ip6route;

typedef struct wless_info {
	unsigned bitrate;
	unsigned mode;
	uintmax_t freq;			// 0..999: channel, 1000+: frequency
} wless_info;

typedef struct topdev_info {
	wchar_t *devname;		// as in output from lspci or lsusb
} topdev_info;

typedef void (*analyzefxn)(struct omphalos_packet *,const void *,size_t);

#define IFACE_TIMESTAT_USECS 40000	// 25Hz sampling
#define IFACE_TIMESTAT_SLOTS 75		// x75 samples == 3s history

typedef struct interface {
	// Packet analysis entry point
	analyzefxn analyzer;

	// Lock (packet thread vs netlink layer vs UI)
	pthread_mutex_t lock;

	// Lifetime stats
	uintmax_t frames;		// Frames received on the interface
	uintmax_t malformed;		// Packet had malformed L2 -- L4 headers
	uintmax_t truncated;		// Packet didn't fit in ringbuffer frame
	uintmax_t truncated_recovered;	// We were able to recvfrom() the packet
	uintmax_t noprotocol;		// Packets without protocol handler
	uintmax_t bytes;		// Total bytes sniffed
	uintmax_t drops;		// PACKET_STATISTICS @ TP_STATUS_LOSING
	uintmax_t txframes;		// Frames generated by omphalos
	uintmax_t txbytes;		// Total bytes generated by omphalos
	uintmax_t txaborts;		// TX frames handed out but aborted
	uintmax_t txerrors;		// TX frames we failed to send

	// Finite time domain stats
	timestat fps,bps;		// frames and bits per second

	struct psocket_marsh *pmarsh;	// State for packet socket thread

	// For recvfrom()ing truncated packets (see PACKET_COPY_THRESH sockopt)
	void *truncbuf;
	size_t truncbuflen;

	unsigned arptype;	// from rtnetlink(7) ifi_type
	unsigned flags;		// from rtnetlink(7) ifi_flags
	size_t l2hlen;		// static l2 header length
	int mtu;		// to match netdevice(7)'s ifr_mtu...
	char *name;
	void *addr;		// multiple hwaddrs are multiple ifaces...
	void *bcast;		// l2 broadcast address (not valid unless
				//	(iface->flags & IFF_BROADCAST) is set)
	size_t addrlen;		// length of l2 addresses (addr, bcast, ...)
	int rfd;		// RX packet socket
	void *rxm;		// RX packet ring buffer
	size_t rs;		// RX packet ring size in bytes
	struct tpacket_req rtpr;// RX packet ring descriptor
	int fd;			// TX PF_PACKET socket
	void *txm;		// TX packet ring buffer
	int fd4,fd6udp,fd6icmp;	// Fallback IPv4 and IPv6 TX raw sockets
	size_t ts;		// TX packet ring size in bytes
	struct tpacket_req ttpr;// TX packet ring descriptor
	unsigned txidx;		// Index of next frame for TX
	void *curtxm;		// Location of next frame for TX
	struct ethtool_drvinfo drv;	// ethtool driver info
	unsigned offload;	// offloading settings
	unsigned offloadmask;	// which offloading settings are valid
	const char *busname;	// "pci", "usb" etc (from sysfs/bus/)
	enum {
		SETTINGS_INVALID,
		SETTINGS_VALID_ETHTOOL,
		SETTINGS_VALID_WEXT,
		SETTINGS_VALID_NL80211,
	} settings_valid;	// set if the settings field can be trusted
	union {
		struct ethtool_cmd ethtool;	// ethtool settings info
		struct wless_info wext;		// wireless extensions info
		nl80211_info nl80211;		// nl80211 info
	} settings;
	topdev_info topinfo;
	// Other interfaces might also offer routes to these same
	// destinations -- they must not be considered unique!
	struct ip4route *ip4r;	// list of IPv4 routes
	struct ip6route *ip6r;	// list of IPv6 routes

	uint128_t ip6defsrc;	// default ipv6 source FIXME

	struct l2host *l2hosts;
	struct l3host *ip4hosts,*ip6hosts,*cells;

	void *opaque;		// opaque callback state
} interface;

int init_interfaces(void);
interface *iface_by_idx(int);
int idx_of_iface(const interface *);
int print_iface_stats(FILE *,const interface *,interface *,const char *);

static inline char *
hwaddrstr(const interface *i){
	char *r;

	if( (r = malloc(HWADDRSTRLEN(i->addrlen))) ){
		l2ntop(i->addr,i->addrlen,r);
	}
	return r;
}

void free_iface(interface *);
void cleanup_interfaces(void);

int print_all_iface_stats(FILE *,interface *);
int add_route4(interface *,const uint32_t *,const uint32_t *,
				const uint32_t *,unsigned);
int add_route6(interface *,const uint128_t,const uint128_t,const uint128_t,
						unsigned);
int del_route4(interface *,const struct in_addr *,unsigned);
int del_route6(interface *,const struct in6_addr *,unsigned);

void set_default_ipv6src(interface *,const uint128_t);

const void *get_source_address(interface *,int,const void *,void *);

const void *get_unicast_address(interface *,int,const void *,void *);

// predicates. racey against netlink messages.
int is_local4(const interface *,uint32_t);
int is_local6(const interface *,const struct in6_addr *);

const char *lookup_arptype(unsigned,analyzefxn *,size_t *);

int enable_promiscuity(const interface *);
int disable_promiscuity(const interface *);
int up_interface(const interface *);
int down_interface(const interface *);

static inline int
interface_sniffing_p(const interface *i){
	return (i->rfd >= 0);
}

static inline int
interface_up_p(const interface *i){
	return (i->flags & IFF_UP);
}

static inline int
interface_carrier_p(const interface *i){
	return (i->flags & IFF_LOWER_UP);
}

static inline int
interface_promisc_p(const interface *i){
	return (i->flags & IFF_PROMISC);
}

static inline int
interface_virtual_p(const interface *i){
	return i->settings_valid == SETTINGS_INVALID;
}

static inline void
lock_interface(interface *i){
	int r = pthread_mutex_lock(&i->lock);
	assert(r == 0);
}

static inline void
unlock_interface(interface *i){
	int r = pthread_mutex_unlock(&i->lock);
	assert(r == 0);
}

#ifdef __cplusplus
}
#endif

#endif
