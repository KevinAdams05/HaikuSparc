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
| *(nothing yet)* | | | | |

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
