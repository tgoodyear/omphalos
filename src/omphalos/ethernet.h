#ifndef OMPHALOS_ETHERNET
#define OMPHALOS_ETHERNET

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/socket.h>
#include <linux/if_ether.h>
#include <linux/rtnetlink.h>

struct omphalos_packet;

// STP actually travels over 802.3 with SAP == 0x42 (most of the time).
#define ETH_P_STP	0x0802
#define ETH_P_CTP	0x9000
// FIXME for SAP/DAP == 0xfe in 802.3 traffic ("routed osi pdu's")
#define ETH_P_OSI	ETH_P_802_3

void handle_ethernet_packet(struct omphalos_packet *,const void *,size_t);

int prep_eth_header(void *,size_t,const struct interface *,const void *,
			uint16_t) __attribute__ ((nonnull (1,3,4)));

// The actual length received might be off due to padding up to 60 octets,
// the minimum Ethernet frame (discounting 4-octet FCS). In the presence of
// 802.1q VLAN tagging, the minimum Ethernet frame is 64 bytes (again
// discounting the 4-octet FCS); LLC/SNAP encapsulation do not extend the
// minimum or maximum frame length. Allow such frames to go through formedness
// checks. Always use the uppermost protocol's measure of size!
static inline int
check_ethernet_padup(size_t rx,size_t expected){
	if(expected == rx){
		return 0;
	}else if(rx > expected){
		// 4 bytes for possible 802.1q VLAN tag. Probably ought verify
		// that 802.1q is actually in use FIXME.
		if(rx <= ETH_ZLEN + 4 - sizeof(struct ethhdr)){
			return 0;
		}
	}
	return 1;
}

// Categorize an Ethernet address independent of context (this function never
// returns RTN_LOCAL or RTN_BROADCAST, for instance).
static inline int
categorize_ethaddr(const void *mac){
	if(((const unsigned char *)mac)[0] & 0x1){
		return RTN_MULTICAST;
	}
	return RTN_UNICAST;
}

#ifdef __cplusplus
}
#endif

#endif
