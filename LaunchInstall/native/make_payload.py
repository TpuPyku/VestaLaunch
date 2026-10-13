#!/usr/bin/env python3
"""make_payload.py - pack VestaLaunch.exe and its Skin folder into payload.bin (embedded by LaunchInstall).

Format: [u32 count][count x (u16 nameLen, name UTF-8 with '/', u32 dataLen, data)][u32 CRC-32 of all before].
Names are relative to \\SDMMC\\VestaLaunch: "VestaLaunch.exe", "Skin/home.png", ...
The installer refuses to run if the CRC does not match (a damaged copy of the installer).

    python make_payload.py        (after VestaLaunch\\native\\build.bat and its /export)
"""
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.abspath(__file__))
LAUNCH = os.path.join(ROOT, "..", "..", "VestaLaunch")   # the repo's build output folder


def main():
    entries = [("VestaLaunch.exe", os.path.join(LAUNCH, "VestaLaunch.exe"))]
    skin = os.path.join(LAUNCH, "Skin")
    for name in sorted(os.listdir(skin)):
        if os.path.isfile(os.path.join(skin, name)):
            entries.append(("Skin/" + name, os.path.join(skin, name)))
    out = bytearray(struct.pack("<I", len(entries)))
    for name, path in entries:
        data = open(path, "rb").read()
        nb = name.encode("utf-8")
        out += struct.pack("<H", len(nb)) + nb + struct.pack("<I", len(data)) + data
    out += struct.pack("<I", zlib.crc32(bytes(out)) & 0xFFFFFFFF)
    path = os.path.join(ROOT, "payload.bin")
    open(path, "wb").write(out)
    print("payload.bin: %d files, %d bytes" % (len(entries), len(out)))


if __name__ == "__main__":
    main()
