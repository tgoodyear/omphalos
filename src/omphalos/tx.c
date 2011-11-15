#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <sys/socket.h>
#include <netinet/ip6.h>
#include <omphalos/tx.h>
#include <linux/if_arp.h>
#include <omphalos/diag.h>
#include <omphalos/pcap.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <omphalos/hwaddrs.h>
#include <omphalos/psocket.h>
#include <omphalos/ethernet.h>
#include <omphalos/omphalos.h>
#include <omphalos/interface.h>

#define TP_STATUS_PREPARING (~0u)

// Transmission is complicated due to idiosyncracies of various emission
// mechanisms, and also portability issues -- both across interfaces, and
// protocols, as we shall see. Transmission is built around the tpacket_hdr
// structure, necessary for use with PACKET_TX_MMAP-enabled packet sockets. In
// terms of performance and power, we can order mechanisms from most desirable
// to least desirable:
//
//  - PACKET_TX_MMAP-enabled PF_PACKET, SOCK_RAW sockets. Only available on
//     properly-configured late 2.6-series Linux kernels. Require CAP_NET_ADMIN.
//  - PF_PACKET, SOCK_RAW sockets. Require CAP_NET_ADMIN.
//  - PF_PACKET, SOCK_DGRAM sockets. Physical layer is inserted based on
//     sockaddr_ll information, which doesn't allow e.g. specification of
//     source hardware address. Require CAP_NET_ADMIN.
//  - PF_INET[6], SOCK_RAW, IPPROTO_RAW sockets. Physical layer is wholly
//     generated by the kernel. IP only. Require CAP_NET_ADMIN.
//  - IP_HDRINCL-enabled PF_INET[6], SOCK_RAW sockets of protocol other than
//     IPPROTO_RAW. Physical layer is wholly generated by the kernel.
//     Individual IP protocols only. Require CAP_NET_ADMIN.
//  - PF_INET[6], SOCK_RAW sockets of protocol other than IPPROTO_RAW. Layers
//     2 through 3 are wholly generated by the kernel. Individual IP protocols
//     only. Require CAP_NET_ADMIN.
//  - PF_INET[6] sockets of type other than SOCK_RAW. Layers 2 through 4 are
//     generated by the kernel. Only certain IP protocols are supported.
//     Subject to iptables actions.
//
// Ideally, transmission would need know nothing about the packet save its
// length, outgoing interface, and location in memory. This is most easily
// effected via PF_PACKET, SOCK_RAW sockets, which are also, happily, the
// highest-performing option. A complication arises, however: PF_PACKET packets
// are not injected into the local IP stack (it is for this reason that they're
// likewise not subject to iptables rules). They *are* copied to other
// PF_PACKET sockets on the host. This causes problems for three cases:
//
//  - self-directed unicast, including all loopback traffic
//  - multicast (independent of IP_MULTICAST_LOOP use), and
//  - broadcast.
//
// In these cases, we must fall back to the next most powerful mechanism,
// PF_INET sockets of SOCK_RAW type and IPPROTO_RAW protocol. These only
// support IPv4 and IPv6, and we need one socket for each. Thankfully, they are
// Layer 2-independent, and thus can be used on any type of interface.
// Unfortunately, we must still known enough of each interface's Layer 2
// protocol to recognize multicast and broadcast (where these concepts exist),
// and self-directed unicast.
//
// So, when given a packet to transmit, we must determine the transmission
// type based on that device's semantics. If the packet ought be visible to our
// local IP stack, we either transmit it solely there (unicast), or duplicate
// it via unicast (multi/broadcast). Note that other PF_PACKET listeners will
// thus see two packets for outgoing multicast and broadcasts of ours.

