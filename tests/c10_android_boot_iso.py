#!/usr/bin/env python3
"""Build the El Torito guest that discovers, writes and reboots from QSSD."""

import pathlib
import struct
import sys

from c09_synthetic_iso import BOOT, SECTOR, desc, generate, u16, u32, u64


def show(message: bytes) -> bytes:
    return b"".join(bytes((0xB8, char, 0x0E, 0xCD, 0x10)) for char in message)


def show_line(offset: int, length: int, row: int, attribute: int) -> bytes:
    return (
        b"\xbd" + struct.pack("<H", offset)  # MOV BP, offset in the guest ES window.
        + b"\xb8\x01\x13"                         # AX=1301, string + update cursor.
        + bytes((0xBB, attribute, 0, 0xB9, length, 0, 0xBA, 0, row, 0xCD, 0x10))
    )


def out8(port: int, value: int, *, protected32: bool = True) -> bytes:
    prefix = b"\x66" if protected32 else b""
    return prefix + b"\xba" + struct.pack("<H", port) + bytes((0xB0, value, 0xEE))


def pci_expected(register: int, value: int) -> bytes:
    return (
        b"\xb8" + struct.pack("<I", 0x80001000 | register)
        + b"\x66\xba\xf8\x0c\xef"       # OUT DX,EAX at CF8
        + b"\x66\xba\xfc\x0c\xed"       # IN EAX,DX at CFC
        + b"\x3d" + struct.pack("<I", value) + expect_equal()
    )


def expect_equal() -> bytes:
    return expect_condition(0x74)


def expect_condition(jcc: int) -> bytes:
    return bytes((jcc, 2, 0xFA, 0xF4))  # Branch on success; otherwise CLI; HLT.


def ata_identify() -> bytes:
    # The contiguous DS/ES window starts at the ISO's load address, including
    # the temporary IDT, its IRQ14 counter and all 256 words of ATA IDENTIFY.
    code = b"\x66\xb8\x58\x00\x8e\xd8\x8e\xc0"  # MOV DS/ES,0058
    code += pci_expected(0x00, 0xC0711B36) + pci_expected(0x08, 0x01010001)
    code += b"\x0f\x01\x1d\x06\x02\x00\x00"  # LIDT [0206]
    for port, value in (
        (0x20, 0x11), (0x21, 0x20), (0x21, 4), (0x21, 1), (0x21, 0xFB),
        (0xA0, 0x11), (0xA1, 0x28), (0xA1, 2), (0xA1, 1), (0xA1, 0xBF),
        (0x1F6, 0xE0), (0x1F7, 0xEC),
    ):
        code += out8(port, value)
    code += b"\xfb\xf4\xfa"  # STI; HLT; CLI, woken by the queued drive IRQ.
    code += b"\x83\x3d\x98\x03\x00\x00\x01" + expect_equal()  # IRQ14 ran once.
    code += b"\x66\xba\xf7\x01\xec\x3c\x58" + expect_equal()  # Ready + DRQ.
    code += b"\xbf\x00\x0a\x00\x00\xb9\x00\x01\x00\x00"  # EDI=0A00, ECX=256
    code += b"\x66\xba\xf0\x01\xfc\xf3\x66\x6d"  # CLD; REP INSW from 1F0.
    code += b"\x66\x83\x3d\x00\x0a\x00\x00\x40" + expect_equal()  # ATA fixed disk.
    code += b"\x66\xa1\xa6\x0a\x00\x00\x66\x25\x00\xc4\x66\x3d\x00\x44" + expect_equal()
    code += b"\xa1\xc8\x0a\x00\x00\x3b\x05\x30\x01\x00\x00" + expect_equal()
    code += b"\xa1\xcc\x0a\x00\x00\x3b\x05\x34\x01\x00\x00" + expect_equal()
    code += b"\x81\x3d\x36\x0a\x00\x00\x45\x52\x45\x56" + expect_equal()  # ATA word-swapped REVE.
    code += b"\x66\xba\xf7\x01\xec\x3c\x50" + expect_equal()  # DRQ cleared.
    code += b"\x89\xda"  # Restore boot CD drive in DL from EBX before returning real16.
    code += b"\xea\x92\x08\x00\x00\x50\x00"  # JMP FAR 0050:0892 (protected16)
    return code


