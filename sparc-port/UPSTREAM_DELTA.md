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

Nothing else. As of the initial commit, `git diff --name-only master...sparc/main` returns only
`README.md` and `sparc-port/`; not one Haiku file is modified.

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
