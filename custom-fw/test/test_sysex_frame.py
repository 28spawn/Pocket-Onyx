#!/usr/bin/env python3
"""
The host's SysEx frame builder must be byte-for-byte the firmware's.

The device builds its frames in pa_sysex_build_frame() (src/sys/sysex.c). The
host tool (custom-fw/tools/mvave_device.py) has to build exactly the same
bytes, or the device will not accept a single frame.

So this does not re-implement anything: it EXTRACTS the firmware's own
function out of the source, compiles it, and compares its output with the
host tool's, case by case. If the firmware's framing changes, this fails.

    python custom-fw/test/test_sysex_frame.py
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FW = os.path.join(ROOT, "src", "sys", "sysex.c")
HOST = os.path.join(ROOT, "tools", "mvave_device.py")

sys.path.insert(0, os.path.join(ROOT, "tools"))
import mvave_device as M                                   # noqa: E402

CASES = [
    (25, bytes([0x10, 0x00, 0x00, 0x01, 0x00])),        # a NAM upload header
    (17, b""),                                            # the device-info read
    (34, bytes(range(0, 256))),                           # a full body chunk
    (34, bytes([0x80, 0xFF, 0x00, 0x7F])),                # the top bits
    (48, b"\x01\x02\x03"),                               # the commit
]


def extract_function(src, name):
    """The function's text, braces balanced."""
    start = src.index("int %s(" % name)
    i = src.index("{", start)
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[start:j + 1]
    raise RuntimeError("unbalanced braces in %s" % name)


def find_clang():
    """clang, wherever it is - the build scripts look in the WinGet folder."""
    import glob
    import shutil
    found = shutil.which("clang") or shutil.which("clang.exe")
    if found:
        return found
    pattern = os.path.join(os.environ.get("LOCALAPPDATA", ""),
                           "Microsoft", "WinGet", "Packages",
                           "MartinStorsjo.LLVM-MinGW.UCRT*", "**", "clang.exe")
    hits = glob.glob(pattern, recursive=True)
    return hits[0] if hits else None


def main():
    src = open(FW, "r", encoding="utf-8").read()
    fn = extract_function(src, "pa_sysex_build_frame")

    tmp = tempfile.mkdtemp(prefix="pa_sysex_")
    c_file = os.path.join(tmp, "frame.c")
    with open(c_file, "w", encoding="utf-8") as f:
        # the firmware's own header, so the constants are the firmware's
        f.write("#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n")
        f.write('#include "pa_sysex.h"\n')
        f.write(fn)
        f.write("\n")
        f.write("int main(void) {\n")
        for cmd, payload in CASES:
            f.write("  {\n    uint8_t p[%d] = {%s};\n" % (
                max(1, len(payload)),
                ",".join(str(b) for b in payload) or "0"))
            f.write("    uint8_t o[600];\n")
            f.write("    int n = pa_sysex_build_frame(%d, p, %d, o, sizeof(o));\n"
                    % (cmd, len(payload)))
            f.write("    for (int i = 0; i < n; i++) printf(\"%02x\", o[i]);\n")
            f.write("    printf(\"\\n\");\n  }\n")
        f.write("  return 0;\n}\n")

    exe = os.path.join(tmp, "frame.exe")
    cc = find_clang()
    if cc is None:
        print("  clang is not on the PATH - cannot run this test")
        return 1
    subprocess.run([cc, "-O1", "-I", os.path.join(ROOT, "include"),
                    "-o", exe, c_file], check=True, capture_output=True, text=True)
    out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout
    firmware = out.split()

    fails = 0
    print("=== the host builds the same frames as the firmware ===")
    for (cmd, payload), fw_hex in zip(CASES, firmware):
        host_hex = M.build_frame(cmd, payload).hex()
        ok = host_hex == fw_hex
        print(("  ok   " if ok else "  FAIL ") +
              "cmd %-3d %3d payload bytes" % (cmd, len(payload)))
        if not ok:
            print("        the firmware: %s" % fw_hex)
            print("        the host:    %s" % host_hex)
            fails += 1

    print()
    print("=== the 8-to-7-bit packing round-trips ===")
    for n in (0, 1, 7, 8, 13, 64, 1000):
        data = bytes((i * 37 + 11) & 0xFF for i in range(n))
        packed = M.pack_8to7(data)
        back = M.unpack_7to8(packed)
        ok = back[:n] == data
        print(("  ok   " if ok else "  FAIL ") +
              "%4d bytes -> %4d packed -> back" % (n, len(packed)))
        if not ok:
            fails += 1

    print()
    print("=== a packed frame is the firmware's frame, packed ===")
    cmd, payload = 25, bytes([0x10, 0x00, 0x00, 0x01, 0x00])
    plain = M.build_frame(cmd, payload)
    ok = M.unpack_7to8(plain[1:-1])[:2] == b"\x00\x59"
    print(("  ok   " if ok else "  FAIL ") +
          "the frame unpacks to the 00 59 header")
    if not ok:
        fails += 1

    print()
    print("=== a binary payload survives the packing ===")
    blob = bytes(range(256))
    wire = M.build_frame(34, blob)
    back = M.unpack_7to8(wire[1:-1])
    ok = back[6:6 + 256] == blob
    print(("  ok   " if ok else "  FAIL ") +
          "256 bytes of payload come back unchanged")
    if not ok:
        fails += 1

    print("\n" + ("THE SYSEX FRAMES MATCH" if fails == 0 else "%d FAILURE(S)" % fails))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
