#!/usr/bin/env python3
#
# Copyright 2026, Kevin Adams <kevinadams05@gmail.com>
# Distributed under the terms of the MIT License.
#
"""Build a Sun-disklabelled disk image that Open Firmware can boot.

Open Firmware will not boot an arbitrary disk image. On SPARC it expects a Sun
disk label (a VTOC) in sector 0, describing partitions in units of cylinders,
and it locates what to boot relative to a partition rather than to the start of
the device. Haiku's image build emits nothing of the sort, which is why its own
documentation says the generated filesystem image is not sufficient to boot a
SPARC machine.

The label is a fixed 512-byte structure, big-endian throughout:

    offset   0  128 bytes  ASCII label text
    offset 420    2 bytes  rpm
    offset 422    2 bytes  physical cylinders
    offset 424    2 bytes  alternates per cylinder
    offset 430    2 bytes  interleave
    offset 432    2 bytes  data cylinders
    offset 434    2 bytes  alternate cylinders
    offset 436    2 bytes  tracks per cylinder
    offset 438    2 bytes  sectors per track
    offset 444  8 x 8      partition table: (start cylinder, sector count)
    offset 508    2 bytes  magic, 0xDABE
    offset 510    2 bytes  checksum

The checksum is chosen so that the XOR of all 256 big-endian 16-bit words in
the sector is zero.

Usage:
  make-sun-image.py --payload LOADER --output disk.img
  make-sun-image.py --payload LOADER --output disk.img --payload-offset 512
  make-sun-image.py --inspect disk.img
"""

import argparse
import os
import struct
import sys


SECTOR_SIZE = 512
LABEL_MAGIC = 0xDABE
NUM_PARTITIONS = 8

OFF_TEXT = 0
OFF_RPM = 420
OFF_PCYL = 422
OFF_APC = 424
OFF_INTRLV = 430
OFF_NCYL = 432
OFF_ACYL = 434
OFF_NTRACKS = 436
OFF_NSECTORS = 438
OFF_PARTITIONS = 444
OFF_MAGIC = 508
OFF_CHECKSUM = 510


def build_label(text, cylinders, tracks, sectors, partitions):
    """Return a 512-byte Sun disk label.

    partitions is a list of (start_cylinder, sector_count), padded to eight.
    """
    label = bytearray(SECTOR_SIZE)

    encoded = text.encode("ascii", "replace")[:127]
    label[OFF_TEXT:OFF_TEXT + len(encoded)] = encoded

    struct.pack_into(">H", label, OFF_RPM, 3600)
    struct.pack_into(">H", label, OFF_PCYL, cylinders)
    struct.pack_into(">H", label, OFF_APC, 0)
    struct.pack_into(">H", label, OFF_INTRLV, 1)
    struct.pack_into(">H", label, OFF_NCYL, cylinders)
    struct.pack_into(">H", label, OFF_ACYL, 0)
    struct.pack_into(">H", label, OFF_NTRACKS, tracks)
    struct.pack_into(">H", label, OFF_NSECTORS, sectors)

    entries = list(partitions) + [(0, 0)] * (NUM_PARTITIONS - len(partitions))
    for index, (start, count) in enumerate(entries[:NUM_PARTITIONS]):
        struct.pack_into(">II", label, OFF_PARTITIONS + index * 8, start, count)

    struct.pack_into(">H", label, OFF_MAGIC, LABEL_MAGIC)

    # The checksum makes the XOR of every 16-bit word in the sector zero.
    struct.pack_into(">H", label, OFF_CHECKSUM, 0)
    checksum = 0
    for offset in range(0, SECTOR_SIZE, 2):
        checksum ^= struct.unpack_from(">H", label, offset)[0]
    struct.pack_into(">H", label, OFF_CHECKSUM, checksum)

    return bytes(label)


def verify_label(label):
    """True if magic and checksum are both right."""
    if struct.unpack_from(">H", label, OFF_MAGIC)[0] != LABEL_MAGIC:
        return False
    checksum = 0
    for offset in range(0, SECTOR_SIZE, 2):
        checksum ^= struct.unpack_from(">H", label, offset)[0]
    return checksum == 0


