# Tools

Development harness for the SPARC port. Everything here is ours; nothing upstream depends on it.

## `qemu-sun4u.sh`

Launches QEMU's `sun4u` machine, which is modelled on the Sun Ultra 5/10 — one of our two target
machines. Serial is the only console, so its output *is* the debugging channel.

```sh
./qemu-sun4u.sh                                   # OpenBIOS to the ok prompt
./qemu-sun4u.sh --disk haiku-sparc.img            # boot from disk
./qemu-sun4u.sh --cpu iie --disk haiku-sparc.img  # ...as a Blade 150
./qemu-sun4u.sh --kernel haiku_loader.openfirmware --timeout 0
./qemu-sun4u.sh --gdb --timeout 0                 # freeze at reset, wait for gdb
./qemu-sun4u.sh --log boot.log                    # tee serial to a file
```

`--timeout` defaults to 60 seconds so an unattended run can never hang a script; a timeout
expiry exits 0 because that is the normal end of a scripted boot, not a failure. Use
`--timeout 0` for interactive sessions. `ctrl-a x` quits, `ctrl-a c` reaches the QEMU monitor.

### The recipe that actually boots

```sh
./make-boot-disk.sh --output loader.img          # the loader, on ext2 + a Sun label
./make-bfs-image.sh --output bfs.img             # the kernel and its add-ons, on BFS

./qemu-sun4u.sh --timeout 0 \
    --serial /tmp/ser.sock --monitor /tmp/mon.sock \
    --disk loader.img --disk bfs.img --disk junk.img --disk junk.img &
./serial-driver.py --socket /tmp/ser.sock --log boot.log \
    --script boot-kernel-debug --timestamps
```

`--disk` is repeatable and the order is load-bearing: index 0 and 1 are IDE channel 0's master
and slave, 2 and 3 are channel 1's, and the loader has to be on index 0 with the BFS volume on
index 1.

**Fill all four slots.** The ATAPI probe of a device that is not there hangs the boot — twenty-five
minutes produced no further output — and a real ISO 9660 image in the CD-ROM rather than a
zero-filled file makes no difference, so this is the ATAPI path itself and not the media. Any two
files will do for the spare slots. That bug is still open.

**Close QEMU's stdin when the console is on a socket**, which `--serial` now does for you. With
`-nographic` and an explicit `-serial`, the *monitor* lands on stdio, and a monitor that reads EOF
takes the machine down with it — which presents as a serial socket that produces nothing at all.

**Take `--monitor` even when nothing is wrong.** `info registers` and `x/24i <pc>` on a *running*
guest is what distinguishes a hung kernel from a merely slow one, and sampling twice a couple of
seconds apart settles it in one step. It is also how a physical address was shown to be a real
register rather than a hole in the address space: write it, read it back.

### Why these particular QEMU settings

- **`--cpu iii` / `--cpu iie`** — QEMU models both our targets by name: `TI UltraSparc IIi`
  (Ultra 10) and `TI UltraSparc IIe` (Blade 150), each with the expected 8 register windows.
  Test against both rather than a generic sun4u.
- **`-nic user,model=sunhme`** — the sun4u machine *already* instantiates an onboard sunhme, so
  this configures it. Adding one with `-device sunhme` fails with
  `PCI: no slot/function available`. sunhme is the real Ultra 10 NIC, so a driver written
  against it is not throwaway work.
- **`--tftp DIR`** — serves `DIR` from QEMU's built-in TFTP server, for exercising the netboot
  path in Phase 1 without standing up real network infrastructure.
- **cmd646-ide** — QEMU emulates the Ultra 5/10's exact IDE controller, so `--disk` exercises
  the real target part. The Blade 150's ALi M5229 is *not* emulated and needs hardware.

### Gotchas worth remembering

QEMU's `-nographic` serial output misbehaves when stdout is a pipe that closes early — piping
into `head` produces *no output at all* rather than a truncated log. Use `--log FILE`, or
`--serial`, or redirect to a file, and grep afterwards.

`-d int -D FILE` logs every trap with the complete register file, window state and `tbr`, and it
is the instrument for anything to do with traps or windows (PROGRESS §14). Be ready for the size:
ninety seconds of a kernel spinning produced **39 GB**. It is greppable and worth it — that trace
is what showed a register window arriving corrupt from a *fill* rather than being written over —
but delete it afterwards.

## `make-boot-disk.sh`

Builds a bootable Sun-disklabelled disk image containing the loader, and prints the `boot`
command for it. This is the **working** boot path — see PROGRESS.md §4.

```sh
./make-boot-disk.sh --output haiku-sparc.img
./qemu-sun4u.sh --disk haiku-sparc.img --timeout 0
# at the ok prompt:
#   boot /pci@1fe,0/pci@1,1/ide@3/ide@0/disk@0:a,\loader.elf
```

It exists because the recipe has three requirements that are invisible until they bite:

1. The payload must be the **ELF** (`boot_loader_openfirmware`), not the a.out wrapper.
2. The ext2 filesystem must be **revision 0** — `mkfs.ext2 -r 0`. Anything newer has features
   OpenBIOS's grubfs cannot read, and the boot fails with a bare `File not found`.
3. Partition a must **not start at cylinder 0**, or the payload overwrites the disk label.

## `make-bfs-image.sh`

Builds the BFS volume the loader boots from: `system/kernel_sparc`, and optionally a kernel
settings file.

```sh
./make-bfs-image.sh --output bfs.img
./make-bfs-image.sh --output bfs.img --kernel /path/to/kernel_sparc
./make-bfs-image.sh --output bfs.img --user-test        # put a userland on it
./make-bfs-image.sh --output bfs.img --serial-debug     # see the warning below
```

