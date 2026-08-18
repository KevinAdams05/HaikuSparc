#!/usr/bin/env bash
#
# Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
# Distributed under the terms of the MIT License.
#
# Build a bootable Sun-disklabelled disk image containing the Open Firmware
# loader, and print the `boot` command that starts it.
#
# This wraps a recipe with three non-obvious requirements, each of which was
# found by having it fail:
#
#   1. The payload must be the ELF (boot_loader_openfirmware), not the a.out
#      wrapper (haiku_loader.openfirmware). Open Firmware has an elf-loader
#      package; nothing loads the a.out.
#   2. The ext2 filesystem must be revision 0. Anything newer has features
#      OpenBIOS's grubfs cannot read, and the boot fails with a bare
#      "File not found" that tells you nothing.
#   3. Partition a must not start at cylinder 0, or the payload overwrites the
#      disk label in sector 0.

set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)

built=generated.sparc/objects/haiku/sparc/release/system/boot/openfirmware
loader="$repo/$built/boot_loader_openfirmware"
output=haiku-sparc.img
fs_mb=16
disk_mb=64
name=loader.elf

usage() {
	cat <<EOF
Usage: make-boot-disk.sh [options]

  --loader FILE   loader ELF to install (default: the one just built)
  --output FILE   disk image to write, default $output
  --name NAME     filename inside the image, default $name
  --disk-mb N     disk image size, default $disk_mb
  --fs-mb N       filesystem size, default $fs_mb
  -h, --help
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--loader)  loader="$2"; shift 2 ;;
		--output)  output="$2"; shift 2 ;;
		--name)    name="$2"; shift 2 ;;
		--disk-mb) disk_mb="$2"; shift 2 ;;
		--fs-mb)   fs_mb="$2"; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		*) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
	esac
done

[[ -r "$loader" ]] || { echo "loader not found: $loader" >&2; exit 1; }
for tool in mkfs.ext2 debugfs; do
	command -v "$tool" >/dev/null || { echo "missing: $tool (install e2fsprogs)" >&2; exit 1; }
done

if ! head -c4 "$loader" | grep -q $'\x7fELF'; then
	echo "warning: $loader is not an ELF; Open Firmware will refuse it" >&2
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fs="$work/fs.ext2"

dd if=/dev/zero of="$fs" bs=1M count="$fs_mb" status=none
# -r 0 is load-bearing; see the header comment.
mkfs.ext2 -q -F -r 0 -b 1024 "$fs"
debugfs -w -R "write $loader $name" "$fs" >/dev/null 2>&1

python3 "$here/make-sun-image.py" --payload "$fs" --output "$output" \
	--start-cylinder 1 --size-mb "$disk_mb"

echo
echo "Boot it with:"
echo "  $here/qemu-sun4u.sh --disk $output --timeout 0"
echo "then at the ok prompt:"
echo "  boot /pci@1fe,0/pci@1,1/ide@3/ide@0/disk@0:a,\\$name"
