#!/usr/bin/env python3

import argparse
import binascii
import struct
from pathlib import Path


SECTOR_SIZE = 512
BOOT_MAGIC = 0x524F434B
DEFAULT_LOAD_ADDR = 0x80000000
DEFAULT_ENTRY_ADDR = 0x80000000
DEFAULT_VERSION = 1


def parse_int(value):
    return int(value, 0)


def align_up(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def make_header(payload_size, load_addr, entry_addr, crc32, version):
    header = struct.pack(
        "<6I",
        BOOT_MAGIC,
        payload_size,
        load_addr,
        entry_addr,
        crc32,
        version,
    )
    return header + bytes(SECTOR_SIZE - len(header))


def main():
    parser = argparse.ArgumentParser(
        description="Create a Microphase A7 sdboot header and optional SD payload image."
    )
    parser.add_argument("bin", type=Path, help="payload image binary")
    parser.add_argument(
        "--load-addr",
        type=parse_int,
        default=DEFAULT_LOAD_ADDR,
        help="payload load address, default: 0x80000000",
    )
    parser.add_argument(
        "--entry-addr",
        type=parse_int,
        default=DEFAULT_ENTRY_ADDR,
        help="payload entry address, default: 0x80000000",
    )
    parser.add_argument(
        "--version",
        type=parse_int,
        default=DEFAULT_VERSION,
        help="image version field, default: 1",
    )
    parser.add_argument(
        "--crc32",
        action="store_true",
        help="write payload CRC32 into the header; default is 0, which disables the boot-time check",
    )
    parser.add_argument(
        "--header-out",
        type=Path,
        help="header output path, default: <bin>.hdr",
    )
    parser.add_argument(
        "--image-out",
        type=Path,
        help="combined header+payload output path, default: <bin>.sdimg",
    )
    parser.add_argument(
        "--header-only",
        action="store_true",
        help="only write the 512-byte header",
    )
    args = parser.parse_args()

    payload = args.bin.read_bytes()
    payload_size = len(payload)
    crc32 = binascii.crc32(payload) & 0xFFFFFFFF if args.crc32 else 0

    header = make_header(
        payload_size=payload_size,
        load_addr=args.load_addr,
        entry_addr=args.entry_addr,
        crc32=crc32,
        version=args.version,
    )

    header_out = args.header_out or args.bin.with_suffix(args.bin.suffix + ".hdr")
    header_out.write_bytes(header)

    print(f"payload:    {args.bin}")
    print(f"size:       0x{payload_size:x} bytes")
    print(f"load_addr:  0x{args.load_addr:08x}")
    print(f"entry_addr: 0x{args.entry_addr:08x}")
    print(f"crc32:      0x{crc32:08x}")
    print(f"version:    0x{args.version:x}")
    print(f"header:     {header_out}")

    if not args.header_only:
        image_out = args.image_out or args.bin.with_suffix(args.bin.suffix + ".sdimg")
        padded_size = align_up(payload_size, SECTOR_SIZE)
        image_out.write_bytes(header + payload + bytes(padded_size - payload_size))
        print(f"sd image:   {image_out}")
        print("write with: dd if={} of=/dev/sdX bs=512 seek=34 conv=notrunc".format(image_out))


if __name__ == "__main__":
    main()
