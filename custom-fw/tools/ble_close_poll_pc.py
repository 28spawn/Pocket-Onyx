"""Read-only: log every link close the unit records (ble_dg close_* fields at DG+224..240) while a host connects.
For each change: reason, who closed it (3 = establishment rule), the event counter, ms since the last RX, ms since the CONNECT_IND, and the unit's clock.
The establishment rule closes after (1 + win_offset + win_size + 6 x interval) x 1.25 ms; compare since_start with it."""
import struct, sys, time
import ble_matrix_pc as m
DG = 0x01C08008
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 120.0
u = m.Unit("Custom MIDI")
print("BUILD", u.build_id(), flush=True)
last = None
t0 = time.time()
while time.time() - t0 < secs:
    try:
        b = u.ram(DG + 224, 16)
    except Exception as e:
        print("read error", e, flush=True); time.sleep(0.2); continue
    why, by, evt, since_rx, since_start, now = struct.unpack("<BBHIII", b)
    key = (why, by, evt, since_start)
    if key != last:
        print("t=%6.1fs  reason %#04x  by %d  evt %d  since_rx %.1f ms  since_start %.1f ms  unit_now %d us" % (time.time() - t0, why, by, evt, since_rx / 1e3, since_start / 1e3, now), flush=True)
        last = key
