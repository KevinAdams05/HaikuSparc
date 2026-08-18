#!/usr/bin/env bash
#
# Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
# Distributed under the terms of the MIT License.
#
# Boot the kernel under QEMU with gdb attached.
#
# This is the workhorse for Phase 2. The trap table, the register-window
# spill/fill handlers and the TLB-miss fast path cannot be debugged from printf:
# they run with no usable stack, and a mistake in the window state registers
# kills the machine with no diagnostic at all. The gdb stub is the only way to
# see %tba, CANSAVE, CANRESTORE and the TSB directly.
#
# Serial goes to a Unix socket, driven by serial-driver.py, because gdb needs the
# terminal and the boot menu needs keystrokes.
#
# Two settings are non-obvious and both are mandatory:
#
#   set architecture sparc:v9   gdb cannot infer it from a bare remote target
#   set endian big              WITHOUT THIS EVERY REGISTER READS BYTE-SWAPPED.
#                               The reset PC shows as 0x200000f0ff010000 rather
#                               than 0x1fff0000020, and you will chase ghosts.

set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)

kernel="$repo/generated.sparc/objects/haiku/sparc/release/system/kernel/kernel_sparc"
loader_image=
bfs_image=
port=1234
cpu="TI UltraSparc IIi"
memory=512
work=/tmp/haiku-sparc-gdb-$$
script=boot-kernel
batch=
interrupt_on=
interrupt_delay=3
filler_iso_arg=
filler_fd_arg=

usage() {
	cat <<EOF
Usage: gdb-kernel.sh [options] [-- extra gdb args]

  --loader IMAGE   Sun-labelled loader disk (default: built fresh)
  --bfs IMAGE      BFS disk with system/kernel_sparc (default: built fresh)
  --kernel FILE    kernel ELF for symbols, default the one just built
  --port N         gdb stub port, default $port
  --cpu iii|iie    target CPU, default iii (UltraSPARC IIi)
  --script NAME    serial-driver script, default $script
  --batch CMDS     run these gdb commands then exit, instead of interactive
  --interrupt-on PATTERN
                   watch the serial log and interrupt the target once PATTERN
                   appears. QEMU halts the CPU on an unhandled trap but does
                   NOT tell gdb, so a plain "continue" would block forever;
                   this is how you get control at a fault.
  --interrupt-delay N   seconds to wait after the match, default $interrupt_delay
  -h, --help

The kernel is loaded by the loader, not by QEMU, so its symbols are added at
KERNEL_LOAD_BASE (0x80000000) once it is in memory. Breakpoints on kernel
addresses only become meaningful after the loader has jumped.

Examples:
  gdb-kernel.sh
  gdb-kernel.sh --batch 'info registers pc npc; bt'
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--loader) loader_image="$2"; shift 2 ;;
		--bfs)    bfs_image="$2"; shift 2 ;;
		--kernel) kernel="$2"; shift 2 ;;
		--port)   port="$2"; shift 2 ;;
		--cpu)    case "$2" in
				iii) cpu="TI UltraSparc IIi" ;;
				iie) cpu="TI UltraSparc IIe" ;;
				*) echo "--cpu must be iii or iie" >&2; exit 2 ;;
			esac; shift 2 ;;
		--script) script="$2"; shift 2 ;;
		--batch)  batch="$2"; shift 2 ;;
		--interrupt-on)    interrupt_on="$2"; shift 2 ;;
		--interrupt-delay) interrupt_delay="$2"; shift 2 ;;
		--filler-iso)      filler_iso_arg="$2"; shift 2 ;;
		--filler-floppy)   filler_fd_arg="$2"; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		--) shift; break ;;
		*) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
	esac
done

command -v gdb-multiarch >/dev/null \
	|| { echo "need gdb-multiarch (apt install gdb-multiarch)" >&2; exit 1; }
[[ -r "$kernel" ]] || { echo "kernel not found: $kernel" >&2; exit 1; }

mkdir -p "$work"
socket="$work/serial.sock"
serial_log="$work/serial.log"

if [[ -z "$loader_image" ]]; then
	loader_image="$work/loader.img"
	"$here/make-boot-disk.sh" --output "$loader_image" >/dev/null
fi
if [[ -z "$bfs_image" ]]; then
	bfs_image="$work/bfs.img"
	"$here/make-bfs-image.sh" --kernel "$kernel" --output "$bfs_image" >/dev/null
fi

