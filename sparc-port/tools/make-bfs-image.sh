#!/usr/bin/env bash
#
# Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
# Distributed under the terms of the MIT License.
#
# Build a BFS volume containing the SPARC kernel, so the Open Firmware loader
# has something to find and boot.
#
# Haiku's own build/scripts/build_haiku_image does this as part of assembling a
# full system image, which needs all of userland to compile. This does only the
# part the port needs right now: initialize BFS, create system/, and copy the
# kernel in.
#
# bfs_shell is a daemon rather than a command-line tool. It talks to
# fs_shell_command over a pair of FIFOs on file descriptors 3 and 4, and the
# handshake below is the same one build_haiku_image performs.
#
# Endianness note: the host tool runs little-endian, so the volume it writes is
# little-endian BFS. That is correct -- Haiku's BFS is built
# BFS_LITTLE_ENDIAN_ONLY by default, so the big-endian SPARC loader byte-swaps
# on read (see src/add-ons/kernel/file_systems/bfs/bfs_endian.h).

set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)

objects="$repo/generated.sparc/objects"
bfs_shell="$objects/linux/x86_64/release/tools/bfs_shell/bfs_shell"
fs_shell_command="$objects/linux/x86_64/release/tools/fs_shell/fs_shell_command"
kernel="$objects/haiku/sparc/release/system/kernel/kernel_sparc"

output=bfs.img
size_mb=48
label=Haiku

usage() {
	cat <<EOF
Usage: make-bfs-image.sh [options]

  --kernel FILE   kernel to install as system/kernel_sparc
  --output FILE   BFS image to write, default $output
  --size-mb N     image size, default $size_mb
  --label NAME    volume name, default $label
  -h, --help
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--kernel)  kernel="$2"; shift 2 ;;
		--output)  output="$2"; shift 2 ;;
		--size-mb) size_mb="$2"; shift 2 ;;
		--label)   label="$2"; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		*) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
	esac
done

for tool in "$bfs_shell" "$fs_shell_command"; do
	if [[ ! -x "$tool" ]]; then
		echo "missing host tool: $tool" >&2
		echo "build them with:" >&2
		echo "  cd $repo/generated.sparc && jam -q '<build>bfs_shell' '<build>fs_shell_command'" >&2
		exit 1
	fi
done
[[ -r "$kernel" ]] || { echo "kernel not found: $kernel" >&2; exit 1; }

rm -f "$output"
dd if=/dev/zero of="$output" bs=1M count="$size_mb" status=none

# block_size 2048 matches what Haiku's own image build uses.
"$bfs_shell" --initialize "$output" "$label" "block_size 2048" >/dev/null

fifo_base="/tmp/make-bfs-image-$$-fifo"
to_shell="${fifo_base}-to-shell"
from_shell="${fifo_base}-from-shell"
rm -f "$to_shell" "$from_shell"
mkfifo "$to_shell" "$from_shell"

# Opening one end of a FIFO blocks until the other end is opened, hence the
# throwaway sleeps holding each end briefly.
sleep 3<"$from_shell" 1 &
exec 6>"$from_shell" 3<"$from_shell"
sleep 5<"$to_shell" 1 &
exec 4>"$to_shell" 5<"$to_shell"
rm -f "$to_shell" "$from_shell"

shell_command() { "$fs_shell_command" 3<&3 4>&4 5>&- 6>&- "$@"; }

"$bfs_shell" -n 3>&5 4<&6 "$output" >/dev/null &
bfs_pid=$!
sleep 1

cleanup() {
	shell_command quit >/dev/null 2>&1 || true
	exec 5>&- 6>&- 3<&- 4>&- 2>/dev/null || true
	wait "$bfs_pid" 2>/dev/null || true
}
trap cleanup EXIT

# ":" prefixes a host path; unprefixed paths are inside the volume.
shell_command mkdir /myfs/system
shell_command cp -f ":$kernel" /myfs/system/kernel_sparc
echo "--- volume contents ---"
shell_command ls /myfs/system

echo
echo "wrote $output ($size_mb MiB, BFS, label '$label')"
echo "  system/kernel_sparc  $(stat -c%s "$kernel") bytes"
