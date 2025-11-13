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

#include <stdio.h>
#include <errno.h>
#include <getopt.h>
#include <ipxe/in.h>
#include <ipxe/netdevice.h>
#include <ipxe/command.h>
#include <ipxe/parseopt.h>
#include <ipxe/socket.h>
#include <usr/apipa.h>

/** @file
 *
 * APIPA (Link-Local) management commands
 *
 */

/** "apipa" options */
struct apipa_options {
	/** Gateway address string */
	char *gateway;
};

/** "apipa" option list */
static struct option_descriptor apipa_opts[] = {
	OPTION_DESC ( "gateway", 'g', required_argument, struct apipa_options,
		      gateway, parse_string ),
};

/** "apipa" command descriptor */
static struct command_descriptor apipa_cmd =
	COMMAND_DESC ( struct apipa_options, apipa_opts, 0, MAX_ARGUMENTS,
		       "[--gateway|-g <gateway>] [<interface>] "
		       "[<setting> <value>]..." );

/**
 * The "apipa" command
 *
 * @v argc		Argument count
 * @v argv		Argument list
 * @ret rc		Return status code
 */
static int apipa_exec ( int argc, char **argv )
{
	struct apipa_options opts;
	struct net_device *netdev;
	struct in_addr gateway_addr;
	struct in_addr *gateway = NULL;
	char *interface_name = NULL;
	int settings_start;
	int num_settings;
	int rc;

	/* Parse options */
	if ( ( rc = parse_options ( argc, argv, &apipa_cmd, &opts ) ) != 0 )
		return rc;

	/* Parse gateway if specified */
	if ( opts.gateway ) {
		if ( inet_aton ( opts.gateway, &gateway_addr ) == 0 ) {
			printf ( "Invalid gateway address: %s\n",
				 opts.gateway );
			return -EINVAL;
		}
		gateway = &gateway_addr;
	}

	/* Get interface name if specified, otherwise use default */
	if ( optind < argc ) {
		interface_name = argv[optind];
		settings_start = optind + 1;
	} else {
		settings_start = optind;
	}

	/* Calculate number of setting arguments */
	num_settings = argc - settings_start;

	/* Check for setting/value pairs */
	if ( ( num_settings % 2 ) != 0 ) {
		printf ( "Settings must be specified as <setting> <value> "
			 "pairs\n" );
		return -EINVAL;
	}

	/* Parse network device */
	if ( interface_name ) {
		if ( ( rc = parse_netdev ( interface_name, &netdev ) ) != 0 )
			return rc;
	} else {
		/* Use default network device */
		netdev = last_opened_netdev ();
		if ( !netdev ) {
			printf ( "No network device specified and no default "
				 "available\n" );
			return -ENODEV;
		}
	}

	/* Configure APIPA */
	return apipa ( netdev, gateway, num_settings, &argv[settings_start] );
}

/** APIPA management commands */
COMMAND ( apipa, apipa_exec );
