/*
 * Copyright (C) 2025 iPXE contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * You can also choose to distribute this program under the terms of
 * the Unmodified Binary Distribution Licence (as given in the file
 * COPYING.UBDL), provided that you have satisfied its requirements.
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );
FILE_SECBOOT ( PERMITTED );

#include <errno.h>
#include <ipxe/apipa.h>
#include <ipxe/arp.h>
#include <ipxe/if_arp.h>
#include <ipxe/if_ether.h>
#include <ipxe/interface.h>
#include <ipxe/iobuf.h>
#include <ipxe/ip.h>
#include <ipxe/netdevice.h>
#include <ipxe/refcnt.h>
#include <ipxe/retry.h>
#include <ipxe/settings.h>
#include <ipxe/timer.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @file
 *
 * IPv4 Link-Local Address configuration (APIPA/RFC 3927)
 *
 */

/** APIPA configurator phases */
enum apipa_phase {
	/** Sending an ARP probe */
	APIPA_PHASE_PROBE = 0,
	/** Waiting for probe response */
	APIPA_PHASE_PROBE_WAIT,
	/** Waiting ANNOUNCE_WAIT after last probe before claiming */
	APIPA_PHASE_ANNOUNCE_WAIT,
	/** Sending ARP announcements */
	APIPA_PHASE_ANNOUNCE,
};

/** An APIPA configurator */
struct apipa_config {
	/** Reference count */
	struct refcnt refcnt;
	/** List of configurators */
	struct list_head list;
	/** Job control interface */
	struct interface job;
	/** Network device */
	struct net_device *netdev;
	/** Retransmission timer */
	struct retry_timer timer;
	/** Current candidate address */
	struct in_addr address;
	/** Current attempt number */
	unsigned int attempt;
	/** Current probe number within attempt */
	unsigned int probe;
	/** Announcement counter */
	unsigned int announcement;
	/** Total conflict count (for rate limiting per RFC 3927) */
	unsigned int conflicts;
	/** Current phase */
	enum apipa_phase phase;
};

/** List of APIPA configurators */
static LIST_HEAD ( apipa_configs );

/**
 * Generate pseudo-random link-local IP address
 *
 * @v netdev		Network device
 * @v attempt		Attempt number (0 for first attempt)
 * @ret address		Link-local IP address (in network byte order)
 */
static struct in_addr apipa_generate_addr ( struct net_device *netdev,
					    unsigned int attempt ) {
	struct in_addr addr;
	uint32_t seed = 0;
	uint8_t *ll_addr = netdev->ll_addr;
	size_t ll_addr_len = netdev->ll_protocol->ll_addr_len;
	unsigned int i;

	for ( i = 0; i < ll_addr_len; i++ )
		seed ^= ( ( uint32_t ) ll_addr[i] << ( ( i % 4 ) * 8 ) );

	seed += ( attempt * APIPA_ADDR_MULTIPLIER ) + random();

	addr.s_addr =
		htonl ( APIPA_MIN + ( seed % ( APIPA_MAX - APIPA_MIN + 1 ) ) );
	return addr;
}

/**
 * Check for ARP conflicts in received packets (RFC 3927 Section 2.2.1)
 *
 * @v netdev		Network device
 * @v address		Address being probed
 * @ret conflict	True if conflict detected
 *
 * Detects two types of conflicts per RFC 3927:
 *  (a) Any ARP (request or reply) with sender IP == probed address
 *  (b) Any ARP probe with target IP == probed address and sender
 *      hardware address != ours (simultaneous prober)
 *
 * The RX queue must be frozen before calling this function.
 */
