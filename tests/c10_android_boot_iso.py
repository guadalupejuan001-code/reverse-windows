#!/usr/bin/env python3
"""Build an El Torito fixture that installs a test MBR through BIOS."""

import pathlib
import sys

from c09_synthetic_iso import BOOT, SECTOR, generate, u16, u32


def show(message: bytes) -> bytes:
    return b"".join(bytes((0xB8, char, 0x0E, 0xCD, 0x10)) for char in message)


def image() -> bytearray:
    iso = generate()
    start = BOOT * SECTOR
    iso[start : start + 4 * SECTOR] = bytes(4 * SECTOR)
    code = bytes((
        0x0E, 0x1F,                   # PUSH CS; POP DS
        0xBE, 0x00, 0x01,             # MOV SI,0100 (optical DAP)
        0xB4, 0x42, 0xCD, 0x13,       # Read MBR from CD sector 29.
        0x72, 0x00,                   # JC failure (patched below)
    ))
    code += show(b"ISO -> QSSD")
    code += bytes((
        0xB4, 0x00, 0xCD, 0x16,       # BIOS waits for a user keystroke.
        0x3C, 0x0D, 0x75, 0x00,       # Only Enter confirms the write.
        0xB2, 0x80,                   # BIOS fixed disk DL=80h
        0xBE, 0x10, 0x01,             # MOV SI,0110 (fixed disk DAP)
        0xB4, 0x43, 0xCD, 0x13,       # Persist MBR to QSSD sector zero.
        0x72, 0x00,                   # JC failure (patched below)
    ))
    keyboard_jump = 11 + 5 * len(b"ISO -> QSSD") + 7
    write_jump = len(code) - 1
    code += show(b" MBR SAVED") + b"\xfa\xf4"
    failure = len(code)
    code += show(b" | FAILED") + b"\xfa\xf4"
    program = bytearray(code)
    for jump in (10, keyboard_jump, write_jump):
        assert 0 <= failure - jump - 1 < 128
        program[jump] = failure - jump - 1
    iso[start : start + len(program)] = program
    for pointer, lba in ((0x100, 29), (0x110, 0)):
        dap = start + pointer
        iso[dap] = 0x10
        u16(iso, dap + 2, 1)
        u16(iso, dap + 6, 0x3000)
        u32(iso, dap + 8, lba)
    mbr = show(b"QSSD REBOOT OK") + b"\xfa\xf4"
    offset = 29 * SECTOR
    iso[offset : offset + len(mbr)] = mbr
    iso[offset + 510 : offset + 512] = b"\x55\xaa"
    return iso


if __name__ == "__main__":
    path = pathlib.Path(sys.argv[1])
    fixture = image()
    try:
        with path.open("xb") as output:
            output.write(fixture)
    except FileExistsError:
        if path.read_bytes() != fixture:
            raise
