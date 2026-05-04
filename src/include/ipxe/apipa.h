#ifndef _IPXE_APIPA_H
#define _IPXE_APIPA_H

/** @file
 *
 * IPv4 Link-Local Address (APIPA/RFC 3927)
 *
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );
FILE_SECBOOT ( PERMITTED );

#include <ipxe/timer.h>

/** APIPA base address (169.254.0.0) */
#define APIPA_BASE ( ( 169UL << 24 ) | ( 254UL << 16 ) )

/** APIPA netmask (255.255.0.0) */
#define APIPA_NETMASK ( 0xFFFF0000UL )

/** Minimum usable APIPA address (169.254.1.0) */
#define APIPA_MIN ( APIPA_BASE | ( 1UL << 8 ) )

/** Maximum usable APIPA address (169.254.254.255) */
#define APIPA_MAX ( APIPA_BASE | ( 254UL << 8 ) | 255UL )

/** Initial wait before first probe in ticks (RFC 3927 PROBE_WAIT = 1s) */
#define APIPA_PROBE_WAIT ( TICKS_PER_SEC )

/** Number of ARP probes to send (RFC 3927 PROBE_NUM) */
#define APIPA_PROBE_NUM 3

/** Minimum time between ARP probes in ticks (RFC 3927 PROBE_MIN = 1s) */
#define APIPA_PROBE_MIN ( TICKS_PER_SEC )

/** Maximum time between ARP probes in ticks (RFC 3927 PROBE_MAX = 2s) */
#define APIPA_PROBE_MAX ( 2 * TICKS_PER_SEC )

/** Time after last probe before claiming in ticks (RFC 3927 ANNOUNCE_WAIT) */
#define APIPA_ANNOUNCE_WAIT ( 2 * TICKS_PER_SEC )

/** Number of ARP announcements (RFC 3927 ANNOUNCE_NUM) */
#define APIPA_ANNOUNCE_NUM 2

/** Time between ARP announcements in ticks (RFC 3927 ANNOUNCE_INTERVAL = 2s) */
#define APIPA_ANNOUNCE_INTERVAL ( 2 * TICKS_PER_SEC )

/** Conflict threshold before rate limiting (RFC 3927 MAX_CONFLICTS) */
#define APIPA_MAX_CONFLICTS 10

/** Delay after MAX_CONFLICTS in ticks (RFC 3927 RATE_LIMIT_INTERVAL) */
#define APIPA_RATE_LIMIT_INTERVAL ( 60 * TICKS_PER_SEC )

/** Maximum number of address selection attempts */
#define APIPA_MAX_ATTEMPTS 64

/** Address generation multiplier (Fermat prime for good distribution) */
#define APIPA_ADDR_MULTIPLIER 65537

#endif /* _IPXE_APIPA_H */
