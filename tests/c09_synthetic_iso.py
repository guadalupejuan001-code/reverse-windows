#!/usr/bin/env python3
"""Create a self-authored, synthetic El Torito x86 test ISO, never Windows media."""
from __future__ import annotations

import argparse
import pathlib
import struct

SECTOR = 2048
CATALOG = 20
BOOT = 24
DATA = 26


def u16(b: bytearray, at: int, value: int) -> None:
    struct.pack_into("<H", b, at, value)


def u32(b: bytearray, at: int, value: int) -> None:
    struct.pack_into("<I", b, at, value)


def u64(b: bytearray, at: int, value: int) -> None:
    struct.pack_into("<Q", b, at, value)


def desc(base: int, limit: int, access: int) -> int:
    return (
        (limit & 0xFFFF)
        | ((base & 0xFFFFFF) << 16)
        | (access << 40)
        | (((limit >> 16) & 15) << 48)
        | (((base >> 24) & 255) << 56)
    )


def generate() -> bytearray:
    iso = bytearray(32 * SECTOR)
    for lba, kind in ((16, 1), (17, 0), (18, 255)):
        at = lba * SECTOR
        iso[at] = kind
        iso[at + 1 : at + 6] = b"CD001"
        iso[at + 6] = 1
    iso[17 * SECTOR + 7 : 17 * SECTOR + 30] = b"EL TORITO SPECIFICATION"
    u32(iso, 17 * SECTOR + 0x47, CATALOG)
    at = CATALOG * SECTOR
    iso[at] = 1
    iso[at + 30 : at + 32] = b"\x55\xaa"
    iso[at + 32] = 0x88
    u16(iso, at + 34, 0x2000)  # Boot CS:IP = 2000:0000
    u16(iso, at + 38, 16)      # Boot image load = 16 * 512 bytes
    u32(iso, at + 40, BOOT)
    total = sum(struct.unpack_from("<H", iso, at + i)[0] for i in range(0, 32, 2))
    u16(iso, at + 28, (-total) & 0xFFFF)
    boot = BOOT * SECTOR

    # Real guest BIOS AH42: load CD sector DATA into physical 0x30000.
    # Real PCI configuration I/O CF8/CFC: read C07 IDE ID from 0:2.0.
    # Real ATA PIO: issue IDENTIFY on 1F6/1F7 to exercise controller.
    # Then transition into protected16 at 0050:0854 and execute 0F00 LTR.
    stub = bytes((
        0xFA,                         # CLI
        0x0E, 0x1F,                   # PUSH CS; POP DS
        0xBE, 0x00, 0x01,             # MOV SI,0100 (DAP)
        0xB4, 0x42,                   # MOV AH,42 (CD extended read)
        0xCD, 0x13,                   # INT 13
        0x66, 0xB8, 0x00, 0x10, 0x00, 0x80,  # MOV EAX,80001000
        0xBA, 0xF8, 0x0C,             # MOV DX,CF8
        0x66, 0xEF,                   # OUT DX,EAX
        0xBA, 0xFC, 0x0C,             # MOV DX,CFC
        0x66, 0xED,                   # IN EAX,DX
        0x66, 0xA3, 0x00, 0x03,       # MOV [0300],EAX (guest PCI ID)
        0xBA, 0xF6, 0x01,             # MOV DX,1F6
        0xB0, 0xE0, 0xEE,             # MOV AL,E0; OUT DX,AL
        0xBA, 0xF7, 0x01,             # MOV DX,1F7
        0xB0, 0xEC, 0xEE,             # MOV AL,EC; OUT DX,AL (IDENTIFY)
        0xEC,                         # IN AL,DX (ATA DRQ ready)
        0xA2, 0x04, 0x03,             # MOV [0304],AL
        0x0F, 0x01, 0x16, 0x00, 0x02, # LGDT [0200]
        0x0F, 0x20, 0xC0,             # MOV EAX,CR0
        0x0D, 0x01, 0x00,             # OR AX,1 (PE)
        0x0F, 0x22, 0xC0,             # MOV CR0,EAX
        0xB8, 0x60, 0x00,             # MOV AX,0060 (TSS)
        0xEA, 0x54, 0x08, 0x50, 0x00, # FAR JMP 0050:0854
    ))
    iso[boot : boot + len(stub)] = stub

    # DAP for INT13 AH42, real16 DS points at boot image (0x2000).
    iso[boot + 0x100] = 16
    u16(iso, boot + 0x102, 1)  # One 2048-byte sector
    u16(iso, boot + 0x104, 0)  # Target 0x3000:0000
    u16(iso, boot + 0x106, 0x3000)
    u32(iso, boot + 0x108, DATA)
    u32(iso, boot + 0x10C, 0)

    # GDT at physical 0x20280. Protected16 code base 0x20000.
    u16(iso, boot + 0x200, 0x006F)
    u32(iso, boot + 0x202, 0x20280)
    u64(iso, boot + 0x280 + 0x50, desc(0x20000, 0x1FFF, 0x9A))
    u64(iso, boot + 0x280 + 0x58, desc(0x20000, 0x1FFF, 0x92))
    u64(iso, boot + 0x280 + 0x60, desc(0x25000, 0x67, 0x89))
    u64(iso, boot + 0x280 + 0x68, desc(0x21800, 0x1F, 0x82))
    u64(iso, boot + 0x1808, desc(0x20000, 0x1FFF, 0x92))
    # Same historic checkpoint as an x86 boot probe: 0050:0854/0F 00.
    # LTR AX, STR AX, HLT. We must not call this the Windows loader.
    iso[boot + 0x0854 : boot + 0x0854 + 7] = bytes(
        (0x0F, 0x00, 0xD8, 0x0F, 0x00, 0xC8, 0xF4)
    )
    iso[DATA * SECTOR : DATA * SECTOR + 8] = b"C09CDROM"
    return iso


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--fault-disk", action="store_true",
                        help="Make a synthetic raw x86 MBR that fails on LOCK prefix")
    parser.add_argument("--bad-catalog", action="store_true",
                        help="Make deliberately corrupt El Torito checksum")
    args = parser.parse_args()
    if args.output.exists():
        raise SystemExit("Refuse to overwrite existing ISO fixture")
    if args.fault_disk and args.bad_catalog:
        raise SystemExit("Select only one negative fixture")
    if args.fault_disk:
        raw = bytearray(512)
        raw[0:32] = b"\x90" * 32
        raw[32:33] = b"\xf0"  # unsupported LOCK prefix
        raw[510:512] = b"\x55\xaa"
        args.output.write_bytes(raw)
        print("C09_SYNTHETIC_RAW_FAULT_CREATED windows_image=false", args.output)
    else:
        iso = generate()
        if args.bad_catalog:
            iso[CATALOG * SECTOR + 4] ^= 0x01
        args.output.write_bytes(iso)
        print("C09_SYNTHETIC_EL_TORITO_CREATED windows_image=false",
              "valid_catalog=" + str(not args.bad_catalog), args.output)


if __name__ == "__main__":
    main()
