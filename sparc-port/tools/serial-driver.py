#!/usr/bin/env python3
#
# Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
# Distributed under the terms of the MIT License.
#
"""Drive the QEMU serial console over a Unix socket, logging everything.

The Open Firmware prompt and Haiku's boot menu are both interactive, and both
live on the only console the machine has. When gdb owns the terminal, serial
needs to go somewhere else -- hence a socket, and this to talk to it.

Steps are given as "wait" / "send" pairs so a session is reproducible instead
of a hand-timed sequence of keystrokes. Waiting on a pattern rather than
sleeping is what makes it reliable: the loader's timing varies by seconds
depending on how long the device scan takes.

Usage:
  serial-driver.py --socket PATH --log FILE [--script NAME] [--timeout SECS]

Scripts:
  boot-kernel   boot the loader, pick the BFS volume, and boot the kernel
  boot-loader   boot the loader only, and stop at its menu
  of-prompt     just capture the Open Firmware prompt

Any --expect/--send pairs given on the command line are appended to the script,
so a canned script can be extended for a one-off experiment.
"""

import argparse
import os
import re
import socket
import sys
import time


# Cursor keys, as the Open Firmware console decodes them: ESC [ A/B/C/D.
KEY_UP = "\x1b[A"
KEY_DOWN = "\x1b[B"
ENTER = "\r"

# The menu also accepts vi keys (text_menu.cpp), which are single bytes and so
# far more robust over a serial line than three-byte escape sequences.
UP = "k"
DOWN = "j"

# Navigation clamps at both ends rather than wrapping -- select_next_valid_item
# falls back to last_selectable_item -- so a burst of one key reliably parks the
# selection on the first or last entry regardless of how many items a menu has.
# That matters because the Debug Options menu's length varies with build
# options.
TO_TOP = UP * 12
TO_BOTTOM = DOWN * 12

BOOT_DEVICE = "/pci@1fe,0/pci@1,1/ide@3/ide@0/disk@0"
BOOT_COMMAND = "boot %s:a,\\loader.elf" % BOOT_DEVICE

# The boot menu is drawn with ANSI positioning and colour, and it puts escape
# sequences *inside* the strings we want to match on -- "Current: <ESC>[0mHaiku"
# is the one that bites. Patterns are matched against a stripped copy of the
# stream while the log keeps the raw bytes.
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]|\x1b\][^\x07]*\x07|[\x00\x0c]")

# QEMU emits this continuously once the menu starts polling for keypresses.
# It is harmless, but it swamps both the log and the match buffer.
NOISE_RE = re.compile(r"pc_serial_read: bad len[^\n]*\n?")

