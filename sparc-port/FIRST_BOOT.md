# First boot on real hardware

What to do the day a machine arrives, in order, and why each step is there. Written before there is
any hardware, from the manuals and from what this port actually depends on — so it is a checklist to
be corrected on contact, not a record of anything that has happened.

**Companion documents:** [Porting plan](PORTING_PLAN.md) · [Hardware matrix](HARDWARE_MATRIX.md) ·
[Progress log](PROGRESS.md)

---

## 1. Before power-on

| | |
| --- | --- |
| Cable | null-modem DE-9, plus a USB-serial adapter for the build host |
| Port | **ttya** (serial A), the one nearest the parallel port on an Ultra 10 |
| Terminal | **9600 baud, 8 data bits, no parity, 1 stop bit** |
| Capture | log to a file from the first byte — `screen -L`, `minicom -C`, or `cu` piped to `tee` |

9600,8,n,1 is the OpenBoot default for `ttya-mode`, and the manuals say so in as many words: "the
terminal should be configured for 9600 baud operation, 8 bits, no parity" (Solaria 650 Technical
Reference Manual, printed p.110 for the NVRAM defaults and p.131 for the terminal).

**Capture from the first byte, not once something interesting happens.** The failures this port
expects on first contact are the ones that produce a few lines and then silence, and the few lines are
the whole diagnosis.

## 2. Put the console on serial, explicitly

At the `ok` prompt:

```
setenv input-device ttya
setenv output-device ttya
reset-all
```

**Do this rather than relying on the keyboard-absence convention.** A keyboard-less Sun does put its
console on ttya, and that convention is real, but it is Sun's rather than IEEE 1275's — the standard
only says the console is chosen from `input-device` and `output-device` after probing (IEEE 1275-1994
§, printed p.89). Setting them costs one line and removes a variable.

It matters more here than it looks. This port's kernel reaches the serial line through Open Firmware's
own stdout — `arch_debug_serial_putchar()` calls `SparcPlatform::SerialDebugPutChar()`, which is an
`of_write()` — and `SparcOpenFirmware::InitSerialDebug()` **deliberately suppresses that output when
the frame buffer is enabled and OF's stdout is a display**, to avoid writing to a screen Haiku is also
drawing on. So a machine consoled to a monitor gives a silent kernel *by design*. Serial console is
not a convenience here; it is the contract.

## 3. Capture the machine's identity before changing anything else

```
banner
.properties
show-devs
printenv
devalias
```

Save the output into `TestHardware/` alongside the x86 machines' `lspci`/`dmidecode` dumps. Phase 9
asks for exactly this on day one, and it is the reference every later "OpenBIOS did it differently"
question gets answered against.

**The kernel captures its own version of this automatically.** Every boot log already contains the
full Open Firmware device tree — node names, device types, PCI vendor:device ids, `reg`, `interrupts`,
and full `interrupt-map`/`interrupt-map-mask` cell values, from
`sparc_dump_openfirmware_devices()`. So the firmware's view and the kernel's view of the same tree can
be diffed, which is the fastest way to find a property the kernel is reading wrongly.

## 4. Expect a dead NVRAM battery

It is soldered in these machines and they are all old enough. Symptoms: an IDPROM checksum complaint
at power-on, a bogus or all-zero MAC address, and a lost `input-device`/`output-device` after every
power cycle — so step 2 may need repeating each time.

**The `hme` driver survives this**, and not by accident: it looks for a station address in four places
in order — `local-mac-address` on the device's own node, `mac-address` on `/chosen`, the root node's
`idprom`, then the Forth word `mac-address`. Real OpenBoot publishes `local-mac-address` (OpenBIOS does
not, which is why the idprom path is the one QEMU exercises), so the first place should answer before
the idprom is consulted at all. The idprom path verifies the checksum and declines rather than
inventing an address from bad bytes.

## 5. Boot it

`make-boot-disk.sh` prints the `boot` command for the media it writes. The loader goes on IDE index 0
and the BFS volume on index 1, the same arrangement `boot-test.sh` uses under QEMU.

Netboot is the faster loop and is **untried**: it fails under OpenBIOS with `Trying net...` then "No
valid state has been set by load or init-program", and whether OpenBIOS implements TFTP at all is an
open question (PROGRESS §14). Real OpenBoot certainly does, so it is worth trying early — but on the
machine, not before it.

## 6. What working looks like

Four tests, each installed where the launch daemon goes, ordered by how much they put between
themselves and the thing they test:

| | expect | if it stops before that |
| --- | --- | --- |
| `boot-test.sh hw --user-test` | `usertest deep`, `winok`, `sig`, `ok` | before `deep`: ELF entry or the spill path. `deep` with no `winok`: a live window is not surviving a trap — the failure mode three trap-entry bugs had |
| `--dynamic-test` | `hellodyn ok, argc 1` | the loader, relocations, or libroot |
| `--net-test` | `hellonet ok` | it names the step: create, address, up, send, receive |
| `--sig-test` | `hellosig ok` | it prints the elapsed time; 2.5s instead of 2.0s means the restart forgot its deadline |

`--serial-debug` adds a settings file that routes kernel output to serial. The boot menu's *Debug
Options* reaches the same place without a file on the volume; both work, and on hardware the file is
one less thing to type.

## 7. What QEMU could not test, and is therefore first to break

Everything here was written from the manual and has never executed on silicon.

| | what it is | how it fails |
| --- | --- | --- |
| `arch_cpu_sync_icache()` | `flush` per doubleword after writing a PLT entry, with `MEMBAR #StoreStore` first | a call through a freshly bound PLT entry executes stale instructions — a jump to nothing, intermittent |
| memory barriers | `#LoadLoad` / `#StoreStore` | only matter if `PSTATE.MM` is not TSO, which nothing here sets |
| TLB/TSB locking | trap handlers, trap table, trap data block and the TSB in locked entries | a miss inside the miss handler, under memory pressure, a long way into a boot |
| CMD646 interrupt latch | CFR bit 2 at config `0x50`, ARTTIM23 bit 4 at `0x57`, cleared by being read | QEMU does not implement read-to-clear, so this code has never done anything. On silicon that does, an unclearable latch is an interrupt that will not go away |
| Erratum 51 | a MEMBAR in a delay slot can deadlock, unrecoverably when `pstate.ie==0` | no assembly in this port has one; keep it that way |
| firmware property names | 19 of them | each now names itself when missing — that was the point of the audit in PROGRESS §55 |

## 8. Known broken, so do not chase them

- **`debug_screen true` hangs the kernel** during PCI initialisation, at the point the bus manager
  maps I/O space, every time. That flag makes every `dprintf()` also call `blue_screen_puts()`, so all
  kernel output goes through the frame buffer console. Bisected against `serial_debug_output`, which
  is fine alone. The frame buffer console is Phase 8's subject.
- **The Blade 150 cannot reach a disk.** Its ALi M5229 IDE controller has no driver and no emulator.
  The Ultra 10's CMD646 and `hme` are both written and exercised. See the note added to
  [HARDWARE_MATRIX §8](HARDWARE_MATRIX.md#8-sourcing-recommendation).
- **The Installer, the HTTP kit, printing and `setmime` do not build**, on a `gcc_syslibs` version
  mismatch that cannot be fixed by bumping the package pin. None of it is on the path to booting
  (PROGRESS §55).
