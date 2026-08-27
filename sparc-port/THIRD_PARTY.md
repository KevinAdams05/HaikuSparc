# Third-party code provenance

Every piece of code in this fork that originated elsewhere is recorded here, with its licence
and where it came from. Written as we go — reconstructing provenance later is miserable, and a
ledger nobody maintained is worse than none.

See [§3.2 of the porting plan](PORTING_PLAN.md#32-donor-code-and-licensing) for the policy this
implements.

## The rule, in one line

**BSD-family code may be ported and copied, with its copyright block preserved verbatim.
GPL code — Linux above all — may be read for behaviour and never copied.**

Check the **individual file header**, not the project's headline licence. BSD trees mix 2-, 3-
and 4-clause files, sometimes within one directory.

## Ledger

| Our file | Source | Origin licence | What was taken | Notes |
| --- | --- | --- | --- | --- |
| `src/system/kernel/arch/sparc/arch_traps.S` | OpenBSD `sys/arch/sparc64/sparc64/locore.s` | BSD 4-clause (Kranenburg, Horvath, UCB and others) | The shape of the window spill, fill and clean-window handlers, and the structure of the TLB miss fast path | Not a verbatim copy. The spill and fill bodies are the obvious sixteen stores and loads that any implementation arrives at, and the miss path was written from the manual's section 15.3.1 refill sequence and verified against this machine before the reference was consulted. Two details *are* taken from OpenBSD and were not in our version: the atomic quad load of the TSB line via `ASI_NUCLEUS_QUAD_LDD`, and testing the TTE Valid bit with `brgez` on the data half. The clean-window sequence follows their `KCLEANWIN` closely. |
| `src/system/kernel/arch/sparc/arch_mmu.cpp` | OpenBSD `sys/arch/sparc64/sparc64/db_interface.c` | BSD 3-clause (UCB / CMU Mach derivation) | Confirmation of the TLB Tag Read and Data Access ASI numbers, and that each read wants a `membar #Sync` after it | The addressing itself is from the manual: `FIGURE 15-13` (printed p.230) puts the entry index in VA<8:3>, which is the `entry << 3` both implementations use. What OpenBSD supplied was corroboration of the four ASI values and the per-read membar, which the manual states less directly. `sparc_dump_tlb()` and the locking code are our own; OpenBSD's equivalent only prints raw words. |
| `src/system/kernel/arch/sparc/arch_asm.S` | OpenBSD `sys/arch/sparc64/sparc64/locore.s` (`cpu_switchto`) | BSD 4-clause (Kranenburg, Horvath, UCB and others) | The shape of the context switch: take a window, `flushw`, then overwrite this window's `%i6` and `%i7` so that `ret; restore` returns onto the incoming thread's stack | Not a copy. Ours is twelve instructions against their several dozen, because OpenBSD's also switches address spaces, contexts and PCB pointers, none of which this port has yet. What was taken is the central idea -- that the switch routine's own window is the pivot -- which is not obvious from the architecture manual and is the reason the routine can be this short. |
| `src/add-ons/kernel/drivers/network/ether/hme/dev/hme/` | FreeBSD `sys/dev/hme/{if_hme.c,if_hmereg.h,if_hmevar.h,if_hme_pci.c}`, release/12.4.0 | BSD 2-clause (The NetBSD Foundation, contributed by Paul Kranenburg; Thomas Moestl) | The whole driver, verbatim | **A copy, not a derivation** -- the licence permits it and keeping it diff-comparable is worth more than restyling. Marked `style-check: donor` so the linter leaves it alone; every Haiku change is marked `Haiku:` in place. From the 12.4 *release* rather than a checkout because FreeBSD deleted this driver in 13 along with sparc64 support -- the file even carries the `gone_in_dev()` call announcing it, which is removed here, since the deprecation is FreeBSD's schedule and not Haiku's. Changes: the FreeBSD 12-to-13 migration Haiku's compatibility layer expects (`DRIVER_MODULE` without a devclass, `if_foreach_llmaddr` instead of walking `if_multiaddrs`), `OF_getetheraddr()` replaced with a lookup of our own, since Haiku has no Open Firmware bus layer, and a `haiku_interrupt_status` field that `hme_intr()` reads instead of the chip -- the Global Status Register is read-to-clear *and* reading it is what deasserts the interrupt, so on a system that services interrupts in a thread the fast handler must read it and hand the value on. `marvell_yukon` carries the identical change under the identical name. **OpenBSD's `hme` was the plan and is the wrong donor**: it is written against OpenBSD's MII API, which Haiku does not have, while Haiku ships a full FreeBSD `miibus`. |

## Anticipated sources

Recorded in advance so the licence question is settled before the code is written, not after.

| Likely source | Licence as verified 2026-08-17 | Standing |
| --- | --- | --- |
| OpenBSD `sys/arch/sparc64/sparc64/pmap.c` | BSD 2-clause, Eduardo Horvath 1996–1999 — **no advertising clause** | Cleanest donor available. Preferred for TSB, TLB demap, context allocation. |
| OpenBSD `sys/arch/sparc64/sparc64/locore.s` | BSD 4-clause — Kranenburg, Horvath, UCB | Usable. UCB rescinded its clause in 1999; the others stand and must be reproduced. |
| OpenBSD `sys/arch/sparc64/sparc64/trap.c`, `clock.c` | BSD 4-clause — as above, plus Ross, Glass | Usable on the same terms. |
| NetBSD `sys/arch/sparc64/` | BSD family, generally a superset of OpenBSD's | Usable. Prefer OpenBSD: leaner, better-licensed, same logic. |
| FreeBSD | BSD 2-clause | Usable. Relevant if `hme` goes through Haiku's existing FreeBSD compat layer. |
| **Linux `arch/sparc`, `drivers/`** | **GPL-2.0** | **Never copied.** Behavioural reference only, and say so explicitly when it informs a decision. |

Haiku itself already ships 125 files carrying the 4-clause advertising notice — the FireWire
stack and `msdosfs` among them — so that licence is settled precedent in this tree rather than
something we would be introducing.

## How to add an entry

When you port something, add a row *in the same commit*, and make sure the file itself carries:

1. The original copyright block, unmodified and complete.
2. Our own header beneath it, in Haiku's two-line MIT form.
3. A comment naming the specific upstream file and revision it derives from.

Ported code keeps the donor's formatting rather than being restyled to Haiku conventions, so it
stays diff-comparable against upstream when the donor fixes a bug. Our own code follows Haiku
style.
