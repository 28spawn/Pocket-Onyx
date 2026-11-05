#!/usr/bin/env python3
"""custom-fw/tools/ota_host_pc.py - the web editor's one-click update, from the PC without a browser: serves a package to the unit's update session over USB MIDI and logs every request, so a stop can be placed.
    python custom-fw/tools/ota_host_pc.py "<package.fwsc>" [--go]        (without --go only the handshake and step 1 run, up to the unit's own decision; nothing is written by the host)
Close the browser editor and every other MIDI program first. Step 1 only stages the loader inside the unit: nothing is committed before the 'success' answer."""
import sys, time, argparse
from pathlib import Path
import rtmidi
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mvave_device as M

ap = argparse.ArgumentParser(); ap.add_argument("pkg"); ap.add_argument("--idle", type=float, default=8.0); a = ap.parse_args()
raw = Path(a.pkg).read_bytes()
img = b"".join(raw[i * 48:i * 48 + 47] for i in range(20)) + raw[960:]
mi, mo = rtmidi.MidiIn(), rtmidi.MidiOut(); mi.ignore_types(sysex=False)
pi = next(i for i, n in enumerate(mi.get_ports()) if any(k in n.lower() for k in ("custom", "pocket", "usb-midi", "onyx")))
po = next(i for i, n in enumerate(mo.get_ports()) if any(k in n.lower() for k in ("custom", "pocket", "usb-midi", "onyx")))
mi.open_port(pi); mo.open_port(po); print("port:", mi.get_ports()[pi])
mo.send_message([0xF0, 0, 0x32, 0x45, 0, 0, 0, 0x40, 0x7F, 0xF7]); time.sleep(0.4)
ident = None
while True:
    m = mi.get_message()
    if not m: break
    if m[0][0] == 0xF0 and len(m[0]) > 30: ident = bytes(M.unpack_7to8(bytes(m[0][1:-1])))
print("identity:", ident[6:21] if ident else None)
mo.send_message([0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7]); print("update-session frame sent"); time.sleep(2.0)
n = 0; last = time.time(); t0 = last
while time.time() - last < a.idle:
    m = mi.get_message()
    if not m: time.sleep(0.001); continue
    f = m[0]
    if f[0] != 0xF0: continue
    u = bytes(M.unpack_7to8(bytes(f[1:-1])))
    if len(u) != 15 or u[2] != 0x30: print("other frame:", u[:12].hex()); continue
    addr = int.from_bytes(u[7:11], "little"); ln = int.from_bytes(u[11:14], "little"); typ = u[6]; last = time.time(); n += 1
    if addr in (0xE0000000, 0xF0000000): data = b"success\0"; print(f"#{n:3d} {time.time()-t0:5.1f}s  0x{addr:08X} -> 'success'")
    else:
        if addr + ln > len(img) or ln > 512: print(f"#{n:3d} REFUSED 0x{addr:X}+{ln}"); continue
        data = img[addr:addr + ln]; print(f"#{n:3d} {time.time()-t0:5.1f}s  read 0x{addr:06X} +{ln}")
    body = bytes([0, 0x59, 0x30, (len(data) + 8) & 255, (len(data) + 8) >> 8, 0, typ]) + addr.to_bytes(4, "little") + bytes([len(data) & 255, len(data) >> 8, 0]) + data
    body += bytes([(~sum(body[6:])) & 255]); time.sleep(0.01); mo.send_message([0xF0] + list(M.pack_8to7(body)) + [0xF7])
print(f"stopped asking after {n} requests ({a.idle:.0f} s idle)")
