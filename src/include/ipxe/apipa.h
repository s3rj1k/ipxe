#ifndef _IPXE_APIPA_H
#define _IPXE_APIPA_H

/** @file
 *
 * IPv4 Link-Local Address (APIPA/RFC 3927)
 *
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );

#include <stdint.h>
#include <ipxe/in.h>
#include <ipxe/netdevice.h>

/** APIPA base address (169.254.0.0) */
#define APIPA_BASE ( ( 169UL << 24 ) | ( 254UL << 16 ) )

/** APIPA netmask (255.255.0.0) */
#define APIPA_NETMASK ( 0xFFFF0000UL )

/** Minimum usable APIPA address (169.254.1.0) */
#define APIPA_MIN ( APIPA_BASE | ( 1UL << 8 ) )

/** Maximum usable APIPA address (169.254.254.255) */
#define APIPA_MAX ( APIPA_BASE | ( 254UL << 8 ) | 255UL )

/** Number of ARP probes to send */
#define APIPA_PROBE_NUM 3

/** Time to wait for ARP probe response (in ticks) */
#define APIPA_PROBE_WAIT ( TICKS_PER_SEC / 5 ) /* 200ms */

/** Maximum number of address selection attempts */
#define APIPA_MAX_ATTEMPTS 10

/** Address generation multiplier (Fermat prime for good distribution) */
#define APIPA_ADDR_MULTIPLIER 65537

struct in_addr;

extern int apipa ( struct net_device *netdev, struct in_addr *gateway, int argc,
		   char **argv );

#endif /* _IPXE_APIPA_H */
