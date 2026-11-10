#!/usr/bin/env python3
# Navi5 — regression for native converter's absent weak ELF definitions.
# SPDX-License-Identifier: GPL-3.0-or-later
# Run after native build: python3 app/tests/host/weak_imports.py SDK_STUB_DIR
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[3]
build = root / "app/build"
source = bytearray((build / "llvm-pie.elf").read_bytes())


def sections(data):
    offset = struct.unpack_from("<Q", data, 40)[0]
    size, count, names = struct.unpack_from("<HHH", data, 58)
    entries = [struct.unpack_from("<IIQQQQIIQQ", data, offset + i * size) for i in range(count)]
    strings = entries[names]
    table = data[strings[4]:strings[4] + strings[5]]
    return {table[s[0]:].split(b"\0")[0].decode(): s for s in entries}


s = sections(source)
dyn = s[".dynsym"]
strings = s[".dynstr"]
names = source[strings[4]:strings[4] + strings[5]]
symbols = {}
for at in range(dyn[4], dyn[4] + dyn[5], 24):
    name, info, other, section, value, size = struct.unpack_from("<IBBHQQ", source, at)
    symbols[names[name:].split(b"\0")[0].decode()] = (at, info, section)
weak = [symbols[n] for n in ("ZSTD_trace_decompress_begin", "ZSTD_trace_decompress_end")]
assert all(info >> 4 == 2 and section == 0 for _, info, section in weak)
indices = {(at - dyn[4]) // 24 for at, _, _ in weak}
rela = s[".rela.dyn"]
patches = []
for at in range(rela[4], rela[4] + rela[5], 24):
    address, info, addend = struct.unpack_from("<QQq", source, at)
    if info >> 32 in indices:
        assert info & 0xffffffff == 6 and addend == 0
        patches.append((at, address))
assert len(patches) == 2


def mapped(data, address):
    offset = struct.unpack_from("<Q", data, 32)[0]
    size, count = struct.unpack_from("<HH", data, 54)
    for i in range(count):
        kind, flags, file, virtual, physical, length, memory, align = struct.unpack_from(
            "<IIQQQQQQ", data, offset + i * size)
        if kind == 1 and virtual <= address and address + 8 <= virtual + length:
            return file + address - virtual
    raise AssertionError("address not file-backed")


with tempfile.TemporaryDirectory(prefix="navi5-weak-") as directory:
    directory = Path(directory)
    def convert(data, success=True):
        input = directory / "input.elf"
        output = directory / "output.elf"
        input.write_bytes(data)
        command = [str(build / "host/ps5-native-tool"), "link", "--in", str(input),
                   "--out", str(output), "--stub-dir", sys.argv[1]]
        for stub in sorted((build / "stubs").glob("*.so")):
            command += ["--stub", str(stub)]
        result = subprocess.run(command, capture_output=True, text=True)
        assert (result.returncode == 0) == success, result.stderr
        return output.read_bytes() if success else result.stderr

    output = convert(source)
    for at, address in patches:
        assert struct.unpack_from("<Q", output, mapped(output, address))[0] == 0
        assert source[at:at + 24] not in output, "absent weak relocation retained"
    altered = bytearray(source)
    at, address = patches[0]
    struct.pack_into("<Qq", altered, at + 8,
                     (next(iter(indices)) << 32) | 1, 37)
    output = convert(altered)
    assert struct.unpack_from("<Q", output, mapped(output, address))[0] == 37, "S+A lost"
    altered = bytearray(source)
    altered[weak[0][0] + 4] = 0x10
    assert "no public SDK stub exports required symbol" in convert(altered, False)
    altered = bytearray(source)
    struct.pack_into("<I", altered, patches[0][0] + 8, 2)
    assert "unsupported unresolved weak relocation" in convert(altered, False)
    print("PASS: absent weak GLOB_DAT=0, R_X86_64_64=S+A; strong/unsupported refused")