def real_ata_readback() -> bytes:
    # Read the BIOS-written sector through PCI ATA, into the next 512 bytes
    # of the same RAM window that still holds the original CD payload.
    code = b"".join(
        out8(port, value, protected32=False)
        for port, value in (
            (0x1F6, 0xE0), (0x1F2, 1), (0x1F3, 0), (0x1F4, 0), (0x1F5, 0), (0x1F7, 0x20),
        )
    )
    code += b"\xec\x3c\x58" + expect_equal()  # Read status at 1F7, ready and DRQ.
    code += b"\xb8\x00\x30\x8e\xd8\x8e\xc0"  # DS/ES=3000 (CD-loaded MBR buffer).
    code += b"\xbf\x00\x02\xb9\x00\x01\xba\xf0\x01\xfc\xf3\x6d"  # REP INSW to ES:0200.
    code += b"\x31\xf6\xbf\x00\x02\xb9\x00\x01\xf3\xa7" + expect_equal()  # REPE CMPSW.
    code += b"\xb0\x03\xe6\x92\xfa\xf4"  # Preserve A20 and request reset via port 92h.
    return code


def image() -> bytearray:
    iso = generate()
    start = BOOT * SECTOR
    iso[start : start + 4 * SECTOR] = bytes(4 * SECTOR)
    entry = bytes((
        0xFA,                             # CLI
        0x80, 0xFA, 0xE0,                 # CMP DL,E0h: BIOS booted the CD.
    )) + expect_equal() + bytes((
        0x80, 0x3E, 0x75, 0x04, 0x01,     # CMP BYTE [0475],1: BIOS listed one hard disk.
    )) + expect_equal() + bytes((
        0x0E, 0x1F,                       # PUSH CS; POP DS
        0xE9, 0x00, 0x00,                 # JMP to the real-mode disk probe below.
    ))
    iso[start : start + len(entry)] = entry
    u16(iso, start + len(entry) - 2, 0xC00 - len(entry))
    bios_probe = show(b"EL TORITO BIOS") + bytes((
        0x52,                             # PUSH DX: preserve the CD drive.
        0xB2, 0x80, 0xBB, 0xAA, 0x55,     # DL=80h, BX=55AAh: query EDD support.
        0xB4, 0x41, 0xCD, 0x13,
        0x73, 0x02, 0xFA, 0xF4,           # JNC success; otherwise CLI; HLT.
        0x81, 0xFB, 0x55, 0xAA,           # CMP BX,AA55h.
    )) + expect_equal() + bytes((
        0xBE, 0x20, 0x01,                 # DS:SI to EDD data buffer.
        0xB4, 0x48, 0xCD, 0x13,
        0x73, 0x02, 0xFA, 0xF4,
        0x66, 0x81, 0x3E, 0x30, 0x01, 0, 0, 0, 0x08,  # sectors low dword = 134217728 (64 GiB)
    )) + expect_equal() + bytes((
        0x66, 0x83, 0x3E, 0x34, 0x01, 0,  # sectors high dword = 0.
    )) + expect_equal() + bytes((
        0x81, 0x3E, 0x38, 0x01, 0, 0x02,  # logical sector bytes = 512.
    )) + expect_equal() + bytes((
        0x5A,                             # POP DX: original El Torito drive.
        0x0F, 0x01, 0x16, 0x00, 0x02,     # LGDT [0200]
        0x66, 0xB8, 0x11, 0, 0, 0,        # MOV EAX,00000011 (PE=1)
        0x0F, 0x22, 0xC0,                 # MOV CR0,EAX
        0xEA, 0x00, 0x04, 0x20, 0x00,     # JMP FAR 0020:0400 (protected32)
    ))
    iso[start + 0xC00 : start + 0xC00 + len(bios_probe)] = bios_probe
    u16(iso, start + 0x120, 26)             # EDD drive parameter packet size.
    u16(iso, start + 0x200, 0x5F)
    u32(iso, start + 0x202, 0x20280)
    u16(iso, start + 0x206, 0x0177)         # Protected IDT through vector 2E.
    u32(iso, start + 0x208, 0x20800)
    u16(iso, start + 0x20C, 0x03FF)         # Return to the reset real-mode IVT.
    u32(iso, start + 0x20E, 0)
    u64(iso, start + 0x280 + 0x20, desc(0x20000, 0xFFFF, 0x9A) | (1 << 54))
    u64(iso, start + 0x280 + 0x50, desc(0x20800, 0xFFFF, 0x9A))
    u64(iso, start + 0x280 + 0x58, desc(0x20000, 0xFFFF, 0x92))

    read_tsc = bytes((
        0xB9, 1, 0, 0, 0,                 # MOV ECX,1
        0xF0, 0x09, 0x0D, 0x90, 3, 0, 0, # LOCK OR DWORD [0390],ECX
        0x83, 0x3D, 0x90, 3, 0, 0, 1,    # CMP DWORD [0390],1
    )) + expect_equal() + bytes((
        0x89, 0xD3,                       # MOV EBX,EDX (preserve BIOS drive in DL)
        0xB9, 0x10, 0, 0, 0,              # MOV ECX,00000010 (IA32_TSC)
        0x0F, 0x31,                       # RDTSC
        0x89, 0xC6,                       # MOV ESI,EAX
        0x89, 0xD7,                       # MOV EDI,EDX
        0x0F, 0x32,                       # RDMSR at protected32 CPL0
        0x39, 0xFA,                       # CMP EDX,EDI: same upper half.
    )) + expect_equal() + b"\x29\xf0" + expect_condition(0x77)  # SUB EAX,ESI; JA.
    read_tsc += b"\x3d\x00\x10\x00\x00" + expect_condition(0x76)  # CMP EAX,4096; JBE.
    read_tsc += b"\xe9" + struct.pack("<i", 0x500 - 0x400 - len(read_tsc) - 5)
    iso[start + 0x400 : start + 0x400 + len(read_tsc)] = read_tsc
    identify = ata_identify()
    assert len(identify) < 0x200
    iso[start + 0x500 : start + 0x500 + len(identify)] = identify
    irq14 = b"\x52\x50\xff\x05\x98\x03\x00\x00"  # PUSH DX/AX; INC DWORD [0398].
    irq14 += out8(0xA0, 0x20) + out8(0x20, 0x20) + b"\x58\x5a\xcf"  # EOIs; IRETD.
    iso[start + 0x700 : start + 0x700 + len(irq14)] = irq14
    u64(iso, start + 0x800 + 0x2E * 8, (0x8E << 40) | (0x20 << 16) | 0x700)

    exit_protected = bytes((
        0xB8, 0x58, 0, 0x8E, 0xD8,       # MOV DS,0058 (cached base 0x20000)
        0x66, 0xB8, 0x10, 0, 0, 0,        # MOV EAX,00000010 (PE=0)
        0x0F, 0x22, 0xC0,                 # MOV CR0,EAX at 0050:089D
        0xEA, 0x40, 0x00, 0x00, 0x20,     # JMP FAR 2000:0040 (real16)
    ))
    protected = start + 0x800 + 0x892  # Protected CS has base 0x20800.
    iso[protected : protected + len(exit_protected)] = exit_protected

    code = bytes((
        0x0F, 0x01, 0x1E, 0x0C, 0x02, # LIDT [020C] restores the real-mode IVT.
        0xBE, 0x00, 0x01,             # MOV SI,0100 (optical DAP)
        0xB4, 0x42, 0xCD, 0x13,       # Read MBR from CD sector 29.
        0x72, 0x00,                   # JC failure (patched below)
    ))
    read_jump = len(code) - 1
    detected = b"QSSD 64 GiB DETECTED"
    prompt = b"ENTER: WRITE TEST MBR AND REBOOT"
    iso[start + 0xD00 : start + 0xD00 + len(detected)] = detected
    iso[start + 0xD20 : start + 0xD20 + len(prompt)] = prompt
    code += b"\x0e\x07"  # PUSH CS; POP ES.
    code += show_line(0xD00, len(detected), 1, 0x0A)
    code += show_line(0xD20, len(prompt), 3, 0x07)
    code += bytes((
        0xB4, 0x00, 0xCD, 0x16,       # BIOS waits for a user keystroke.
        0x3C, 0x0D, 0x75, 0x00,       # Only Enter confirms the write.
    ))
    keyboard_jump = len(code) - 1
    code += bytes((
        0xB2, 0x80,                   # BIOS fixed disk DL=80h
        0xBE, 0x10, 0x01,             # MOV SI,0110 (fixed disk DAP)
        0xB4, 0x43, 0xCD, 0x13,       # Persist MBR to QSSD sector zero.
        0x72, 0x00,                   # JC failure (patched below)
    ))
    write_jump = len(code) - 1
    code += b"\xe9" + struct.pack("<h", 0xE00 - 0x40 - len(code) - 3)
    failure = len(code)
    code += show(b" | FAILED") + b"\xfa\xf4"
    program = bytearray(code)
    for jump in (read_jump, keyboard_jump, write_jump):
        assert 0 <= failure - jump - 1 < 128
        program[jump] = failure - jump - 1
    assert 0x40 + len(program) <= 0x100  # Keep the boot code clear of the DAPs.
    iso[start + 0x40 : start + 0x40 + len(program)] = program
    readback = real_ata_readback()
    iso[start + 0xE00 : start + 0xE00 + len(readback)] = readback
    for pointer, lba in ((0x100, 29), (0x110, 0)):
        dap = start + pointer
        iso[dap] = 0x10
        u16(iso, dap + 2, 1)
        u16(iso, dap + 6, 0x3000)
        u32(iso, dap + 8, lba)
    mbr = b"\x80\xfa\x80" + expect_equal()  # BIOS booted the same QSSD as DL=80h.
    mbr += show(b"QSSD REBOOT OK") + b"\xfa\xf4"
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
