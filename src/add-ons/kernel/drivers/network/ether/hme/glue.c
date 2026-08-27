/*
 * Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
 * Distributed under the terms of the MIT License.
 */

#include <sys/bus.h>
#include <sys/rman.h>

#include <machine/bus.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>

#include <dev/mii/mii.h>

#include "dev/hme/if_hmereg.h"
#include "dev/hme/if_hmevar.h"

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


/*!	Silences the chip so the interrupt can be serviced in a thread.

	This pair is required of any driver that hands bus_setup_intr() a handler and
	no filter, which is what if_hme_pci.c does. The compatibility layer then
	installs `intr_wrapper` as the real interrupt handler: it runs at interrupt
	time, asks this function whether the interrupt is ours and to quieten the
	chip, and only then releases the semaphore that wakes the thread hme_intr()
	actually runs on.

	It is not optional and the alternative is not "nothing happens". Both used to
	be NO_HAIKU_CHECK_DISABLE_INTERRUPTS() and NO_HAIKU_REENABLE_INTERRUPTS(),
	whose bodies are a panic and an empty function -- so the first frame this
	interface ever received panicked the kernel, in a driver that had been
	attaching cleanly for weeks, because nothing had ever made it raise an
	interrupt before.

	**It has to read the status register, and that is the whole subtlety.**

	The Global Status Register is documented R-AC -- "all the bits are
	automatically cleared to 0 when the Status Register is read by the software"
	(PCIO manual, TABLE 6-9, printed p.81) -- and on this part reading it is also
	what *deasserts* the interrupt. Masking the Global Interrupt Mask does not:
	the mask decides which events raise the pin, not whether an already-latched
	one keeps it raised. Masking alone left the interrupt asserted, the machine
	took it again immediately, and the boot ended in a storm that starved the very
	thread meant to service it -- no panic, no output, just a processor pinned at
	PIL 15 in the interrupt entry path.

	So the value is read here and handed to hme_intr() through the softc, because
	by the time that runs the register is empty. That is a two-line change to the
	donor and it is the same one marvell_yukon carries, under the same field name,
	for a chip with the same property.

	The mask write stays. It is not what quietens the pin, but it keeps the chip
	from raising a *new* event between here and the thread, which would set the
	status bits again behind hme_intr()'s back.

	Claiming unconditionally is honest on this hardware: the interface has its own
	INO rather than a shared PCI pin, so an interrupt arriving here is ours by
	construction. The status is checked for zero anyway -- if it ever is, this was
	not our interrupt and saying so costs nothing.

	Reached through bus_space rather than the driver's HME_SEB_READ_4, which is
	defined inside if_hme.c and deliberately not exported; the softc's tag and
	handle are, and the macro expands to exactly this.
*/
int
HAIKU_CHECK_DISABLE_INTERRUPTS(device_t dev)
{
	struct hme_softc* sc = device_get_softc(dev);

	uint32_t status = bus_space_read_4(sc->sc_sebt, sc->sc_sebh, HME_SEBI_STAT);
	if (status == 0)
		return 0;

	sc->haiku_interrupt_status = status;
	bus_space_write_4(sc->sc_sebt, sc->sc_sebh, HME_SEBI_IMASK, 0xffffffff);
	return 1;
}


/*!	Lets the chip interrupt again, once the thread has serviced it.

	The value is the one hme_init_locked() programs, and it has to be: this runs
	after every interrupt, so anything else here would quietly become the
	driver's real interrupt mask a few microseconds after it configured itself.
*/
void
HAIKU_REENABLE_INTERRUPTS(device_t dev)
{
	struct hme_softc* sc = device_get_softc(dev);

	bus_space_write_4(sc->sc_sebt, sc->sc_sebh, HME_SEBI_IMASK,
		~(HME_SEB_STAT_HOSTTOTX
			| HME_SEB_STAT_RXTOHOST
			| HME_SEB_STAT_TXALL
			| HME_SEB_STAT_TXPERR
			| HME_SEB_STAT_RCNTEXP
			| HME_SEB_STAT_ALL_ERRORS));
}