static int apipa_check_arp_conflict ( struct net_device *netdev,
				      struct in_addr address ) {
	struct io_buffer *iobuf;
	struct ll_protocol *ll_protocol = netdev->ll_protocol;
	const void *ll_dest;
	const void *ll_source;
	uint16_t net_proto;
	unsigned int flags;
	struct arphdr *arphdr;
	struct in_addr zero_addr = { .s_addr = 0 };
	int conflict = 0;

	while ( ( iobuf = netdev_rx_dequeue ( netdev ) ) ) {
		if ( ll_protocol->pull ( netdev, iobuf, &ll_dest, &ll_source,
					 &net_proto, &flags ) != 0 ) {
			free_iob ( iobuf );
			continue;
		}

		if ( net_proto != htons ( ETH_P_ARP ) ) {
			free_iob ( iobuf );
			continue;
		}

		if ( iob_len ( iobuf ) < sizeof ( *arphdr ) ) {
			free_iob ( iobuf );
			continue;
		}

		arphdr = iobuf->data;

		if ( arphdr->ar_hln != ll_protocol->ll_addr_len ||
		     arphdr->ar_pln != sizeof ( address ) ) {
			free_iob ( iobuf );
			continue;
		}

		if ( iob_len ( iobuf ) < arp_len ( arphdr ) ) {
			free_iob ( iobuf );
			continue;
		}

		/* RFC 3927 Section 2.2.1 conflict case (a):
		 * sender IP matches our probed address */
		if ( memcmp ( arp_sender_pa ( arphdr ), &address,
			      sizeof ( address ) ) == 0 ) {
			printf ( "  Conflict detected for %s\n",
				 inet_ntoa ( address ) );
			conflict = 1;
		}

		/* RFC 3927 Section 2.2.1 conflict case (b):
		 * ARP probe (sender IP == 0.0.0.0) with target IP matching
		 * our probed address and sender hardware address != ours
		 * (simultaneous prober) */
		if ( !conflict &&
		     memcmp ( arp_sender_pa ( arphdr ), &zero_addr,
			      sizeof ( zero_addr ) ) == 0 &&
		     memcmp ( arp_target_pa ( arphdr ), &address,
			      sizeof ( address ) ) == 0 &&
		     memcmp ( arp_sender_ha ( arphdr ), netdev->ll_addr,
			      arphdr->ar_hln ) != 0 ) {
			printf ( "  Conflict detected for %s (simultaneous "
				 "probe)\n",
				 inet_ntoa ( address ) );
			conflict = 1;
		}

		free_iob ( iobuf );

		if ( conflict )
			break;
	}

	return conflict;
}

/**
 * Store APIPA configuration settings
 *
 * @v netdev		Network device
 * @v address		Assigned IP address
 * @ret rc		Return status code
 */
static int apipa_store_settings ( struct net_device *netdev,
				  struct in_addr address ) {
	struct settings *settings = netdev_settings ( netdev );
	struct in_addr netmask = { .s_addr = htonl ( APIPA_NETMASK ) };
	int rc;

	if ( ( rc = store_setting ( settings, &ip_setting, &address.s_addr,
				    sizeof ( address.s_addr ) ) ) != 0 )
		return rc;
	if ( ( rc = store_setting ( settings, &netmask_setting, &netmask.s_addr,
				    sizeof ( netmask.s_addr ) ) ) != 0 )
		return rc;

	return 0;
}

static void apipa_done ( struct apipa_config *apipa, int rc );

/**
 * Free APIPA configurator
 *
 * @v refcnt		Reference count
 */
static void apipa_free ( struct refcnt *refcnt ) {
	struct apipa_config *apipa =
		container_of ( refcnt, struct apipa_config, refcnt );

	netdev_put ( apipa->netdev );
	free ( apipa );
}

/**
 * Advance to next APIPA candidate address
 *
 * @v apipa		APIPA configurator
 * @ret rc		Return status code (0 = ok, error = exhausted)
 */
static int apipa_next_address ( struct apipa_config *apipa ) {
	apipa->attempt++;
	apipa->conflicts++;
	if ( apipa->attempt >= APIPA_MAX_ATTEMPTS )
		return -EADDRINUSE;

	apipa->address = apipa_generate_addr ( apipa->netdev, apipa->attempt );
	apipa->probe = 0;
	apipa->phase = APIPA_PHASE_PROBE;
	return 0;
}

