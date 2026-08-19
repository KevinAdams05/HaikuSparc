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