def inspect(path):
    with open(path, "rb") as handle:
        label = handle.read(SECTOR_SIZE)
    if len(label) < SECTOR_SIZE:
        print("file is shorter than one sector")
        return 1

    magic = struct.unpack_from(">H", label, OFF_MAGIC)[0]
    print("magic          0x%04X %s" % (magic,
        "(ok)" if magic == LABEL_MAGIC else "(EXPECTED 0xDABE)"))
    print("checksum       %s" % ("ok" if verify_label(label) else "BAD"))
    print("text           %s"
        % label[:128].split(b"\0")[0].decode("ascii", "replace"))

    cylinders = struct.unpack_from(">H", label, OFF_NCYL)[0]
    tracks = struct.unpack_from(">H", label, OFF_NTRACKS)[0]
    sectors = struct.unpack_from(">H", label, OFF_NSECTORS)[0]
    print("geometry       %d cyl x %d trk x %d sec = %d sectors"
        % (cylinders, tracks, sectors, cylinders * tracks * sectors))

    for index in range(NUM_PARTITIONS):
        start, count = struct.unpack_from(">II", label,
            OFF_PARTITIONS + index * 8)
        if count == 0:
            continue
        offset = start * tracks * sectors
        print("partition %s    start cyl %-5d sectors %-9d byte offset %d"
            % ("abcdefgh"[index], start, count, offset * SECTOR_SIZE))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--payload",
        help="file to place in partition a, normally the Open Firmware loader")
    parser.add_argument("--output", help="disk image to write")
    parser.add_argument("--inspect", metavar="IMAGE",
        help="decode and validate the label of an existing image")
    parser.add_argument("--size-mb", type=int, default=64,
        help="image size in MiB, default 64")
    parser.add_argument("--sectors", type=int, default=63,
        help="sectors per track, default 63")
    parser.add_argument("--tracks", type=int, default=16,
        help="tracks per cylinder, default 16")
    parser.add_argument("--payload-offset", type=int, default=0,
        help="byte offset of the payload within partition a, default 0")
    parser.add_argument("--start-cylinder", type=int, default=0,
        help="first cylinder of partition a, default 0")
    parser.add_argument("--label-text", default="Haiku sparc64 boot",
        help="ASCII label text")
    args = parser.parse_args()

    if args.inspect:
        return inspect(args.inspect)

    if not args.payload or not args.output:
        parser.error("--payload and --output are both required")

    sectors_per_cylinder = args.tracks * args.sectors
    total_sectors = args.size_mb * 1024 * 1024 // SECTOR_SIZE
    cylinders = total_sectors // sectors_per_cylinder
    total_sectors = cylinders * sectors_per_cylinder

    with open(args.payload, "rb") as handle:
        payload = handle.read()

    partition_start_sector = args.start_cylinder * sectors_per_cylinder
    payload_sector = partition_start_sector + args.payload_offset // SECTOR_SIZE
    needed = payload_sector * SECTOR_SIZE + len(payload)
    if needed > total_sectors * SECTOR_SIZE:
        parser.error("payload does not fit; raise --size-mb")

    # Partition a spans the whole disk from its start cylinder, which is what
    # `boot disk:a` will open. Partition c conventionally covers the entire
    # disk and is what many tools expect to find.
    partitions = [
        (args.start_cylinder,
            (cylinders - args.start_cylinder) * sectors_per_cylinder),
        (0, 0),
        (0, total_sectors),
    ]

    image = bytearray(total_sectors * SECTOR_SIZE)
    image[payload_sector * SECTOR_SIZE:
        payload_sector * SECTOR_SIZE + len(payload)] = payload
    image[0:SECTOR_SIZE] = build_label(args.label_text, cylinders,
        args.tracks, args.sectors, partitions)

    with open(args.output, "wb") as handle:
        handle.write(image)

    print("wrote %s: %d MiB, %d cyl x %d trk x %d sec"
        % (args.output, len(image) // (1024 * 1024), cylinders, args.tracks,
            args.sectors))
    print("  partition a starts at cylinder %d (byte %d)"
        % (args.start_cylinder, partition_start_sector * SECTOR_SIZE))
    print("  payload %s at byte %d, %d bytes"
        % (os.path.basename(args.payload), payload_sector * SECTOR_SIZE,
            len(payload)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