/**
 * Poll and retry on ARP conflict for current address
 *
 * @v apipa		APIPA configurator
 * @ret handled		True if conflict detected (timer restarted or done)
 */
static int apipa_retry_on_conflict ( struct apipa_config *apipa ) {
	unsigned long delay;
	int rc;

	netdev_poll ( apipa->netdev );
	if ( !apipa_check_arp_conflict ( apipa->netdev, apipa->address ) )
		return 0;

	printf ( "  %s unavailable, retrying...\n",
		 inet_ntoa ( apipa->address ) );
	if ( ( rc = apipa_next_address ( apipa ) ) != 0 ) {
		apipa_done ( apipa, rc );
		return 1;
	}

	/* RFC 3927 Section 2.1: after MAX_CONFLICTS, rate limit */
	delay = ( apipa->conflicts >= APIPA_MAX_CONFLICTS )
			? APIPA_RATE_LIMIT_INTERVAL
			: 0;
	start_timer_fixed ( &apipa->timer, delay );
	return 1;
}

/**
 * Handle APIPA timer expiry
 *
 * @v timer		Retry timer
 * @v fail		Failure indicator
 */
static void apipa_expired ( struct retry_timer *timer, int fail ) {
	struct apipa_config *apipa =
		container_of ( timer, struct apipa_config, timer );
	struct net_device *netdev = apipa->netdev;
	struct in_addr zero_addr = { .s_addr = 0 };
	unsigned long delay;
	int rc;

	/* If timer has been retried to exhaustion, fail */
	if ( fail ) {
		apipa_done ( apipa, -ETIMEDOUT );
		return;
	}

	switch ( apipa->phase ) {
	case APIPA_PHASE_PROBE:
		/* Send ARP probe and wait PROBE_MIN..PROBE_MAX for response
		 * (RFC 3927 Section 2.2.1) */
		printf ( "  Probing %s...\n", inet_ntoa ( apipa->address ) );
		netdev_poll ( netdev );
		rc = arp_tx_request ( netdev, &ipv4_protocol, &apipa->address,
				      &zero_addr );
		if ( rc != 0 ) {
			DBGC ( netdev, "APIPA %s probe tx failed: %s\n",
			       netdev->name, strerror ( rc ) );
			apipa_done ( apipa, rc );
			return;
		}
		/* Wait random PROBE_MIN (1s) to PROBE_MAX (2s) for responses */
		apipa->phase = APIPA_PHASE_PROBE_WAIT;
		delay = APIPA_PROBE_MIN +
			( random() % ( APIPA_PROBE_MAX - APIPA_PROBE_MIN ) );
		start_timer_fixed ( &apipa->timer, delay );
		return;

	case APIPA_PHASE_PROBE_WAIT:
		/* Check for conflicts after inter-probe delay */
		if ( apipa_retry_on_conflict ( apipa ) )
			return;

		/* No conflict on this probe */
		apipa->probe++;
		if ( apipa->probe < APIPA_PROBE_NUM ) {
			/* More probes needed: send next immediately */
			apipa->phase = APIPA_PHASE_PROBE;
			start_timer_nodelay ( &apipa->timer );
			return;
		}

		/* All probes passed.  Wait ANNOUNCE_WAIT (2s) after last
		 * probe before claiming (RFC 3927 Section 2.3). */
		apipa->phase = APIPA_PHASE_ANNOUNCE_WAIT;
		start_timer_fixed ( &apipa->timer, APIPA_ANNOUNCE_WAIT );
		return;

	case APIPA_PHASE_ANNOUNCE_WAIT:
		/* Final conflict check before claiming */
		if ( apipa_retry_on_conflict ( apipa ) )
			return;

		/* Claim the address: store settings (triggers route +
		 * gratuitous ARP) */
		rc = apipa_store_settings ( netdev, apipa->address );
		if ( rc != 0 ) {
			apipa_done ( apipa, rc );
			return;
		}

		printf ( "  %s configured with %s\n", netdev->name,
			 inet_ntoa ( apipa->address ) );

		/* Send first explicit announcement and schedule second */
		arp_tx_request ( netdev, &ipv4_protocol, &apipa->address,
				 &apipa->address );
		apipa->announcement = 1;
		apipa->phase = APIPA_PHASE_ANNOUNCE;
		start_timer_fixed ( &apipa->timer, APIPA_ANNOUNCE_INTERVAL );
		return;

	case APIPA_PHASE_ANNOUNCE:
		/* Send remaining announcements (RFC 3927 Section 2.3) */
		arp_tx_request ( netdev, &ipv4_protocol, &apipa->address,
				 &apipa->address );
		apipa->announcement++;
		if ( apipa->announcement < APIPA_ANNOUNCE_NUM ) {
			start_timer_fixed ( &apipa->timer,
					    APIPA_ANNOUNCE_INTERVAL );
			return;
		}
		apipa_done ( apipa, 0 );
		return;
	}
}

