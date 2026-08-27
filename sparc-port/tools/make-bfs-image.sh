#!/usr/bin/env bash
#
# Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
# Distributed under the terms of the MIT License.
#
# Build a BFS volume containing the SPARC kernel and the kernel add-ons needed
# to find a disk, so the Open Firmware loader has something to find and boot.
#
# Haiku's own build/scripts/build_haiku_image does this as part of assembling a
# full system image, which needs all of userland to compile. This does only the
# part the port needs right now: initialize BFS, create system/, copy the kernel
# in, and install the boot-time kernel add-ons.
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
addons="$objects/haiku/sparc/release/add-ons/kernel"

# The add-ons the loader has to preload for the kernel to reach a disk, each
# named by its path below add-ons/kernel. That path is the same in the build
# tree and on the volume, with one difference: the build leaves the binary at
# <path>/<leaf> while the volume wants it at <path>.
#
# This is the same set Haiku's own image gives boot module symlinks to (see
# build/jam/packages/Haiku), minus everything that is for hardware sun4u does
# not have. Each one is here because something else in the list needs it: sabre
# publishes the host bridge the pci bus manager attaches beneath,
# generic_ide_pci binds the CMD646 through ata_adapter, ata presents itself to
# the system as a SCSI bus, scsi_disk drives that bus through scsi_periph, and
# the scsi bus manager defers work to dpc.
addon_paths=(
	busses/pci/sabre
	bus_managers/pci
	bus_managers/ata
	bus_managers/scsi
	busses/ata/cmd646
	busses/ata/generic_ide_pci
	generic/ata_adapter
	generic/dpc
	generic/scsi_periph
	drivers/disk/scsi/scsi_disk
	drivers/disk/scsi/scsi_cd
	file_systems/bfs
	partitioning_systems/intel
)

# Drivers the kernel loads on demand rather than at boot, each named by its path
# below add-ons/kernel. Unlike the list above these are not preloaded by the
# loader: legacy_driver_probe() finds them when something looks a device up under
# /dev, which is why they go in drivers/bin with a symlink under drivers/dev
# naming the directory they publish into.
#
# hme is the Ultra 10's onboard Ethernet -- function 1 of the PCIO chip whose
# function 0 is the EBus bridge.
ondemand_paths=(
	drivers/network/ether/hme
)

# Where each of the above publishes, below drivers/dev.
declare -A ondemand_dev=(
	[drivers/network/ether/hme]=net
)

# Kernel modules that are not drivers and are not preloaded either -- the module
# manager finds them by path when something asks for them by name. A FreeBSD
# compatibility driver asks for network/stack/buffer/v1 before it will attach, so
# without these the driver loads, matches its device, and then releases it again:
#
#   pci_reserve_device(1, 1, 1, hme)
#   module: Search for network/stack/buffer/v1 failed.
#   pci_unreserve_device(1, 1, 1, hme)
#
# which reads like a driver problem and is a missing file.
#
# The three protocol modules below are what turns a link into an address. The
# stack loads them by name when something asks -- ipv4 when a socket is opened
# in that family, arp when an ethernet interface needs to turn an address into a
# station address, icmp when a datagram wants an error back or a ping wants an
# answer -- so a missing one presents as a socket call failing rather than as
# anything about a module.
module_paths=(
	network/stack
	network/datalink_protocols/ethernet_frame
	network/datalink_protocols/arp
	network/devices/ethernet
	network/protocols/ipv4
	network/protocols/icmp
	network/protocols/udp
)

output=bfs.img
size_mb=48
label=Haiku
serial_debug=0
install_addons=1

usage() {
	cat <<EOF
Usage: make-bfs-image.sh [options]

  --kernel FILE   kernel to install as system/kernel_sparc
  --output FILE   BFS image to write, default $output
  --size-mb N     image size, default $size_mb
  --label NAME    volume name, default $label
  --no-add-ons    leave the kernel add-ons out, giving a volume the loader can
                  boot but the kernel cannot find a disk from. Only useful for
                  telling a kernel problem apart from an add-on problem.
  --user-test     build sparc-port/tools/usertest and install it as the volume's
                  launch_daemon, so the kernel's own boot path runs a userland.
                  Freestanding -- no libroot, no runtime_loader -- so it tests
                  the kernel's ELF loading and userspace entry rather than a
                  userland's bootstrap.
  --dynamic-test  install the *real* runtime_loader and libroot.so, and build
                  sparc-port/tools/hellodyn against them as the launch_daemon.
                  The other half of --user-test: that one takes everything out of
                  the way to test the kernel, this one puts it all back.
                  Mutually exclusive with --user-test, which installs a stand-in
                  where the real runtime_loader goes.
  --net-test      as --dynamic-test, but installs sparc-port/tools/hellonet --
                  which brings up the hme interface and pings QEMU's gateway.
                  Needs the network modules, which the image always carries.
  --serial-debug  write a kernel settings file enabling serial_debug_output, so
                  the kernel's early output goes to serial rather than the
                  framebuffer blue screen where nothing can read it.
                  CURRENTLY BROKEN, and off by default: merely having the file
                  present makes the loader die before the kernel, with a
                  mem_address_not_aligned trap on a "call %g1" in
                  of_finddevice, meaning gCallOpenFirmware itself has been
                  corrupted. Reading driver settings damages loader state
                  somehow -- see PROGRESS.md section 15.
  -h, --help
EOF
}

