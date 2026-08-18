# Upstream delta

Changes this fork makes to **shared, cross-architecture Haiku files** — the ones upstream also
edits. This is the entire merge-conflict risk surface, so the goal is to keep this list short
enough to read in one sitting.

See [§7 of the porting plan](PORTING_PLAN.md#7-staying-in-sync-with-upstream).

## The two classes

**Class A — inside `*/arch/sparc/*` or `sparc-port/`.** Not tracked here. Upstream touches those
paths only during tree-wide sweeps, so conflicts are rare and shallow. Target: 95% of our
commits.

**Class B — everything else.** Tracked here, one row per change, each in its own labelled commit.

## Ledger

| Files | Commit | Why | Upstreamable? |
| --- | --- | --- | --- |
| `README.md` | `91c5724` | Repo landing page. A new path — Haiku's own file is `ReadMe.md` — so it can never conflict. | No, and never needs to. |
| `src/system/boot/platform/openfirmware/devices.cpp` | *(loader build fix)* | `printf("%ld", status)` where `status_t` is `int`. Harmless on the 32-bit PowerPC OF port; a hard `-Werror=format=` failure on 64-bit SPARC. | **Yes** — plain 64-bit portability bug. |
| `src/system/boot/platform/openfirmware/network.cpp` | *(loader build fix)* | `memcpy` into a `mac_addr_t`, which has a user-defined copy constructor and so is not trivially copyable — `-Werror=class-memaccess`. Now copies into the `address` array member. | **Yes** — and it is the Sun-specific `mac-address` path, so it matters to us directly. |
| `src/system/boot/platform/openfirmware/video.cpp` | *(loader build fix)* | `platform_init_video()` held an `edid1_info` on the stack *and* memcpy'd it to a `kernel_args` allocation, costing 1840 bytes against a 1023-byte budget. Now decodes straight into the allocation, dropping both the stack use and a redundant copy. | **Yes** — smaller and simpler on every platform. |
| `src/system/boot/loader/menu.cpp` | *(loader build fix)* | `alloca(128)` inside a conditional made the function's stack usage unbounded to the compiler. Replaced with a function-scoped 128-byte array. | **Yes** — but note this file is shared by *every* architecture, making it the highest-risk row here. |

### Why these four exist

The Open Firmware bootloader **does not build from upstream master** — not for SPARC, and by
inspection not for PowerPC either. Haiku's own guide says the loader works, which was evidently
true at some earlier point. Three of the four failures are architecture-neutral and would fail
any modern-GCC build of this loader; only the `%ld` one is genuinely 64-bit-specific.

`-Wstack-usage=1023` is applied to the `openfirmware` boot target and no other
(`build/jam/ArchitectureRules:554`). That is deliberate — the loader runs on the firmware's own
small stack — so the right response was to fix the stack consumers rather than to raise the
limit for SPARC, even though SPARC's mandatory 176-byte register-window save area does make
every frame inherently larger.

**Follow-up:** `menu.cpp` is cross-architecture and has not yet been compile-tested on x86_64.
Do that before the first upstream submission.

## Decisions that keep this list short

**Ticket #19597 — deliberately not implemented.** Implementing separate kernel/user address
spaces would mean changing `IS_USER_ADDRESS` and `user_memcpy` for every architecture: the
single largest Class B change this port could possibly make. The TTE Global bit
(UltraSPARC-IIi manual, FIGURE 15-1, printed p.205) lets us use a shared address space with
kernel mappings marked Global and user mappings context-tagged, which requires **no shared-code
changes at all**. Revisit only if profiling ever justifies it. See
[§4.3](PORTING_PLAN.md#43-kernel-and-user-address-spaces--ticket-19597).

**Documentation lives in `sparc-port/`, not `docs/`.** Haiku has its own `docs/` tree; sharing
directories would manufacture merge noise for no benefit.

## Checking the delta at any time

```sh
git diff --name-only master...sparc/main                    # our entire delta
git diff --name-only master...sparc/main -- . ':!sparc-port' ':!README.md'   # Class B only
```

The second command should print nothing, or only rows that appear in the table above. If it
prints something undocumented, either add the row or reconsider the change.

## Before merging upstream

```sh
git fetch upstream master
git diff --name-only master...sparc/main            > /tmp/ours
git diff --name-only master..upstream/master        > /tmp/theirs
comm -12 <(sort /tmp/ours) <(sort /tmp/theirs)      # the intersection is the risk
```

An empty intersection means the merge is mechanical. Anything listed wants a human before
merging — and `rerere` is enabled, so any conflict resolved once is replayed automatically
afterwards.
