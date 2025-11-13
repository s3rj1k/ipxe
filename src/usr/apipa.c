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

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <ipxe/netdevice.h>
#include <ipxe/ip.h>
#include <ipxe/arp.h>
#include <ipxe/if_arp.h>
#include <ipxe/timer.h>
#include <ipxe/settings.h>
#include <ipxe/parseopt.h>
#include <ipxe/ethernet.h>
#include <ipxe/if_ether.h>
#include <ipxe/iobuf.h>
#include <ipxe/apipa.h>
#include <usr/apipa.h>

/** @file
 *
 * IPv4 Link-Local Address configuration (APIPA/RFC 3927)
 *
 */

/**
 * Generate pseudo-random link-local IP address
 *
 * @v netdev		Network device
 * @v attempt		Attempt number (0 for first attempt)
 * @ret address		Link-local IP address (in host byte order)
 *
 * Generates an address in the range 169.254.1.0 to 169.254.254.255
 * Uses MAC address and attempt number as seed for deterministic generation
 * per RFC 3927. Each attempt generates a different address.
 */
static uint32_t apipa_generate_addr ( struct net_device *netdev,
				      unsigned int attempt )
{
	uint32_t seed = 0;
	uint32_t addr_range;
	uint32_t addr_offset;
	uint8_t *ll_addr = netdev->ll_addr;
	size_t ll_addr_len =
		netdev->ll_protocol ? netdev->ll_protocol->ll_addr_len : 0;
	unsigned int i;

	/* Use link-layer address as base seed for deterministic generation
	 * (RFC 3927 section 2.1). Handle variable-length addresses safely.
	 */
	for ( i = 0; ll_addr && i < ll_addr_len && i < 4; i++ ) {
		/* Start from end of address for better distribution */
		size_t idx = ll_addr_len - 1 - i;
		seed |= ( ( uint32_t ) ll_addr[idx] << ( i * 8 ) );
	}

	/* If address is shorter than 4 bytes, incorporate the bytes again
	 * to ensure we use all available entropy
	 */
	for ( i = 0; ll_addr && i < ll_addr_len && ( i + 4 ) < 8; i++ ) {
		seed ^= ( ( uint32_t ) ll_addr[i] << ( ( i % 4 ) * 8 ) );
	}

	/* Modify seed based on attempt number to generate different addresses
	 * for each retry while maintaining deterministic behavior
	 */
	seed = seed + ( attempt * APIPA_ADDR_MULTIPLIER );

	/* Generate deterministic offset within the usable range */
	addr_range = ( APIPA_MAX - APIPA_MIN + 1 );
	addr_offset = ( seed % addr_range );

	return ( APIPA_MIN + addr_offset );
}

/**
 * Check for ARP conflicts in received packets
 *
 * @v netdev		Network device
 * @v address		Address being probed
 * @ret conflict	True if conflict detected
 *
 * Inspects received packets in the RX queue to detect if any ARP packet
 * has a sender IP matching the address being probed. This is used for
 * RFC 3927 conflict detection during address probing.
 */
static int apipa_check_arp_conflict ( struct net_device *netdev,
				      struct in_addr address )
{
	struct io_buffer *iobuf;
	struct io_buffer *tmp;
	struct ethhdr *ethhdr;
	struct arphdr *arphdr;
	void *arp_sender_pa;
	size_t ll_hlen = netdev->ll_protocol->ll_header_len;
	size_t min_len;

	/* Check all packets in RX queue */
	list_for_each_entry_safe ( iobuf, tmp, &netdev->rx_queue, list )
	{
		/* Check if it's an ARP packet */
		if ( iob_len ( iobuf ) < ll_hlen + sizeof ( *ethhdr ) )
			continue;

		ethhdr = iobuf->data;
		if ( ethhdr->h_protocol != htons ( ETH_P_ARP ) )
			continue;

		/* Ensure we can read ARP header */
		if ( iob_len ( iobuf ) < ll_hlen + sizeof ( *arphdr ) )
			continue;

		arphdr = ( iobuf->data + ll_hlen );

		/* Validate address lengths match what we expect */
		if ( arphdr->ar_hln != netdev->ll_protocol->ll_addr_len )
			continue;
		if ( arphdr->ar_pln != sizeof ( address ) )
			continue;

		/* Calculate minimum size needed to access sender IP */
		min_len = ll_hlen + sizeof ( *arphdr ) + arphdr->ar_hln +
			  arphdr->ar_pln;

		/* Ensure packet is long enough */
		if ( iob_len ( iobuf ) < min_len )
			continue;

		/* Get sender IP address (after sender MAC) */
		arp_sender_pa = ( ( void * ) arphdr ) + sizeof ( *arphdr ) +
				arphdr->ar_hln;

		/* Check if sender IP matches our probed address */
		if ( memcmp ( arp_sender_pa, &address, sizeof ( address ) ) ==
		     0 ) {
			DBGC ( netdev, "APIPA %s conflict: ARP from %s\n",
			       netdev->name, inet_ntoa ( address ) );
			return 1;
		}
	}