user_test=${user_test:-0}
dynamic_test=${dynamic_test:-0}

# Which program --dynamic-test installs where the launch daemon goes. They are
# built and linked identically; what differs is only what they exercise once the
# loader has done its work, so there is one recipe below rather than one per
# program.
dynamic_program=${dynamic_program:-hellodyn}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--kernel)  kernel="$2"; shift 2 ;;
		--output)  output="$2"; shift 2 ;;
		--size-mb) size_mb="$2"; shift 2 ;;
		--label)   label="$2"; shift 2 ;;
		--no-add-ons) install_addons=0; shift ;;
		--serial-debug) serial_debug=1; shift ;;
		--user-test) user_test=1; shift ;;
		--dynamic-test) dynamic_test=1; shift ;;
		--net-test) dynamic_test=1; dynamic_program=hellonet; shift ;;
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

# All of them up front, so a half-populated image is not what reports the
# missing one. The jam target is the leaf name in every case.
if [[ "$install_addons" == "1" ]]; then
	missing=()
	for path in "${addon_paths[@]}"; do
		[[ -r "$addons/$path/${path##*/}" ]] || missing+=("${path##*/}")
	done
	if [[ ${#missing[@]} -gt 0 ]]; then
		echo "missing kernel add-ons: ${missing[*]}" >&2
		echo "build them with:" >&2
		echo "  cd $repo/generated.sparc && jam -q ${missing[*]}" >&2
		exit 1
	fi
fi

work=$(mktemp -d)

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
	rm -rf "$work"
}
trap cleanup EXIT

# ":" prefixes a host path; unprefixed paths are inside the volume.
shell_command mkdir /myfs/system
shell_command cp -f ":$kernel" /myfs/system/kernel_sparc

# fs_shell's mkdir has no -p, so intermediate directories are created one level
# at a time. It also complains about a directory that already exists, and that
# complaint comes out of the bfs_shell daemon's own stderr rather than the
# client's, so it cannot be redirected away from here -- hence remembering what
# has been created instead of asking the volume.
declare -A created_directories=([system]=1)

volume_mkdir_p() {
	local partial=
	local component
	local IFS=/
	for component in $1; do
		partial="${partial:+$partial/}$component"
		if [[ -z "${created_directories[$partial]:-}" ]]; then
			shell_command mkdir "/myfs/$partial"
			created_directories[$partial]=1
		fi
	done
}

if [[ "$install_addons" == "1" ]]; then
	volume_mkdir_p system/add-ons/kernel/boot

	for path in "${addon_paths[@]}"; do
		leaf="${path##*/}"

		volume_mkdir_p "system/add-ons/kernel/${path%/*}"
		shell_command cp -f ":$addons/$path/$leaf" \
			"/myfs/system/add-ons/kernel/$path"

		# The loader looks in add-ons/kernel/boot first and only falls back to
		# scanning the individual directories if every boot directory it knows
		# about is missing -- and that fallback list is stale, still naming
		# busses/ide where the ATA controller drivers have lived under
		# busses/ata for years. So boot/ is the path that has to work.
		#
		# Symlinks rather than copies, and not for tidiness: the loader skips a
		# file whose inode it has already loaded
		# (src/system/boot/loader/elf.cpp), which is how bfs and intel avoid
		# being loaded a second time when load_modules() finishes by scanning
		# file_systems/ and partitioning_systems/ unconditionally. Copies would
		# have their own inodes and would be loaded twice.
		#
		# The link is relative and resolved from boot/, so three levels up is
		# the system directory. Both halves of that work: BFS stores ".." as a
		# real b+tree entry (Inode.cpp), and the loader's Directory::Lookup
		# traverses links.
		shell_command ln -s "../../../add-ons/kernel/$path" \
			"/myfs/system/add-ons/kernel/boot/$leaf"
	done

	# On-demand drivers, in the layout legacy_drivers.cpp expects: the binary
	# under drivers/bin, and a symlink under drivers/dev in the directory the
	# driver publishes into. legacy_driver_probe() is handed that directory's
	# name when devfs is asked to look something up beneath it, and scans the
	# matching drivers/dev subdirectory for links to try.
	volume_mkdir_p system/add-ons/kernel/drivers/bin

	for path in "${ondemand_paths[@]}"; do
		leaf="${path##*/}"
		dev="${ondemand_dev[$path]}"

		if [[ ! -r "$addons/$path/$leaf" ]]; then
			echo "missing on-demand driver: $leaf" >&2
			echo "  cd $repo/generated.sparc && jam -q $leaf" >&2
			exit 1
		fi

		shell_command cp -f ":$addons/$path/$leaf" \
			"/myfs/system/add-ons/kernel/drivers/bin/$leaf"

		volume_mkdir_p "system/add-ons/kernel/drivers/dev/$dev"
		shell_command ln -s "../../bin/$leaf" \
			"/myfs/system/add-ons/kernel/drivers/dev/$dev/$leaf"

		echo "  drivers/bin/$leaf -> published under /dev/$dev"
	done

	# Modules, at the path the module manager derives from the name they publish.
	for path in "${module_paths[@]}"; do
		leaf="${path##*/}"

		if [[ ! -r "$addons/$path/$leaf" ]]; then
			echo "missing kernel module: $leaf" >&2
			echo "  cd $repo/generated.sparc && jam -q $leaf" >&2
			exit 1
		fi

		volume_mkdir_p "system/add-ons/kernel/${path%/*}"
		shell_command cp -f ":$addons/$path/$leaf" \
			"/myfs/system/add-ons/kernel/$path"
		echo "  $path"
	done
fi

if [[ $dynamic_test -eq 1 ]]; then
	if [[ $user_test -eq 1 ]]; then
		echo "--user-test and --dynamic-test both install a runtime_loader" >&2
		exit 2
	fi

	# The real userland: the kernel enters runtime_loader, which relocates
	# itself, loads libroot.so, relocates that, resolves the program's imports
	# against it, and only then calls main(). Everything --user-test deliberately
	# takes out of the way.
	cc="$repo/generated.sparc/cross-tools-sparc/bin/sparc64-unknown-haiku-gcc"
	glue="$objects/haiku/sparc/release/system/glue"
	libroot_dir="$objects/haiku/sparc/release/system/libroot"
	loader="$objects/haiku/sparc/release/system/runtime_loader/runtime_loader"
	gcc_dir=$("$cc" -print-file-name=crtbeginS.o | xargs dirname)
	libgcc_glob="$repo/generated.sparc/build_packages/gcc_syslibs-*"
	libgcc=$(ls $libgcc_glob/lib/libgcc_s.so.1 2>/dev/null | head -1)

	for file in "$cc" "$loader" "$libroot_dir/libroot.so" \
			"$glue/start_dyn.o" "$glue/init_term_dyn.o" \
			"$glue/arch/sparc/crti.o" "$glue/arch/sparc/crtn.o"; do
		if [[ ! -r "$file" ]]; then
			echo "missing for --dynamic-test: $file" >&2
			echo "  cd $repo/generated.sparc && jam -q runtime_loader libroot.so"\
				"'<sparc>glue_common.o'" >&2
			exit 1
		fi
	done

	dynamic_binary="$(dirname "$output")/$dynamic_program"

	# Haiku's headers are not in the cross compiler's sysroot, so they are named
	# here the same way the build names them. The link order is the one
	# ArchitectureRules calls HAIKU_EXECUTABLE_BEGIN_GLUE_CODE and
	# HAIKU_EXECUTABLE_END_GLUE_CODE, and -soname=_APP_ is what marks an image as
	# an application rather than a library.
	"$cc" -c -o "$dynamic_binary.o" \
		"$repo/sparc-port/tools/$dynamic_program/$dynamic_program.c" \
		-I"$repo/headers" -I"$repo/headers/os" -I"$repo/headers/os/support" \
		-I"$repo/headers/os/kernel" -I"$repo/headers/posix"

	# -z max-page-size matches what ArchitectureRules now gives every other
	# sparc binary: without it the linker leaves 1 MB between text and data,
	# and runtime_loader's map_image() refuses the image as Bad data.
	"$cc" -nostdlib -Xlinker -soname=_APP_ \
		-Xlinker -z -Xlinker max-page-size=0x2000 -o "$dynamic_binary" \
		"$glue/arch/sparc/crti.o" "$gcc_dir/crtbeginS.o" \
		"$glue/start_dyn.o" "$glue/init_term_dyn.o" \
		"$dynamic_binary.o" \
		-L"$libroot_dir" -lroot \
		"$gcc_dir/crtendS.o" "$glue/arch/sparc/crtn.o"

	volume_mkdir_p system/servers
	volume_mkdir_p system/lib

	shell_command cp -f ":$loader" /myfs/system/runtime_loader
	shell_command cp -f ":$libroot_dir/libroot.so" /myfs/system/lib/libroot.so

	# libroot.so is linked against libgcc_s.so.1 -- the unwinder and the software
	# routines the compiler emits calls to -- and runtime_loader resolves that by
	# name at load time, so it has to be on the volume beside it.
	shell_command cp -f ":$libgcc" /myfs/system/lib/libgcc_s.so.1
	shell_command cp -f ":$dynamic_binary" \
		/myfs/system/servers/launch_daemon

	echo "  system/runtime_loader          $(stat -c%s "$loader") bytes"
	echo "  system/lib/libroot.so          $(stat -c%s "$libroot_dir/libroot.so") bytes"
	echo "  system/lib/libgcc_s.so.1       $(stat -c%s "$libgcc") bytes"
	echo "  system/servers/launch_daemon   $(stat -c%s "$dynamic_binary") bytes ($dynamic_program)"
fi

if [[ $user_test -eq 1 ]]; then
	# The kernel's boot path runs /boot/system/servers/launch_daemon, so putting
	# the test there needs no kernel change -- load_image() and everything under
	# it is exercised exactly as it would be for the real thing.
	usertest_source="$repo/sparc-port/tools/usertest/usertest.S"
	usertest_binary="$(dirname "$output")/usertest"
	cc="$repo/generated.sparc/cross-tools-sparc/bin/sparc64-unknown-haiku-gcc"

	if [[ ! -x "$cc" ]]; then
		echo "no cross compiler at $cc" >&2
		exit 1
	fi

	"$cc" -nostdlib -nostartfiles -static -Wl,-e,_start \
		"$usertest_source" -o "$usertest_binary"

	# Installed twice, and the second one is the one that runs.
	#
	# Haiku's kernel never enters a program directly. team_create_thread_start()
	# loads /boot/system/runtime_loader and enters *that*, whatever the executable
	# is, and runtime_loader then loads the program. So a freestanding static
	# binary put where the program goes is never reached -- but put where the
	# loader goes, it is entered by exactly the path the real thing uses.
	#
	# The program file still has to exist for the team to get that far, so the
	# same binary goes in both places.
	volume_mkdir_p system/servers
	shell_command cp -f ":$usertest_binary" /myfs/system/servers/launch_daemon
	shell_command cp -f ":$usertest_binary" /myfs/system/runtime_loader
	usertest_size=$(stat -c%s "$usertest_binary")
	echo "  system/runtime_loader                 $usertest_size bytes (usertest)"
	echo "  system/servers/launch_daemon          $usertest_size bytes (usertest)"
fi

if [[ "$serial_debug" == "1" ]]; then
	# The loader reads driver settings from this exact path
	# (src/system/boot/loader/load_driver_settings.cpp). Without
	# serial_debug_output the kernel's early output -- including any panic --
	# goes to the framebuffer blue screen instead of the serial console.
	settings="$work/kernel-settings"
	cat > "$settings" <<-'SETTINGS'
	serial_debug_output true
	debug_screen true
	SETTINGS
	volume_mkdir_p home/config/settings/kernel/drivers
	shell_command cp -f ":$settings" \
		/myfs/home/config/settings/kernel/drivers/kernel
fi

# Done writing. The listing below comes from a second, interactive bfs_shell
# rather than from the daemon: command output goes to the daemon's stdout, not
# the client's, and the daemon's is not something this script can interleave
# with its own. Reading the finished image back is a better check anyway --
# it is the volume as the loader will see it, not as the writer meant it.
shell_command quit >/dev/null 2>&1 || true
exec 5>&- 6>&- 3<&- 4>&-
wait "$bfs_pid" 2>/dev/null || true
trap 'rm -rf "$work"' EXIT

echo "--- volume contents ---"
{
	echo "ls /myfs/system"
	if [[ "$install_addons" == "1" ]]; then
		echo "ls /myfs/system/add-ons/kernel/boot"
	fi
	echo quit
} | "$bfs_shell" "$output" 2>&1 | grep -v '^fssh:/> *$' | sed 's/^fssh:\/> //'

echo
echo "wrote $output ($size_mb MiB, BFS, label '$label')"
echo "  system/kernel_sparc  $(stat -c%s "$kernel") bytes"
if [[ "$install_addons" == "1" ]]; then
	echo "  ${#addon_paths[@]} kernel add-ons, linked from system/add-ons/kernel/boot"
fi
