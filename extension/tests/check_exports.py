# SPDX-License-Identifier: Apache-2.0
"""Lists what a shared library exports and fails if that is anything but its entry point.

Usage: python -I check_exports.py <library> <entry point>

The extension carries ENet inside it. Godot has its own copy of ENet, so none of ours may be visible from
outside: a name that starts with enet_ is called out, and so is anything else besides the entry point.
On Linux the names come from `nm -D`; on Windows the export table of the DLL is read directly.
"""
import os
import struct
import subprocess
import sys


def linux_exports(path):
    result = subprocess.run(["nm", "-D", "--defined-only", path], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError("nm failed: " + result.stderr.strip())
    names = []
    for line in result.stdout.splitlines():
        parts = line.split()
        if parts:
            names.append(parts[-1])
    return names


def windows_exports(path):
    with open(path, "rb") as file:
        data = file.read()
    if data[:2] != b"MZ":
        raise RuntimeError("not a Windows executable file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe : pe + 4] != b"PE\0\0":
        raise RuntimeError("no PE signature")
    number_of_sections = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise RuntimeError("only 64-bit libraries (PE32+) are supported")
    export_rva, export_size = struct.unpack_from("<II", data, optional + 112)
    if export_rva == 0:
        return []
    sections = []
    table = optional + optional_size
    for index in range(number_of_sections):
        _, virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
            "<8sIIII", data, table + index * 40
        )
        sections.append((virtual_address, max(virtual_size, raw_size), raw_pointer))

    def to_offset(rva):
        for address, size, pointer in sections:
            if address <= rva < address + size:
                return rva - address + pointer
        raise RuntimeError(f"address {rva:#x} is in no section")

    base = to_offset(export_rva)
    number_of_names = struct.unpack_from("<I", data, base + 24)[0]
    names_rva = struct.unpack_from("<I", data, base + 32)[0]
    names = []
    for index in range(number_of_names):
        name_rva = struct.unpack_from("<I", data, to_offset(names_rva) + index * 4)[0]
        start = to_offset(name_rva)
        names.append(data[start : data.index(b"\0", start)].decode("ascii"))
    return names


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    path, entry = sys.argv[1], sys.argv[2]
    if not os.path.isfile(path):
        print(f"FAIL: no such library: {path}")
        return 1
    try:
        names = windows_exports(path) if os.name == "nt" else linux_exports(path)
    except RuntimeError as error:
        print(f"FAIL: {error}")
        return 1
    if entry not in names:
        print(f"FAIL: the entry point {entry} is not exported")
        return 1
    extra = sorted(name for name in names if name != entry)
    if extra:
        enet = [name for name in extra if name.lstrip("_").startswith("enet_")]
        print(f"FAIL: {len(extra)} names are exported besides {entry}" + (f", {len(enet)} of them from ENet" if enet else ""))
        for name in extra[:40]:
            print("  " + name)
        return 1
    print(f"ok: only {entry} is exported")
    return 0


if __name__ == "__main__":
    sys.exit(main())