# Empty removable drives make OpenBIOS misbehave: opening an empty CD-ROM traps
# inside the firmware, and scanning an empty floppy hangs. Give both media.
filler_iso="${filler_iso_arg:-$work/filler.iso}"
filler_fd="${filler_fd_arg:-$work/filler.fd}"
if [[ -z "$filler_iso_arg" ]]; then
	# The placeholder needs actual content: an ISO built from an empty file
	# still trips the firmware's empty-media bug, which presents as a trap at
	# 0xffd1c184 during the loader's device scan.
	mkdir -p "$work/isoroot"
	head -c 65536 /dev/urandom > "$work/isoroot/placeholder"
	xorriso -as mkisofs -quiet -o "$filler_iso" "$work/isoroot" 2>/dev/null \
		|| dd if=/dev/zero of="$filler_iso" bs=1M count=4 status=none
fi
if [[ -z "$filler_fd_arg" ]]; then
	dd if=/dev/zero of="$filler_fd" bs=1024 count=1440 status=none
fi

cleanup() {
	[[ -n "${qemu_pid:-}" ]] && kill "$qemu_pid" 2>/dev/null || true
	[[ -n "${driver_pid:-}" ]] && kill "$driver_pid" 2>/dev/null || true
	[[ -n "${watcher_pid:-}" ]] && kill "$watcher_pid" 2>/dev/null || true
}
trap cleanup EXIT

echo "=== serial log: $serial_log"
echo "=== gdb stub:   localhost:$port"

/usr/bin/qemu-system-sparc64 -M sun4u -cpu "$cpu" -m "$memory" -nographic \
	-bios /usr/share/qemu/openbios-sparc64 \
	-chardev "socket,id=s0,path=$socket,server=on,wait=off" -serial chardev:s0 \
	-drive "file=$loader_image,format=raw,if=ide,index=0,media=disk" \
	-drive "file=$bfs_image,format=raw,if=ide,index=1,media=disk" \
	-drive "file=$filler_iso,format=raw,if=ide,index=2,media=cdrom" \
	-fda "$filler_fd" \
	-gdb "tcp::$port" \
	>"$work/qemu.log" 2>&1 &
qemu_pid=$!

python3 "$here/serial-driver.py" --socket "$socket" --log "$serial_log" \
	--script "$script" --timeout 900 --step-timeout 150 --quiet \
	>"$work/driver.log" 2>&1 &
driver_pid=$!

gdb_args=(
	-q
	-ex "set confirm off"
	-ex "set pagination off"
	-ex "set architecture sparc:v9"
	-ex "set endian big"
	-ex "target remote localhost:$port"
	# symbol-file, NOT add-symbol-file with an offset. The kernel is an ELF DYN
	# but it is *linked* at KERNEL_LOAD_BASE (0x80000000) -- nm shows absolute
	# 0x8000xxxx addresses -- and the loader honours that. Passing 0x80000000 to
	# add-symbol-file relocates on top of it and every symbol gdb prints is then
	# silently wrong: 0x800f36c0 resolves to inet_ntop rather than
	# create_debug_alloc_pool.
	-ex "symbol-file $kernel"
	-ex "echo \n=== attached; kernel symbols loaded at their link addresses ===\n"
	-ex "echo === the loader runs first; 'continue' until it jumps ===\n\n"
)

if [[ -n "$batch" ]]; then
	IFS=';' read -ra commands <<< "$batch"
	for command in "${commands[@]}"; do
		gdb_args+=(-ex "$command")
	done
	gdb_args+=(-batch)
fi

gdb_args+=("$@")

if [[ -n "$interrupt_on" ]]; then
	# gdb must be backgrounded so its pid is known; SIGINT is how gdb is told to
	# interrupt the inferior, and in batch mode it then carries on with the
	# remaining -ex commands, which is exactly the "stop and inspect" we want.
	gdb-multiarch "${gdb_args[@]}" &
	gdb_pid=$!

	(
		for _ in $(seq 1 600); do
			if [[ -r "$serial_log" ]] && grep -qE "$interrupt_on" "$serial_log"; then
				sleep "$interrupt_delay"
				kill -INT "$gdb_pid" 2>/dev/null
				exit 0
			fi
			sleep 0.5
		done
	) &
	watcher_pid=$!

	wait "$gdb_pid" || true
else
	gdb-multiarch "${gdb_args[@]}" || true
fi

echo
echo "=== serial log kept at $serial_log"