Needs two host tools built first, and will tell you so if they are missing:

```sh
cd ../../generated.sparc && jam -q '<build>bfs_shell' '<build>fs_shell_command'
```

`bfs_shell` is a daemon rather than a command-line tool — it talks to `fs_shell_command` over a
pair of FIFOs on descriptors 3 and 4 — so this script performs the same handshake
`build/scripts/build_haiku_image` does, without needing all of userland to compile.

**`--serial-debug` is off by default because it currently breaks the boot.** It writes a settings
file enabling `serial_debug_output`, which should send the kernel's early output to serial instead
of the frame buffer. Merely having the file present makes the *loader* die first, with
`mem_address_not_aligned` on a `call %g1` in `of_finddevice` — `gCallOpenFirmware` itself is
corrupt, so reading driver settings damages loader state. See PROGRESS.md §15. The working route
to serial output is the boot menu, via `serial-driver.py --script boot-kernel-debug`.

Endianness is not a problem here, though it looks like it should be: the host tool writes a
little-endian volume, Haiku's BFS is built `BFS_LITTLE_ENDIAN_ONLY`, and the big-endian SPARC
loader byte-swaps on read.

### `--user-test`, and where it installs

Builds `usertest/usertest.S` with the cross compiler and puts it on the volume **twice** — as
`system/servers/launch_daemon` and as `system/runtime_loader`. The second one is the one that runs,
and the reason is worth knowing before reading a boot log:

**Haiku's kernel never enters the program.** `team_create_thread_start()` loads
`/boot/system/runtime_loader` and enters *that*, whatever the executable is; `runtime_loader` then
loads the program. So a freestanding binary installed where the program goes is never reached, and
the only symptom is

```
error starting "/boot/system/servers/launch_daemon" error = -1
```

with no ELF diagnostic anywhere — because the loader that is missing is not the file the message
names. Installed where the loader goes, the same binary is entered by exactly the path the real
loader will use. The program file still has to exist for team creation to get that far, hence both
copies.

A successful run prints two lines on the same serial console as the kernel:

```
usertest sig
usertest ok
```

`sig` comes from a `SIGUSR1` handler the program registered and sent itself, `ok` from after the
handler returned — so the order is the test. Silence after the boot's last kernel message means the
window machinery or the ELF entry failed; `sig` with no `ok` means delivery worked and the return
did not.

## `make-sun-image.py`

Builds and inspects Sun disk labels; `make-boot-disk.sh` uses it. The label is a fixed 512-byte
big-endian VTOC with magic `0xDABE` at offset 508 and a checksum chosen so the XOR of all 256
16-bit words is zero.

```sh
python3 make-sun-image.py --payload fs.ext2 --output disk.img --start-cylinder 1
python3 make-sun-image.py --inspect disk.img
```

## `gdb-kernel.sh`

Boots the kernel under QEMU with gdb attached and the serial console driven automatically. The
workhorse for Phase 2.

```sh
./gdb-kernel.sh                                    # interactive
./gdb-kernel.sh --interrupt-on 'Unhandled Exception' \
    --batch 'continue; info registers pc; bt'      # scripted
./gdb-kernel.sh --cpu iie                          # as a Blade 150
```

**Read PROGRESS.md §14 before relying on this.** In short: `set endian big` is mandatory or every
register reads byte-swapped; the kernel's symbols load with `symbol-file`, *not*
`add-symbol-file … 0x80000000`; QEMU halts on an unhandled trap without telling gdb, hence
`--interrupt-on`; and **breakpoints on kernel addresses never fire**, software or hardware. For
trap and MMU work use QEMU's tracing instead:

```sh
qemu-system-sparc64 ... -d int,mmu -D trace.log
```

which logs every trap with the complete register file, window state and `tbr`.

## `serial-driver.py`

Drives the Open Firmware prompt and Haiku's boot menu over a Unix socket, so serial is available
while gdb owns the terminal. Steps wait on patterns rather than sleeping, because the loader's
device scan takes a variable number of seconds.

```sh
serial-driver.py --socket /tmp/ser.sock --log serial.log --script boot-kernel
```

Scripts: `boot-kernel` (all the way into the kernel), `boot-loader` (stop at the menu),
`of-prompt`. Extend any of them with `--expect`/`--send` pairs.

Two things it has to compensate for: the menu embeds ANSI escapes *inside* the strings worth
matching on (`Current: <ESC>[0mHaiku`), and those escapes straddle socket reads, so patterns are
matched against a stripped copy with partial escapes carried between chunks.

## `style-check.py`

Haiku coding-style checker scoped to this fork's own code — full files for what we wrote, only
changed lines for upstream files we edit, and cosmetic rules for our own code alone. Rules come
from Haiku's own `src/tools/checkstyle/checkstyle.py`, including its real 100-column limit.

```sh
python3 style-check.py               # our delta vs master
python3 style-check.py --list-rules
python3 style-check.py --self-test   # 25 cases against the checker itself
```

Exit status is 0 when clean, so it can gate a build or a push. See PROGRESS.md §11 for why it is
scoped this way.

## Host requirements

Already present on the build host and verified working:

| | |
| --- | --- |
| `qemu-system-sparc` 8.2.2 | provides `/usr/bin/qemu-system-sparc64` |
| `qemu-system-data` | provides `/usr/share/qemu/openbios-sparc64`, OpenBIOS v1.1 |

Still needed:

```sh
sudo apt install flex bison gawk    # to build the Haiku cross toolchain
sudo apt install gdb-multiarch      # the stock gdb has no sparc:v9 target
```