	return 0;
}

/**
 * Probe a candidate APIPA address for conflicts
 *
 * @v netdev		Network device
 * @v address		Address to probe
 * @ret rc		Return status code (0 = no conflict, non-zero =
 * conflict/error)
 *
 * Sends 3 ARP probes per RFC 3927 and checks for conflicts by inspecting
 * received ARP packets. Returns 0 if the address is safe to use, -EADDRINUSE
 * if a conflict is detected, or other error code on transmission failure.
 */
static int apipa_probe_address ( struct net_device *netdev,
				 struct in_addr address )
{
	struct in_addr zero_addr = { .s_addr = 0 };
	unsigned int probe;
	int rc;

	DBGC ( netdev, "APIPA %s probing %s\n", netdev->name,
	       inet_ntoa ( address ) );

	/* RFC 3927: Send 3 ARP probes to detect conflicts */
	for ( probe = 0; probe < APIPA_PROBE_NUM; probe++ ) {
		/* Flush any old packets from previous probes to keep RX queue
		 * small and avoid redundant scanning in
		 * apipa_check_arp_conflict
		 */
		netdev_poll ( netdev );

		/* Send ARP probe with sender IP = 0.0.0.0 (RFC 3927) */
		rc = arp_tx_request ( netdev, &ipv4_protocol, &address,
				      &zero_addr );
		if ( rc != 0 ) {
			DBGC ( netdev,
			       "APIPA %s probe transmission failed: "
			       "%s\n",
			       netdev->name, strerror ( rc ) );
			return rc;
		}

		DBGC2 ( netdev, "APIPA %s sent probe %u/%u\n", netdev->name,
			probe + 1, APIPA_PROBE_NUM );

		/* Wait for potential responses */
		mdelay ( ( APIPA_PROBE_WAIT * 1000 ) / TICKS_PER_SEC );

		/* Check for conflicts before processing packets */
		if ( apipa_check_arp_conflict ( netdev, address ) ) {
			/* Still process packets normally */
			netdev_poll ( netdev );
			return -EADDRINUSE;
		}

		/* Process any received packets */
		netdev_poll ( netdev );

		/* RFC 3927 Section 2.2.1: Wait 1-2 seconds between probes
		 * (except after last probe)
		 */
		if ( probe < ( APIPA_PROBE_NUM - 1 ) )
			mdelay ( 1000 + ( random () % 1000 ) );
	}

	return 0;
}

/**
 * Store APIPA configuration settings
 *
 * @v netdev		Network device
 * @v address		Assigned IP address
 * @v netmask		Network mask
 * @v gateway		Gateway address
 * @v argc		Number of setting/value arguments
 * @v argv		Setting/value argument pairs
 * @ret rc		Return status code
 *
 * Stores the IP configuration and custom settings in the network device's
 * settings, making them available to iPXE's configuration system.
 */
static int apipa_store_settings ( struct net_device *netdev,
				  struct in_addr address,
				  struct in_addr netmask,
				  struct in_addr gateway, int argc,
				  char **argv )
{
	struct settings *settings = netdev_settings ( netdev );
	struct named_setting setting;
	char addr_str[16];
	char mask_str[16];
	char gw_str[16];
	int i;
	int rc;

	/* Format addresses into local buffers to avoid inet_ntoa()
	 * static buffer reuse issues
	 */
	snprintf ( addr_str, sizeof ( addr_str ), "%s", inet_ntoa ( address ) );
	snprintf ( mask_str, sizeof ( mask_str ), "%s", inet_ntoa ( netmask ) );

	/* Store IP settings */
	if ( ( rc = storef_setting ( settings, &ip_setting, addr_str ) ) != 0 )
		return rc;

	if ( ( rc = storef_setting ( settings, &netmask_setting, mask_str ) ) !=
	     0 )
		return rc;

	if ( gateway.s_addr ) {
		snprintf ( gw_str, sizeof ( gw_str ), "%s",
			   inet_ntoa ( gateway ) );
		if ( ( rc = storef_setting ( settings, &gateway_setting,
					     gw_str ) ) != 0 )
			return rc;
	}

	/* Store custom settings if provided (as setting/value pairs) */
	for ( i = 0; i < argc; i += 2 ) {
		/* Parse setting name using iPXE's parser */
		if ( ( rc = parse_autovivified_setting ( argv[i],
							 &setting ) ) != 0 )
			return rc;

		/* Apply default type if necessary */
		if ( !setting.setting.type )
			setting.setting.type = &setting_type_string;

		/* Store the setting value */
		if ( ( rc = storef_setting ( setting.settings, &setting.setting,
					     argv[i + 1] ) ) != 0 )
			return rc;

		DBGC ( netdev, "APIPA %s stored setting %s = %s\n",
		       netdev->name, argv[i], argv[i + 1] );
	}

	return 0;
}

