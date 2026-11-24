"""Resolve observed Windows kernel PCs against its exact public Microsoft PDB."""

import bisect
import hashlib
import json
from pathlib import Path
import struct
import sys
from uuid import UUID

PDB_GUID = "68A17FAF3012B7846079AEECDBE0A583"
PE_AGE = 1
MICROSOFT_VALIDATOR = "https://github.com/microsoft/microsoft-pdb/blob/805655a28bd8198004be2ac27e6e0290121a5e89/PDB/dbi/pdb.cpp#L826-L881"
TEXT_SECTION = 8
TEXT_RVA = 0x200000
OBSERVED_RANGES = (
    (0x39B401, 0x39B3A4, 0x39B45B),
    (0x3BB0F1, 0x3BAE80, 0x3BB35E),
    (0x21BFD8, 0x21BFD0, 0x21C00F),
    (0x40C212, 0x40C180, 0x40C269),
    (0x3F2272, 0x3F2168, 0x3F22C0),
    (0x2D4689, 0x2D33F1, 0x2D4D8D),
    (0x231B31, 0x231A24, 0x231BE8),
    (0x290327, 0x29030C, 0x290353),
    (0x2D112E, 0x2D10F0, 0x2D1223),
    (0x31CB04, 0x31CB04, 0x31CB09),
    (0x2D13C7, 0x2D13BF, 0x2D13E1),
    (0x208A40, 0x2089EE, 0x208ABD),
    (0x2DB12B, 0x2DB0E0, 0x2DB2F2),
    (0x2D31AB, 0x2D3165, 0x2D328D),
    (0x2D52EC, 0x2D51D0, 0x2D55A3),
    (0x233EC3, 0x233990, 0x234584),
    (0x3CE272, 0x3CE260, 0x3CE27E),
    (0x393F49, 0x393A90, 0x3947A0),
    (0x3FCF10, 0x3FCF00, 0x3FCF43),
    (0x3F4D9F, 0x3F4D90, 0x3F4DA0),
    (0x401BDE, 0x401B70, 0x401C02),
)


def read_msf_streams(data):
    magic = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\x00\x00\x00"
    if not data.startswith(magic):
        raise ValueError("Unexpected Microsoft PDB container")
    block_size, _, block_count, directory_size, _, block_map = struct.unpack_from("<6I", data, 32)
    if block_size * block_count != len(data):
        raise ValueError("The PDB block table does not cover the input correctly")

    def take_blocks(start, size, source):
        count = (size + block_size - 1) // block_size
        indexes = struct.unpack_from(f"<{count}I", source, start)
        if any(index >= block_count for index in indexes):
            raise ValueError("PDB references an unavailable block")
        pieces = [data[index * block_size : (index + 1) * block_size] for index in indexes]
        return b"".join(pieces)[:size], start + 4 * count

    directory, _ = take_blocks(block_map * block_size, directory_size, data)
    count = struct.unpack_from("<I", directory)[0]
    lengths = struct.unpack_from(f"<{count}I", directory, 4)
    cursor = 4 + 4 * count
    streams = []
    for length in lengths:
        if length == 0xFFFFFFFF:
            streams.append(None)
        else:
            stream, cursor = take_blocks(cursor, length, directory)
            streams.append(stream)
    return streams


def public_kernel_symbols(data):
    streams = read_msf_streams(data)
    info_age = struct.unpack_from("<I", streams[1], 8)[0]
    dbi_age = struct.unpack_from("<I", streams[3], 8)[0]
    guid = UUID(bytes_le=streams[1][12:28]).hex.upper()
    # Microsoft OpenValidate4 permits a later PDB Info age with the exact DBI age.
    if guid != PDB_GUID or info_age < PE_AGE or dbi_age != PE_AGE:
        raise ValueError(f"PDB GUID {guid}, Info age {info_age}, and DBI age {dbi_age} do not match the kernel")
    identity = {"guid": guid, "pe_codeview_age": PE_AGE, "pdb_info_age": info_age, "pdb_dbi_age": dbi_age}
    symbol_stream_index = struct.unpack_from("<H", streams[3], 20)[0]
    records = streams[symbol_stream_index]
    symbols = {}
    cursor = 0
    while cursor + 4 <= len(records):
        length, kind = struct.unpack_from("<HH", records, cursor)
        advance = cursor + length + 2
        if length < 2 or advance > len(records):
            raise ValueError("Invalid PDB symbol extent")
        if kind == 0x110E:
            _, offset, segment = struct.unpack_from("<IIH", records, cursor + 4)
            if segment == TEXT_SECTION:
                name = records[cursor + 14 : advance].split(b"\0", 1)[0].decode()
                symbols.setdefault(TEXT_RVA + offset, []).append(name)
        cursor = advance
    return symbols, identity


def main():
    data = Path(sys.argv[1]).read_bytes()
    symbols, identity = public_kernel_symbols(data)
    if len(symbols) < 1000:
        raise ValueError("Failed to recover the Windows kernel public text symbol table")
    addresses = sorted(symbols)
    result = []
    for pc, begin, end in OBSERVED_RANGES:
        closest = addresses[bisect.bisect_right(addresses, pc) - 1]
        result.append({
            "observed_rva": f"0x{pc:x}",
            "pdata_range": [f"0x{begin:x}", f"0x{end:x}"],
            "exact_function_entry": symbols.get(begin, []),
            "nearest_symbol_rva": f"0x{closest:x}",
            "nearest_public_symbol": symbols[closest],
        })
    print(json.dumps({"pdb_identity": identity, "validator": MICROSOFT_VALIDATOR,
                      "pdb_sha256": hashlib.sha256(data).hexdigest(),
                      "text_symbol_count": len(symbols), "observations": result}, indent=2))


if __name__ == "__main__":
    main()
