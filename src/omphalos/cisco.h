#ifndef OMPHALOS_CISCO
#define OMPHALOS_CISCO

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

struct omphalos_packet;

// Various Cisco garbage protocols

// Cisco Discovery Protocol over Ethernet
void handle_cld_packet(struct omphalos_packet *,const void *,size_t);

// Unidirectional Link Detection Protocol over Ethernet
void handle_udld_packet(struct omphalos_packet *,const void *,size_t);

// EIGRP over IPv4/IPv6
void handle_eigrp_packet(struct omphalos_packet *,const void *,size_t);

#ifdef __cplusplus
}
#endif

#endif