# Each step is (pattern to wait for, text to send). A pattern of None sends
# immediately; a send of None just waits.
SCRIPTS = {
    "of-prompt": [
        (r"0 >", None),
    ],
    "boot-loader": [
        (r"0 >", BOOT_COMMAND + "\n"),
        (r"Welcome to the Haiku Boot Loader", None),
    ],
    "boot-kernel": [
        (r"0 >", BOOT_COMMAND + "\n"),
        # The menu redraws several times while devices are scanned; wait for the
        # main menu, then open the volume list, take the only volume, and boot.
        (r"Select boot volume/state", ENTER),
        # The volume list opens with the only volume already highlighted, and
        # picking it returns to the main menu -- which then grows a "Continue
        # booting" entry, itself already highlighted. So three Enters in total.
        (r"Select Boot Volume/State", ENTER),
        (r"Continue booting", ENTER),
        (r"load kernel", None),
    ],
    # Same, but turns on serial_debug_output first, so the kernel's early
    # output -- including any panic -- comes to the serial console instead of
    # the framebuffer blue screen where nothing can read it. Going through the
    # menu passes the flag in kernel_args and avoids the driver settings file,
    # which currently corrupts the loader (see PROGRESS.md section 15).
    "boot-kernel-debug": [
        (r"0 >", BOOT_COMMAND + "\n"),
        (r"Select boot volume/state", ENTER),
        (r"Select Boot Volume/State", ENTER),
        # Main menu: park at the top, then down twice to "Select debug options".
        (r"Continue booting", TO_TOP + DOWN + DOWN + ENTER),
        # "Enable serial debug output" is the first entry and is markable, so
        # Enter toggles it and stays put. Then park at the bottom, which is
        # "Return to main menu".
        (r"Debug Options", ENTER + TO_BOTTOM + ENTER),
        (r"Continue booting", TO_BOTTOM + ENTER),
        (r"load kernel", None),
    ],
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True, help="QEMU serial Unix socket")
    parser.add_argument("--log", required=True, help="file to write all output to")
    parser.add_argument("--script", default="boot-kernel", choices=sorted(SCRIPTS))
    parser.add_argument("--timeout", type=float, default=180.0,
        help="overall timeout in seconds")
    parser.add_argument("--step-timeout", type=float, default=90.0,
        help="how long to wait for any single pattern")
    parser.add_argument("--expect", action="append", default=[],
        help="extra pattern to wait for, paired with --send")
    parser.add_argument("--send", action="append", default=[],
        help="extra text to send (\\r and \\e are interpreted)")
    parser.add_argument("--timestamps", action="store_true",
                        help="prefix each logged line with seconds since start, "
                             "which is the only way to tell how long the target "
                             "spent between two messages")
    parser.add_argument("--quiet", action="store_true",
        help="do not echo to stdout")
    args = parser.parse_args()

    steps = list(SCRIPTS[args.script])
    for index, pattern in enumerate(args.expect):
        text = args.send[index] if index < len(args.send) else None
        if text is not None:
            text = text.replace("\\r", "\r").replace("\\n", "\n") \
                .replace("\\e", "\x1b")
        steps.append((pattern, text))

    deadline = time.monotonic() + args.timeout

    connection = None
    for _ in range(60):
        try:
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            connection.connect(args.socket)
            break
        except OSError:
            connection = None
            time.sleep(0.25)
    if connection is None:
        print("could not connect to %s" % args.socket, file=sys.stderr)
        return 1
    connection.settimeout(0.25)

    # A single growing buffer with a cursor, rather than a buffer that gets
    # cleared after each match. Clearing loses output that pump() already read
    # ahead of the current step -- which is exactly what happens here, because
    # the loader redraws its menu faster than the steps advance. The cursor also
    # stops an earlier menu draw from satisfying a later pattern.
    seen = ""
    search_from = 0
    carry = ""
    log = open(args.log, "w", buffering=1, errors="replace")
    started = time.time()
    pending = [""]

    def write_log(text):
        """Writes to the log, optionally stamping each completed line.

        Stamps go on line boundaries rather than on each chunk, because a chunk
        is whatever recv() happened to return and can end mid-word. Whatever is
        left over is carried to the next call.
        """
        if not args.timestamps:
            log.write(text)
            return

        pending[0] += text
        while "\n" in pending[0]:
            line, pending[0] = pending[0].split("\n", 1)
            log.write("[%8.3f] %s\n" % (time.time() - started, line))

    def pump():
        """Read whatever is available; return False once the socket closes."""
        nonlocal seen
        try:
            chunk = connection.recv(65536)
        except socket.timeout:
            return True
        except OSError:
            return False
        if not chunk:
            return False
        text = chunk.decode("latin1")
        text = NOISE_RE.sub("", text)
        write_log(text)
        if not args.quiet:
            sys.stdout.write(text)
            sys.stdout.flush()
        nonlocal search_from, carry
        # An escape sequence can straddle a recv() boundary, and a half-stripped
        # sequence leaves literal "[0m" litter in the middle of the very strings
        # we match on. Hold back any trailing partial escape for the next chunk.
        text = carry + text
        carry = ""
        partial = re.search(r"\x1b\[?[0-9;]*$", text)
        if partial is not None and partial.start() > 0:
            carry = text[partial.start():]
            text = text[:partial.start()]
        seen += ANSI_RE.sub("", text)
        if len(seen) > 1048576:
            dropped = len(seen) - 524288
            seen = seen[dropped:]
            search_from = max(0, search_from - dropped)
        return True

    status = 0
    for pattern, text in steps:
        expression = re.compile(pattern)
        step_deadline = min(time.monotonic() + args.step_timeout, deadline)
        match = expression.search(seen, search_from)
        while match is None:
            if time.monotonic() > step_deadline:
                print("\n[serial-driver] timed out waiting for %r"
                    % pattern, file=sys.stderr)
                tail = " ".join(seen[search_from:].split())[-400:]
                print("[serial-driver] unmatched tail: ...%s" % tail,
                    file=sys.stderr)
                status = 1
                break
            if not pump():
                print("\n[serial-driver] serial closed while waiting for %r"
                    % pattern, file=sys.stderr)
                status = 1
                break
            match = expression.search(seen, search_from)
        if status != 0:
            break
        search_from = match.end()
        print("\n[serial-driver] matched %r" % pattern, file=sys.stderr)
        if text is not None:
            # Let the console settle: it drops input that arrives while it is
            # still redrawing.
            time.sleep(1.5)
            connection.sendall(text.encode("latin1"))

    # Keep draining until the overall deadline, so the log captures whatever
    # happens after the last step -- which is usually the interesting part.
    while time.monotonic() < deadline:
        if not pump():
            break

    log.close()
    connection.close()
    return status


if __name__ == "__main__":
    sys.exit(main())