/**
 * Finish APIPA autoconfiguration
 *
 * @v apipa		APIPA configurator
 * @v rc		Reason for finishing
 */
static void apipa_done ( struct apipa_config *apipa, int rc ) {
	/* Unfreeze RX queue to resume normal packet processing */
	netdev_rx_unfreeze ( apipa->netdev );
	/* Signal completion to parent (ifconf) */
	intf_shutdown ( &apipa->job, rc );
	stop_timer ( &apipa->timer );
	/* Remove from list and drop reference */
	list_del ( &apipa->list );
	ref_put ( &apipa->refcnt );
}

/** APIPA configurator job interface operations */
static struct interface_operation apipa_job_op[] = {
	INTF_OP ( intf_close, struct apipa_config *, apipa_done ),
};

/** APIPA configurator job interface descriptor */
static struct interface_descriptor apipa_job_desc =
	INTF_DESC ( struct apipa_config, job, apipa_job_op );

/**
 * Start APIPA autoconfiguration (RFC 3927)
 *
 * @v job		Job control interface
 * @v netdev		Network device
 * @ret rc		Return status code
 */
static int start_apipa ( struct interface *job, struct net_device *netdev ) {
	struct apipa_config *apipa;

	/* Allocate and initialise structure */
	apipa = zalloc ( sizeof ( *apipa ) );
	if ( !apipa )
		return -ENOMEM;
	ref_init ( &apipa->refcnt, apipa_free );
	intf_init ( &apipa->job, &apipa_job_desc, &apipa->refcnt );
	timer_init ( &apipa->timer, apipa_expired, &apipa->refcnt );
	apipa->netdev = netdev_get ( netdev );

	/* Generate first candidate address */
	apipa->address = apipa_generate_addr ( netdev, 0 );
	apipa->phase = APIPA_PHASE_PROBE;

	/* Freeze RX queue for manual ARP conflict inspection */
	netdev_rx_freeze ( netdev );

	/* Ensure rate-limit timer (60s) does not trigger retry backoff fail */
	set_timer_limits ( &apipa->timer, 0,
			   ( 4 * APIPA_RATE_LIMIT_INTERVAL ) );

	/* RFC 3927 Section 2.1: wait random 0-1s before first probe */
	start_timer_fixed ( &apipa->timer, random() % APIPA_PROBE_WAIT );

	/* Attach parent interface and add to configurator list */
	intf_plug_plug ( &apipa->job, job );
	list_add ( &apipa->list, &apipa_configs );
	return 0;
}

/** APIPA network device configurator */
struct net_device_configurator apipa_configurator __net_device_configurator = {
	.name = "apipa",
	.start = start_apipa,
};