// Acquire a frame from the ringbuffer. Start writing, given return value
// 'frame', at: (char *)frame + ((struct tpacket_hdr *)frame)->tp_mac.
void *get_tx_frame(interface *i,size_t *fsize){
	struct tpacket_hdr *thdr;
	void *ret;

	assert(pthread_mutex_lock(&i->lock) == 0);
	thdr = i->curtxm;
	if(thdr == NULL){
		pthread_mutex_unlock(&i->lock);
		diagnostic(L"Can't transmit on %s (fd %d)",i->name,i->fd);
		return NULL;
	}
	if(thdr->tp_status != TP_STATUS_AVAILABLE){
		if(thdr->tp_status != TP_STATUS_WRONG_FORMAT){
			pthread_mutex_unlock(&i->lock);
			diagnostic(L"No available TX frames on %s",i->name);
			return NULL;
		}
		thdr->tp_status = TP_STATUS_AVAILABLE;
	}
	// Need indicate that this one is in use, but don't want to
	// indicate that it should be sent yet
	thdr->tp_status = TP_STATUS_PREPARING;
	// FIXME we ought be able to set this once for each packet, and be done
	thdr->tp_net = thdr->tp_mac = TPACKET_ALIGN(sizeof(struct tpacket_hdr));
	ret = i->curtxm;
	i->curtxm += inclen(&i->txidx,&i->ttpr);
	pthread_mutex_unlock(&i->lock);
	*fsize = i->ttpr.tp_frame_size;
	return ret;
}

// Loopback devices don't play nicely with PF_PACKET sockets; transmitting on
// them results in packets visible to device-level sniffers (tcpdump, wireshark,
// ourselves) but never injected into the machine's IP stack. These results are
// not portable across systems, either; FreeBSD and OpenBSD do things
// differently, to the point of a distinct loopback header type.
//
// So, we take the packet as prepared, and sendto() over the (unbound) TX
// sockets (we need one per protocol family -- effectively, AF_INET and
// AF_INET6). This suffers a copy, of course.
//
// We currently only support IPv4, but ought support IPv6 also. UDP is the only
// transport protocol supported now, or likely to be supported ever (due to the
// mechanics of the mechanism).
static ssize_t
send_to_self(interface *i,void *frame){
	struct tpacket_hdr *thdr = frame;
	struct sockaddr_in6 sina6;
	struct sockaddr_in sina;
	unsigned short l2proto;
	struct sockaddr *ss;
	const void *payload;
	socklen_t slen;
	const char *l2;
	unsigned plen;
	size_t l2len;
	int fd,r;

	l2 = ((const char *)frame + thdr->tp_mac);
	switch(i->arptype){
		case ARPHRD_ETHER:
			l2len = sizeof(struct ethhdr);
			l2proto = ((const struct ethhdr *)l2)->h_proto;
			break;
		case ARPHRD_LOOPBACK: // emulates loopback
			l2len = sizeof(struct ethhdr);
			l2proto = ((const struct ethhdr *)l2)->h_proto;
			break;
		default:
			assert(0);
			break;
	}
	if(l2proto == ntohs(ETH_P_IP)){
		const struct udphdr *udp;
		const struct iphdr *ip;

		fd = i->fd4;
		ss = (struct sockaddr *)&sina;
		slen = sizeof(sina);
		memset(ss,0,slen);
		sina.sin_family = AF_INET;
		ip = (const struct iphdr *)(l2 + l2len);
		assert(ip->protocol == IPPROTO_UDP);
		sina.sin_addr.s_addr = ip->daddr;
		udp = (const struct udphdr *)((const char *)ip + ip->ihl * 4u);
		sina.sin_port = udp->dest;
		plen = ntohs(ip->tot_len);
		payload = ip;
	}else if(l2proto == ntohs(ETH_P_IPV6)){
		const struct ip6_hdr *ip;
		const struct udphdr *udp;

		fd = i->fd6;
		ss = (struct sockaddr *)&sina6;
		slen = sizeof(sina6);
		memset(ss,0,slen);
		sina6.sin6_family = AF_INET6;
		ip = (const struct ip6_hdr *)(l2 + l2len);
		assert(ip->ip6_ctlun.ip6_un1.ip6_un1_nxt == IPPROTO_UDP);
		memcpy(&sina6.sin6_addr,&ip->ip6_dst,sizeof(ip->ip6_dst));
		udp = (const struct udphdr *)((const char *)ip + sizeof(*ip));
		// IPv6 doesn't support IP_HDRINCL.
		sina6.sin6_port = 0;
		plen = ntohs(udp->len);
		payload = udp;
	}else{
		return -1;
	}
	if((r = sendto(fd,payload,plen,0,ss,slen)) < 0){
		diagnostic(L"Error self-TXing on %s:%d (%s)",i->name,fd,strerror(errno));
	}
	return r;
}

