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

### Gotcha worth remembering

QEMU's `-nographic` serial output misbehaves when stdout is a pipe that closes early — piping
into `head` produces *no output at all* rather than a truncated log. Use `--log FILE`, or
redirect to a file, and grep afterwards.

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
