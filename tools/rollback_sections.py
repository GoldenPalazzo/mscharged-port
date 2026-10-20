#!/usr/bin/env python3
"""Compiler launcher of the Windows game modules: the rollback's sections.

On ELF, cmake/rollback_sections.ld gathers the writable data of the original
game's code in sections of their own, which a rollback saves with the arenas.
PE linkers take no such script, but they merge the sections named "<name>$<x>"
into one section <name>, in the order of <x>. So this launcher runs the
compile command it is given, then, when the source is one of the original
game's files the script gathers, renames the object's writable data and zeroed
data sections (.data, .bss and their "$" variants: one by variable, the static
locals' guard variables among them) to .rbkd$m and .rbkb$m.
src/platform/rollback_platform.cpp puts the bounds, __rbk_data_start and the
others, in the "$a" and "$z" sections around them.

The files are the script's: keep both lists the same.

Usage: rollback_sections.py <compile command...>
"""

import fnmatch
import os
import struct
import subprocess
import sys

GAME_EXCLUDED = [
    "*src/Game/Audio/*", "*src/Game/Network[!IR]*", "*src/Game/FriendManager*", "*src/Game/LANLobby*",
    "*src/Game/Net.cpp*", "*src/Game/NetTournManager*", "*src/Game/NetMessage*",
    "*src/Game/Pad/FlickDetection*", "*src/Game/FE/feInput*", "*src/Game/RumbleActions*",
]
NL_EXCLUDED = [
    "*src/NL/gl/*", "*src/NL/glx/*", "*src/NL/gc/*", "*src/NL/nlFile*", "*src/NL/nlAsyncFileBuffer*",
    "*src/NL/nlBundleFile*", "*src/NL/nlDebug*", "*src/NL/nlTicker*", "*src/NL/nlTime*", "*src/NL/nlPrint*",
    "*src/NL/nlBufferedWriter*", "*src/NL/plat/nlFileCache*", "*src/NL/plat/nlFlash*",
    "*src/NL/plat/PadBackend*", "*src/NL/plat/WiiFreestylePad*", "*src/NL/plat/*Socket*",
    "*src/NL/plat/Transport*",
]


def matches(path, patterns):
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def target_names(source):
    """The new names of the data and zeroed data sections of an object, or None."""
    path = os.path.abspath(source).replace(os.sep, "/")
    if (fnmatch.fnmatchcase(path, "*src/Game/*") and not matches(path, GAME_EXCLUDED)) \
            or (fnmatch.fnmatchcase(path, "*src/NL/*") and not matches(path, NL_EXCLUDED)) \
            or fnmatch.fnmatchcase(path, "*src/ode/*"):
        return b".rbkd$m", b".rbkb$m"
    return None


def rename_sections(path, data_name, bss_name):
    with open(path, "r+b") as file:
        image = bytearray(file.read())
        # A big object (more than 65279 sections) has a header of its own.
        if image[0:4] == b"\x00\x00\xff\xff" and struct.unpack_from("<H", image, 4)[0] >= 2:
            sections, symbols_at, symbols = struct.unpack_from("<III", image, 44)
            table, symbol_size = 56, 20
        else:
            sections, = struct.unpack_from("<H", image, 2)
            symbols_at, symbols, optional = struct.unpack_from("<IIH", image, 8)
            table, symbol_size = 20 + optional, 18
        strings = symbols_at + symbols * symbol_size
        changed = False
        for index in range(sections):
            at = table + index * 40
            raw = bytes(image[at:at + 8])
            if raw.startswith(b"/"):
                offset = int(raw[1:].rstrip(b"\0").decode())
                end = image.index(b"\0", strings + offset)
                name = bytes(image[strings + offset:end])
            else:
                name = raw.rstrip(b"\0")
            if name in (b".data", b".bss") or name.startswith((b".data$", b".bss$")):
                new = data_name if name.startswith(b".data") else bss_name
                image[at:at + 8] = new.ljust(8, b"\0")
                changed = True
        if changed:
            file.seek(0)
            file.write(image)


def main(command):
    result = subprocess.run(command)
    if result.returncode:
        return result.returncode
    source = output = None
    for index, argument in enumerate(command[:-1]):
        if argument == "-c":
            source = command[index + 1]
        elif argument == "-o":
            output = command[index + 1]
    names = target_names(source) if source and output else None
    if names:
        rename_sections(output, *names)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
