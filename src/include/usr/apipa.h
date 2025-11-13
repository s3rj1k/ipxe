#ifndef _USR_APIPA_H
#define _USR_APIPA_H

/** @file
 *
 * APIPA management
 *
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );

struct net_device;
struct in_addr;

extern int apipa ( struct net_device *netdev, struct in_addr *gateway, int argc,
		   char **argv );

#endif /* _USR_APIPA_H */
