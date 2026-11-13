#!/usr/bin/env python3
"""Build an El Torito fixture that returns to real mode and installs a test MBR."""

import pathlib
import sys

from c09_synthetic_iso import BOOT, SECTOR, desc, generate, u16, u32, u64


def show(message: bytes) -> bytes:
    return b"".join(bytes((0xB8, char, 0x0E, 0xCD, 0x10)) for char in message)


def image() -> bytearray:
    iso = generate()
    start = BOOT * SECTOR
    iso[start : start + 4 * SECTOR] = bytes(4 * SECTOR)
    enter_protected = bytes((
        0xFA,                             # CLI
        0x0E, 0x1F,                       # PUSH CS; POP DS
        0x0F, 0x01, 0x16, 0x00, 0x02,     # LGDT [0200]
        0x66, 0xB8, 0x11, 0, 0, 0,        # MOV EAX,00000011 (PE=1)
        0x0F, 0x22, 0xC0,                 # MOV CR0,EAX
        0xEA, 0x00, 0x04, 0x20, 0x00,     # JMP FAR 0020:0400 (protected32)
    ))
    iso[start : start + len(enter_protected)] = enter_protected
    u16(iso, start + 0x200, 0x5F)
    u32(iso, start + 0x202, 0x20280)
    u64(iso, start + 0x280 + 0x20, desc(0x20000, 0xFFFF, 0x9A) | (1 << 54))
    u64(iso, start + 0x280 + 0x50, desc(0x20800, 0xFFFF, 0x9A))
    u64(iso, start + 0x280 + 0x58, desc(0x20000, 0xFFFF, 0x92))

    read_tsc = bytes((
        0xB9, 1, 0, 0, 0,                 # MOV ECX,1
        0xF0, 0x09, 0x0D, 0x90, 3, 0, 0, # LOCK OR DWORD [0390],ECX
        0x83, 0x3D, 0x90, 3, 0, 0, 1,    # CMP DWORD [0390],1
        0x74, 1, 0xF4,                    # JE timestamp probe; otherwise HLT
        0x89, 0xD3,                       # MOV EBX,EDX (preserve BIOS drive in DL)
        0xB9, 0x10, 0, 0, 0,              # MOV ECX,00000010 (IA32_TSC)
        0x0F, 0x31,                       # RDTSC
        0x89, 0xC6,                       # MOV ESI,EAX
        0x89, 0xD7,                       # MOV EDI,EDX
        0x0F, 0x32,                       # RDMSR at protected32 CPL0
        0x39, 0xFA, 0x75, 0x14,           # CMP EDX,EDI; JNE failure
        0x29, 0xF0, 0x76, 0x10,           # SUB EAX,ESI; JBE failure
        0x3D, 0x00, 0x10, 0, 0,           # CMP EAX,4096 (same advancing counter)
        0x77, 0x09,                       # JA failure
        0x89, 0xDA,                       # MOV EDX,EBX
        0xEA, 0x92, 0x08, 0, 0, 0x50, 0,  # JMP FAR 0050:0892 (protected16)
        0xF4,                             # failure: HLT with IF clear
    ))
    iso[start + 0x400 : start + 0x400 + len(read_tsc)] = read_tsc

    exit_protected = bytes((
        0xB8, 0x58, 0, 0x8E, 0xD8,       # MOV DS,0058 (cached base 0x20000)
        0x66, 0xB8, 0x10, 0, 0, 0,        # MOV EAX,00000010 (PE=0)
        0x0F, 0x22, 0xC0,                 # MOV CR0,EAX at 0050:089D
        0xEA, 0x40, 0x00, 0x00, 0x20,     # JMP FAR 2000:0040 (real16)
    ))
    protected = start + 0x800 + 0x892  # Protected CS has base 0x20800.
    iso[protected : protected + len(exit_protected)] = exit_protected

    code = bytes((
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
    keyboard_jump = 9 + 5 * len(b"ISO -> QSSD") + 7
    write_jump = len(code) - 1
    code += b"\xb0\x03\xe6\x92\xfa\xf4"  # Preserve A20 and request reset via port 92h.
    failure = len(code)
    code += show(b" | FAILED") + b"\xfa\xf4"
    program = bytearray(code)
    for jump in (8, keyboard_jump, write_jump):
        assert 0 <= failure - jump - 1 < 128
        program[jump] = failure - jump - 1
    assert 0x40 + len(program) <= 0x100  # Keep the boot code clear of the DAPs.
    iso[start + 0x40 : start + 0x40 + len(program)] = program
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
