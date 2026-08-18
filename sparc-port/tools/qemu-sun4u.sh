#!/usr/bin/env bash
#
# Launch the QEMU sun4u machine for Haiku/SPARC development.
#
# The sun4u machine is modelled on the Sun Ultra 5/10, which is one of our two
# target machines. QEMU emulates both target CPUs by name, the Ultra 10's exact
# IDE controller (cmd646-ide) and its exact NIC (sunhme), so what works here has
# a real chance of working on the hardware.
#
# Serial is the only console. Everything this prints is the debugging channel.
#
# Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
# Distributed under the terms of the MIT License.

set -euo pipefail

QEMU=${QEMU:-/usr/bin/qemu-system-sparc64}
BIOS=${BIOS:-/usr/share/qemu/openbios-sparc64}

cpu_key=iii
memory=512
timeout_secs=60
disk=
cdrom=
kernel=
logfile=
use_gdb=0
gdb_port=1234
tftp_dir=
extra=()

usage() {
	cat <<'EOF'
Usage: qemu-sun4u.sh [options] [-- extra qemu args]

  --cpu iii|iie      Target CPU. iii = UltraSPARC IIi (Ultra 10, default)
                                 iie = UltraSPARC IIe (Blade 150)
  --memory MB        Guest RAM, default 512
  --disk FILE        Attach FILE as an IDE disk on the cmd646 controller
  --cdrom FILE       Attach FILE as an IDE CD-ROM
  --kernel FILE      Boot an ELF directly, bypassing boot media (early bring-up)
  --tftp DIR         Serve DIR over the built-in TFTP server, for netboot
  --gdb [PORT]       Freeze at reset and wait for gdb (default port 1234)
  --timeout SECS     Kill after SECS, default 60. 0 disables.
  --log FILE         Tee all serial output to FILE as well as the terminal
  -h, --help         This text

Examples:
  qemu-sun4u.sh                                  # boot OpenBIOS to the ok prompt
  qemu-sun4u.sh --disk haiku-sparc.img           # boot from a disk image
  qemu-sun4u.sh --cpu iie --disk haiku-sparc.img # as a Blade 150
  qemu-sun4u.sh --kernel haiku_loader.openfirmware --timeout 0
  qemu-sun4u.sh --gdb --kernel objects/.../kernel_sparc   # then: see gdb-attach.sh
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--cpu)     cpu_key="$2"; shift 2 ;;
		--memory)  memory="$2"; shift 2 ;;
		--disk)    disk="$2"; shift 2 ;;
		--cdrom)   cdrom="$2"; shift 2 ;;
		--kernel)  kernel="$2"; shift 2 ;;
		--tftp)    tftp_dir="$2"; shift 2 ;;
		--timeout) timeout_secs="$2"; shift 2 ;;
		--log)     logfile="$2"; shift 2 ;;
		--gdb)
			use_gdb=1
			if [[ ${2-} =~ ^[0-9]+$ ]]; then gdb_port="$2"; shift; fi
			shift ;;
		-h|--help) usage; exit 0 ;;
		--)        shift; extra=("$@"); break ;;
		*)         echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
	esac
done

case "$cpu_key" in
	iii) cpu="TI UltraSparc IIi"; model="Ultra 5/10" ;;
	iie) cpu="TI UltraSparc IIe"; model="Blade 100/150" ;;
	*)   echo "--cpu must be iii or iie" >&2; exit 2 ;;
esac

[[ -x "$QEMU" ]] || { echo "not found: $QEMU" >&2; exit 1; }
[[ -r "$BIOS" ]] || { echo "not found: $BIOS  (install qemu-system-data)" >&2; exit 1; }

args=(
	-M sun4u
	-cpu "$cpu"
	-m "$memory"
	-bios "$BIOS"
	-nographic
)

# The sun4u machine already instantiates an onboard sunhme, so this configures
# that NIC rather than adding a second one -- adding one fails with
# "no slot/function available". sunhme is what a real Ultra 10 carries, so the
# driver we eventually write against it is not throwaway. The user-mode backend
# also gives us a built-in TFTP server for netboot testing.
nic="user,model=sunhme"
[[ -n "$tftp_dir" ]] && nic+=",tftp=${tftp_dir}"
args+=(-nic "$nic")

[[ -n "$disk"   ]] && args+=(-drive "file=${disk},format=raw,if=ide,media=disk")
[[ -n "$cdrom"  ]] && args+=(-drive "file=${cdrom},format=raw,if=ide,media=cdrom")
[[ -n "$kernel" ]] && args+=(-kernel "$kernel")

if [[ $use_gdb -eq 1 ]]; then
	args+=(-S -gdb "tcp::${gdb_port}")
fi

[[ ${#extra[@]} -gt 0 ]] && args+=("${extra[@]}")

echo "=== qemu sun4u | ${cpu} (${model}) | ${memory}MB ===" >&2
if [[ $use_gdb -eq 1 ]]; then
	echo "=== frozen at reset, waiting for gdb on port ${gdb_port} ===" >&2
fi
echo "=== ctrl-a x to quit, ctrl-a c for the qemu monitor ===" >&2

run() {
	if [[ "$timeout_secs" != "0" ]]; then
		# 124 from timeout is the normal end of an unattended run, not a failure.
		timeout --foreground "$timeout_secs" "$QEMU" "${args[@]}" || {
			rc=$?; [[ $rc -eq 124 ]] && exit 0; exit $rc
		}
	else
		exec "$QEMU" "${args[@]}"
	fi
}

if [[ -n "$logfile" ]]; then
	mkdir -p "$(dirname "$logfile")"
	run 2>&1 | tee "$logfile"
else
	run
fi
