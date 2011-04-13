#include <sys/socket.h>
#include <linux/if_arp.h>
#include <omphalos/arp.h>
#include <omphalos/interface.h>

void handle_arp_packet(interface *i,const void *frame,size_t len){
	const struct arphdr *ap = frame;

	if(len < sizeof(*ap)){
		++i->malformed;
		return;
	}
	if(len != sizeof(*ap) + ap->ar_hln * 2 + ap->ar_pln * 2){
		fprintf(stderr,"%s malformed expected %zu got %zu\n",
			__func__, sizeof(*ap) + ap->ar_hln * 2 + ap->ar_pln * 2,len);
		++i->malformed;
		return;
	}
	switch(ap->ar_op){
	case ARPOP_REQUEST:{
		// FIXME reply with ARP spoof...
	break;}default:{
		fprintf(stderr,"%s unknown op %u\n",__func__,ap->ar_op);
		++i->noprotocol;
	break;}}
}
