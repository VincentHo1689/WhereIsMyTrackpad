#!/usr/bin/env python3
"""inject-dylib.py - add (or list) an LC_LOAD_DYLIB load command in a Mach-O file.

Unlike DYLD_INSERT_LIBRARIES, a load command makes the library load on every
launch, with no environment variable and no wrapper. It is written into the
padding that already exists between the end of the load commands and the first
section, so no existing file offsets move.

The code signature is invalidated by design; re-sign the binary afterwards.

Usage:
  inject-dylib.py add    <macho> <dylib-path>
  inject-dylib.py remove <macho> <dylib-path>
  inject-dylib.py list   <macho>
  inject-dylib.py check  <macho> <dylib-path>
"""
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
FAT_MAGIC_64 = 0xCAFEBABF
LC_LOAD_DYLIB = 0xC
LC_SEGMENT_64 = 0x19


def align8(n):
    return (n + 7) & ~7


def load_command_size(name):
    return align8(24 + len(name.encode()) + 1)


def arch_slices(data):
    """Yield (arch_name, base, size) for thin and fat Mach-O files."""
    if len(data) < 8:
        raise ValueError("file too small to be Mach-O")
    be_magic = struct.unpack_from(">I", data, 0)[0]
    if be_magic in (FAT_MAGIC, FAT_MAGIC_64):
        n = struct.unpack_from(">I", data, 4)[0]
        base, off = 8, 8
        for _ in range(n):
            cputype, _sub, s_off, s_size, _align = struct.unpack_from(">iiIII", data, off)
            name = {0x0100000C: "arm64", 0x01000007: "x86_64"}.get(cputype, hex(cputype))
            yield name, s_off, s_size
            off += 20
        return
    yield "thin", 0, len(data)


def commands(img):
    """Yield (cmd, cmdsize, offset_in_slice) for every load command."""
    ncmds, sizeofcmds = struct.unpack_from("<II", img, 16)
    off = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", img, off)
        yield cmd, cmdsize, off
        off += cmdsize


def first_section_offset(img):
    first = None
    for cmd, _size, off in commands(img):
        if cmd == LC_SEGMENT_64:
            nsects = struct.unpack_from("<I", img, off + 64)[0]
            so = off + 72
            for _ in range(nsects):
                sec_off = struct.unpack_from("<I", img, so + 48)[0]
                if sec_off and (first is None or sec_off < first):
                    first = sec_off
                so += 80
    return first


def existing_dylibs(img):
    names = []
    for cmd, _size, off in commands(img):
        if cmd == LC_LOAD_DYLIB:
            name_off = struct.unpack_from("<I", img, off + 8)[0]
            end = img.index(b"\x00", off + name_off)
            names.append(img[off + name_off:end].decode(errors="replace"))
    return names


def make_command(name):
    raw = name.encode() + b"\x00"
    size = align8(24 + len(raw))
    cmd = struct.pack("<IIIIII", LC_LOAD_DYLIB, size, 24, 0, 0, 0) + raw
    return cmd + b"\x00" * (size - len(cmd))


def add(data, name):
    out = bytearray(data)
    for arch, base, size in arch_slices(data):
        img = bytes(out[base:base + size])
        if struct.unpack_from("<I", img, 0)[0] != MH_MAGIC_64:
            print(f"  [{arch}] skipped (not a 64-bit Mach-O)")
            continue
        if name in existing_dylibs(img):
            print(f"  [{arch}] already present")
            continue
        ncmds, sizeofcmds = struct.unpack_from("<II", img, 16)
        end = 32 + sizeofcmds
        limit = first_section_offset(img)
        command = make_command(name)
        if limit is not None and end + len(command) > limit:
            raise SystemExit(
                f"  [{arch}] no room: need {len(command)} bytes, have "
                f"{(limit - end) if limit else 0}")
        out[base + end:base + end + len(command)] = command
        struct.pack_into("<II", out, base + 16, ncmds + 1, sizeofcmds + len(command))
        print(f"  [{arch}] added ({len(command)} bytes)")
    return bytes(out)


def remove(data, name):
    out = bytearray(data)
    for arch, base, size in arch_slices(data):
        img = bytes(out[base:base + size])
        if struct.unpack_from("<I", img, 0)[0] != MH_MAGIC_64:
            continue
        ncmds, sizeofcmds = struct.unpack_from("<II", img, 16)
        start, end = 32, 32 + sizeofcmds
        kept, dropped, removed = bytearray(), 0, 0
        for cmd, cmdsize, off in commands(img):
            block = img[off:off + cmdsize]
            if cmd == LC_LOAD_DYLIB and block[24:block.index(b"\x00", 24)] == name.encode():
                removed += cmdsize
                dropped += 1
            else:
                kept += block
        if not dropped:
            continue
        out[base + start:base + start + len(kept)] = kept
        out[base + start + len(kept):base + end] = b"\x00" * (sizeofcmds - len(kept))
        struct.pack_into("<II", out, base + 16, ncmds - dropped, sizeofcmds - removed)
        print(f"  [{arch}] removed {dropped} command(s) ({removed} bytes)")
    return bytes(out)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    action, path = sys.argv[1], sys.argv[2]
    data = open(path, "rb").read()
    if action == "list":
        for arch, base, size in arch_slices(data):
            print(f"[{arch}]")
            for n in existing_dylibs(data[base:base + size]):
                print("  " + n)
    elif action in ("check", "add", "remove"):
        if len(sys.argv) < 4:
            raise SystemExit(__doc__)
        name = sys.argv[3]
        found = any(name in existing_dylibs(data[base:base + size])
                    for arch, base, size in arch_slices(data))
        if action == "check":
            print("present" if found else "absent")
            raise SystemExit(0 if found else 1)
        if action == "remove":
            if not found:
                print("not injected; nothing to do")
                raise SystemExit(0)
            open(path, "wb").write(remove(data, name))
            print(f"removed: {name}")
            raise SystemExit(0)
        if found:
            print("already injected; nothing to do")
            raise SystemExit(0)
        open(path, "wb").write(add(data, name))
        print(f"injected: {name}")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