/**
 * Configure network device with link-local address (APIPA/RFC 3927)
 *
 * @v netdev		Network device
 * @v gw		Gateway address (or NULL for none)
 * @v argc		Number of setting/value arguments (must be even)
 * @v argv		Setting/value argument pairs
 * @ret rc		Return status code
 *
 * Partially implements RFC 3927 IPv4 Link-Local address autoconfiguration.
 */
int apipa ( struct net_device *netdev, struct in_addr *gw, int argc,
	    char **argv )
{
	struct in_addr address;
	struct in_addr netmask;
	struct in_addr network;
	struct in_addr gateway = { .s_addr = 0 };
	unsigned int attempts;
	int rc;

	/* Validate network device has link-layer address */
	if ( !netdev->ll_protocol || !netdev->ll_addr ) {
		printf ( "%s: no link-layer address available\n",
			 netdev->name );
		return -ENODEV;
	}

	/* Open network device if not already open */
	if ( !netdev_is_open ( netdev ) ) {
		if ( ( rc = netdev_open ( netdev ) ) != 0 ) {
			printf ( "Could not open %s: %s\n", netdev->name,
				 strerror ( rc ) );
			return rc;
		}
	}

	/* Check link state to avoid wasting time probing without connectivity
	 */
	if ( !netdev_link_ok ( netdev ) ) {
		printf ( "%s: link is down (%s), cannot configure APIPA\n",
			 netdev->name, strerror ( netdev->link_rc ) );
		return netdev->link_rc;
	}

	printf ( "Configuring %s with link-local address...\n", netdev->name );

	/* Set netmask for link-local network (255.255.0.0) */
	netmask.s_addr = htonl ( APIPA_NETMASK );
	network.s_addr = htonl ( APIPA_BASE );

	/* Use provided gateway if specified */
	if ( gw )
		gateway = *gw;

	/* RFC 3927 Section 2.1: Wait random 0-1 second before probing */
	mdelay ( random () % 1000 );

	/* Try multiple address candidates */
	for ( attempts = 0; attempts < APIPA_MAX_ATTEMPTS; attempts++ ) {
		/* Generate candidate address (varies with attempt number) */
		address.s_addr =
			htonl ( apipa_generate_addr ( netdev, attempts ) );

		/* Probe the address for conflicts */
		rc = apipa_probe_address ( netdev, address );
		if ( rc == 0 ) {
			/* No conflict - use this address */
			break;
		}

		/* Handle probe failure or conflict */
		DBGC ( netdev, "APIPA %s probe failed for %s: %s\n",
		       netdev->name, inet_ntoa ( address ), strerror ( rc ) );
	}

	/* Check if we exceeded maximum attempts */
	if ( attempts >= APIPA_MAX_ATTEMPTS ) {
		printf ( "Failed to find available link-local address\n" );
		return -EADDRINUSE;
	}

	/* Add the route (this also assigns the address) */
	if ( ( rc = ipv4_add_miniroute ( netdev, address, network, netmask,
					 gateway ) ) != 0 ) {
		printf ( "Could not configure %s: %s\n", netdev->name,
			 strerror ( rc ) );
		return rc;
	}

	/* RFC 3927 Section 2.3: Send 2 gratuitous ARPs spaced 2 seconds apart
	 * to announce our claimed address
	 */
	for ( unsigned int announcement = 0; announcement < 2;
	      announcement++ ) {
		/* Wait 2 seconds before second announcement (RFC 3927) */
		if ( announcement > 0 )
			mdelay ( 2000 );

		/* Send gratuitous ARP announcement */
		rc = arp_tx_request ( netdev, &ipv4_protocol, &address,
				      &address );
		if ( rc != 0 ) {
			printf ( "Failed to announce address: %s\n",
				 strerror ( rc ) );
			return rc;
		}

		DBGC2 ( netdev, "APIPA %s sent announcement %u/2\n",
			netdev->name, announcement + 1 );
	}

	/* Display configuration */
	printf ( "%s configured with %s", netdev->name, inet_ntoa ( address ) );
	if ( gateway.s_addr )
		printf ( " gw %s", inet_ntoa ( gateway ) );
	printf ( "\n" );

	/* Store IP configuration and custom settings */
	rc = apipa_store_settings ( netdev, address, netmask, gateway, argc,
				    argv );
	if ( rc != 0 ) {
		printf ( "Failed to store settings: %s\n", strerror ( rc ) );
		return rc;
	}

	return 0;
}
