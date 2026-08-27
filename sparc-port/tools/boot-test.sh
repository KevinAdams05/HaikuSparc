#!/usr/bin/env bash
#
# Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
# Distributed under the terms of the MIT License.
#
# The whole test loop, in one command: build the media from the current tree,
# boot it headless, drive the console, and leave a log worth grepping.
#
#   boot-test.sh TAG [make-bfs-image.sh args...]
#   boot-test.sh dyn --dynamic-test
#   BOOT_SETTLE=200 boot-test.sh slow --user-test
#
# The porting plan asks for exactly this in section 5.4 -- "a single script
# should take a source tree to a serial log with no interaction, because this
# loop runs hundreds of times" -- and for a long time it existed only as an
# ad-hoc wrapper in a scratch directory. Scratch directories evaporate, and this
# one took two sessions with it. It belongs in the tree with everything else the
# loop depends on.
#
# Leaves, in the work directory:
#
#   TAG.log     the serial console, with the escape sequences OpenBIOS emits
#   TAG.txt     the same, stripped -- this is the one to grep
#   qemu-TAG.log        QEMU's own stderr, which is where "Failed to get write
#                       lock" appears when a previous run is still alive
#   TAG.pcap    every frame the NIC saw, in and out
#   loader-TAG.img, bfs-TAG.img     the media, kept for post-mortems
#   mon-TAG.sock        the monitor, live while the guest is
#
# The monitor socket is the reason to reach for this rather than qemu-sun4u.sh
# directly: a kernel that wedges prints nothing at all, and `info registers` on
# the still-running guest is the only way to find out where. See the tools README.

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

# Not in the repo: these are build products, and a full run of them is a couple
# of hundred megabytes. Overridable so a bisect can keep runs apart.
work=${BOOT_TEST_WORK:-${TMPDIR:-/tmp}/haiku-sparc-boot}

if [[ $# -lt 1 ]]; then
	echo "usage: $(basename "$0") TAG [make-bfs-image.sh args...]" >&2
	exit 2
fi

tag="$1"; shift

mkdir -p "$work"

loader="$work/loader-$tag.img"
bfs="$work/bfs-$tag.img"
log="$work/$tag.log"
text="$work/$tag.txt"
socket="$work/$tag.sock"
monitor="$work/mon-$tag.sock"

# A previous guest still holding the filler images fails the next run with
# "Failed to get write lock", and the error lands in QEMU's log rather than on
# the console -- so it presents as a boot that produced no output at all.
if pgrep -f '[q]emu-system-sparc64' >/dev/null; then
	echo "boot-test: a sparc64 guest is already running; stopping it" >&2
	pkill -9 -f '[q]emu-system-sparc64' || true
	sleep 1
fi

"$here/make-boot-disk.sh" --output "$loader" >/dev/null
"$here/make-bfs-image.sh" --output "$bfs" "$@" | tail -3

# All four IDE slots, always. The ATAPI probe of a device that is not there hangs
# the boot -- see the note at the top of qemu-sun4u.sh, which cost an afternoon.
for filler in "$work/filler0.img" "$work/filler1.img"; do
	[[ -f "$filler" ]] || dd if=/dev/zero of="$filler" bs=1M count=1 status=none
done
# And a floppy, which is not optional either and is a separate trap from the four
# IDE slots. Without one the boot loader gets as far as enumerating disks, walks
# all four of them looking for a partition map, and then stops -- no menu, no
# error, no further output. With one it boots. The floppy is never read; what
# matters is that the firmware has a node to answer for.
[[ -f "$work/blank.fd" ]] \
	|| dd if=/dev/zero of="$work/blank.fd" bs=1k count=1440 status=none

rm -f "$socket" "$monitor" "$log"

"$here/qemu-sun4u.sh" \
	--disk "$loader" --disk "$bfs" \
	--disk "$work/filler0.img" --disk "$work/filler1.img" \
	--serial "$socket" --monitor "$monitor" \
	--pcap "$work/$tag.pcap" \
	--timeout "${BOOT_QEMU_TIMEOUT:-0}" \
	-- -fda "$work/blank.fd" \
	</dev/null >"$work/qemu-$tag.log" 2>&1 &
qemu_pid=$!

trap 'kill $qemu_pid 2>/dev/null || true' EXIT

for _ in $(seq 50); do
	[[ -S "$socket" ]] && break
	sleep 0.1
done
if [[ ! -S "$socket" ]]; then
	echo "boot-test: QEMU never opened $socket -- see $work/qemu-$tag.log" >&2
	tail -3 "$work/qemu-$tag.log" >&2 || true
	exit 1
fi

# Newline-separated, so an argument may contain spaces.
extra=()
if [[ -n "${BOOT_EXTRA:-}" ]]; then
	while IFS= read -r line; do
		[[ -n "$line" ]] && extra+=("$line")
	done <<<"${BOOT_EXTRA}"
fi

python3 "$here/serial-driver.py" --socket "$socket" --log "$log" \
	--script "${BOOT_SCRIPT:-boot-kernel-debug}" \
	--timeout "${BOOT_TIMEOUT:-240}" --step-timeout 150 --quiet --timestamps \
	"${extra[@]}" || true

# The scripted steps end at the `boot` command; everything interesting happens
# after them. Keep capturing, then stop.
#
# Deliberately a plain sleep rather than a match on some expected line: the runs
# worth having this script for are the ones that stop saying anything, and a
# script that waits for output it will never see reports a timeout instead of a
# log.
sleep "${BOOT_SETTLE:-60}"

# Stripped copy. OpenBIOS drives the console with real escape sequences, and a
# grep for a kernel message will otherwise miss the lines that happen to have one
# in the middle.
sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' "$log" >"$text"

printf 'boot-test: %s\n' "$text"
printf '  %s panic(s), %s winfixup(s), %s line(s)\n' \
	"$(grep -c 'PANIC' "$text" || true)" \
	"$(grep -c 'winfixup' "$text" || true)" \
	"$(grep -c '' "$text" || true)"

# A pcap with only its 24-byte header means the NIC transmitted nothing, which is
# worth saying out loud rather than leaving to be discovered by an empty decode.
if [[ -f "$work/$tag.pcap" ]]; then
	pcap_size=$(stat -c%s "$work/$tag.pcap")
	if [[ "$pcap_size" -le 24 ]]; then
		printf '  %s: no frames\n' "$work/$tag.pcap"
	else
		printf '  %s: %s bytes captured\n' "$work/$tag.pcap" "$pcap_size"
	fi
fi
