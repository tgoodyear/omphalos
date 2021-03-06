#include <stdint.h>
#include <omphalos/pim.h>
#include <omphalos/diag.h>
#include <omphalos/omphalos.h>
#include <omphalos/interface.h>

typedef struct pimhdr {
	unsigned pimver: 4;
	unsigned pimtype: 4;
	unsigned reserved: 8;
	uint16_t csum;
} pimhdr;

void handle_pim_packet(omphalos_packet *op,const void *frame,size_t len){
	const struct pimhdr *pim = frame;

	if(len < sizeof(*pim)){
		diagnostic("%s malformed with %zu",__func__,len);
		++op->i->malformed;
		return;
	}
	// FIXME
}
