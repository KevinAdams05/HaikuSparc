/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */

#include <sys/bus.h>

HAIKU_FBSD_DRIVER_GLUE(hme, hme, pci);

/*	ukphy rather than nsphy, and it is the right answer rather than the available
 *	one.
 *
 *	The transceiver on this part is a National Semiconductor DP83840 -- QEMU's
 *	sunhme reports exactly that in MII_PHYID1 -- and FreeBSD has an nsphy driver
 *	for it. What nsphy adds over the generic driver is handling for the DP83840's
 *	*vendor* registers, and neither the emulated part nor the auto-negotiating
 *	configuration this port cares about touches them. ukphy drives the IEEE
 *	registers every PHY has, which is all of what is modelled and all of what
 *	link negotiation needs.
 */
HAIKU_FBSD_MII_DRIVER(ukphy);

HAIKU_DRIVER_REQUIREMENTS(0);
NO_HAIKU_CHECK_DISABLE_INTERRUPTS();
NO_HAIKU_REENABLE_INTERRUPTS();