// Determine whether the packet is (a) self-directed and/or (b) out-directed.
// We should never return 0 for both, unless we don't know the L2 protocol.
static inline int
categorize_tx(const interface *i,const void *frame,int *self,int *out){
	int r = 0;

	switch(i->arptype){
		case ARPHRD_ETHER:{
			const struct ethhdr *eth = frame;

			r = categorize_l2addr(i,eth->h_dest);
			// LOCAL, MULTICAST, and BROADCAST all ought go to us,
			// unless they're ARP.
			*self = (!(r == RTN_UNICAST)) && (ntohs(eth->h_proto) != ETH_P_ARP);
			*out = !(r == RTN_LOCAL);
			break;
		}case ARPHRD_LOOPBACK:{
			*self = 1;
			*out = 0;
			break;
		}default:
			diagnostic(L"Need implement %s for %u",__func__,i->arptype);
			*self = 0;
			*out = 0;
			break;
	}
	return r;
}

/* to log transmitted packets, use the following: {
}*/
// Mark a frame as ready-to-send. Must have come from get_tx_frame() using this
// same interface. Yes, we will see packets we generate on the RX ring.
void send_tx_frame(interface *i,void *frame){
	int self,out;

	categorize_tx(i,frame,&self,&out);
	if(self){
		int ret;

		ret = send_to_self(i,frame);
		if(ret < 0){
			++i->txerrors;
		}else{
			i->txbytes += ret;
			++i->txframes;
		}
	}
	if(out){
		struct tpacket_hdr *thdr = frame;
		uint32_t tplen = thdr->tp_len;
		int ret;

		assert(thdr->tp_status == TP_STATUS_PREPARING);
		//thdr->tp_status = TP_STATUS_SEND_REQUEST;
		//ret = send(i->fd,NULL,0,0);
		ret = send(i->fd,(const char *)frame + thdr->tp_mac,thdr->tp_len,0);
		thdr->tp_status = TP_STATUS_AVAILABLE;
		if(ret == 0){
			ret = tplen;
		}
		//diagnostic(L"Transmitted %d on %s",ret,i->name);
		if(ret < 0){
			diagnostic(L"Error out-TXing on %s (%s)",i->name,strerror(errno));
			++i->txerrors;
		}else{
			i->txbytes += ret;
			++i->txframes;
		}
	}
}

void abort_tx_frame(interface *i,void *frame){
	struct tpacket_hdr *thdr = frame;

	++i->txaborts;
	thdr->tp_status = TP_STATUS_AVAILABLE;
	diagnostic(L"Aborted TX %llu on %s",i->txaborts,i->name);
}

void prepare_arp_probe(const interface *i,void *frame,size_t *flen,
			const void *haddr,size_t hln,const void *paddr,size_t pln,const void *saddr){
	struct tpacket_hdr *thdr;
	unsigned char *payload;
	struct ethhdr *ehdr;
	struct arphdr *ahdr;
	size_t tlen;

	thdr = frame;
	if(*flen < sizeof(*thdr)){
		diagnostic(L"%s %s frame too small for tx",__func__,i->name);
		return;
	}
	tlen = thdr->tp_mac + sizeof(*ehdr) + sizeof(*ahdr)
			+ 2 * hln + 2 * pln;
	if(*flen < tlen){
		diagnostic(L"%s %s frame too small for tx",__func__,i->name);
		return;
	}
	assert(hln == i->addrlen); // FIXME handle this case
	// FIXME what about non-ethernet
	ehdr = (struct ethhdr *)((char *)frame + thdr->tp_mac);
	assert(prep_eth_header(ehdr,*flen - thdr->tp_mac,i,haddr,ETH_P_ARP) == sizeof(struct ethhdr));
	thdr->tp_len = sizeof(struct ethhdr) + sizeof(struct arphdr)
		+ hln * 2 + pln * 2;
	ahdr = (struct arphdr *)((char *)ehdr + sizeof(*ehdr));
	ahdr->ar_hrd = htons(ARPHRD_ETHER);
	ahdr->ar_pro = htons(ETH_P_IP);
	ahdr->ar_hln = hln;
	ahdr->ar_pln = pln;
	ahdr->ar_op = htons(ARPOP_REQUEST);
	// FIXME this is all horribly unsafe
	payload = (unsigned char *)ahdr + sizeof(*ahdr);
	// FIXME allow for spoofing
	memcpy(payload,i->addr,hln);
	memcpy(payload + hln,saddr,pln);
	// FIXME need a source network address
	memcpy(payload + hln + pln,haddr,hln);
	memcpy(payload + hln + pln + hln,paddr,pln);
	*flen = tlen;
}
