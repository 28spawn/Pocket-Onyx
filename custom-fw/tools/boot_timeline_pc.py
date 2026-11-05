#!/usr/bin/env python3
"""
custom-fw/tools/boot_timeline_pc.py - what did the unit do in its first seconds? READ-ONLY. Run it right after a power-on (it waits for the USB-MIDI port, which appears 1.5 - 4 s after boot).

The first 1 - 2 s of audio happen before USB is up, so nothing can be watched live. The unit keeps its own record in SRAM: the event log (include/pa_evlog.h: boot, view, preset, model, swap
requested / applied with its microseconds, over-budget audio blocks, the second core's state, the governor, the Bluetooth radio's start-up, all stamped with the 24 MHz clock), the audio
interrupt's counters (entries, overruns, longest block), the second core's status, the accelerator self-test and the main loop's pass count. This reads them all in one go and prints the events as
milliseconds after the BOOT event, with everything that could make noise in the first seconds marked:

    blocks over budget (ISR_LONG, with their microseconds)   a swap (a preset / amp / cab loading under the audio)   the second core starting or failing   the governor stepping down
    the Bluetooth radio starting (its interrupts)             a slow UI tick                                          the screen's frame counter (screen traffic)

    python custom-fw/tools/boot_timeline_pc.py        (power the unit on first; do not touch it, change no preset: the log holds only 64 events and a swap or a preset change adds to it)
"""
import struct, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m

EVLOG = 0x01C7D600
NAMES = {1: "BUTTONS", 2: "ENCODER", 3: "VIEW", 4: "PRESET", 5: "MODEL", 6: "SLOW_TICK", 7: "SWAP_REQ", 8: "SWAP_DONE", 9: "ISR_LONG (audio block over 1350 us)", 10: "BYPASS", 11: "SMP (second core)",
         12: "BOOT", 13: "RUNAWAY", 14: "AMP_RESET", 15: "GOVERNOR", 16: "AMP_SLOW", 17: "FREEZE", 18: "POOL_FAIL", 19: "LOAD_FAIL", 20: "BLE", 21: "MASTER"}
BLE = {1: "boot decision", 2: "start-up began", 3: "radio+baseband up", 4: "advertising enabled", 5: "first advertising event", 6: "a central connected", 7: "the central left", 8: "error", 9: "longest BLE interrupt", 10: "MIDI notifications on", 11: "first BLE-MIDI message"}

def connect():
    t0 = time.time()
    while time.time() - t0 < 90:
        try:
            return m.Unit("Custom MIDI")
        except SystemExit:
            time.sleep(0.25)
    raise SystemExit("the unit's MIDI port did not appear within 90 s")

def main():
    u = connect()
    t_conn = time.time()
    print("BUILD", u.build_id())
    tsta = struct.unpack("<12I", u.ram(0x01C7C390, 48))
    print(f"TSTA: main-loop passes {tsta[1]}, seconds since boot (TIMER4) {tsta[2]}  [read {time.time() - t_conn:.1f} s after the port appeared]")
    st = struct.unpack("<6I", u.ram(0x01C7C250, 24))
    smp = struct.unpack("<8I", u.ram(0x01C7D200, 32)); gov = struct.unpack("<I", u.ram(0x01C7D4C0, 4))[0]
    hw = struct.unpack("<8I", u.ram(0x01C7C800, 32))
    print(f"audio interrupt so far: {st[0]} entries ({st[0] * 1.451 / 1000:.2f} s of audio), {st[3]} overruns, longest block {st[2] / 24:.0f} us, auto-bypass {st[4]}")
    print(f"second core: {'online' if smp[1] else 'OFF (reason %d)' % smp[6]}, {smp[2]} hand-overs, its longest job {smp[4] / 24:.0f} us, core 0's longest wait for it {smp[5] / 24:.0f} us; governor level {gov & 0xFF}, average block {gov >> 8} us")
    print(f"accelerator self-test: {hw[1]} jobs, {hw[2] / 24:.0f} us total, {hw[3]} mismatches, mode {hw[5]} (1 = accelerator)")
    h = struct.unpack("<4I", u.ram(EVLOG, 16))
    if h[0] != 0x474C5645: raise SystemExit("no event log")
    n = h[1]; cap = 64
    first = max(0, n - cap)
    ev = []
    for i in range(first, n):
        ev.append(struct.unpack("<4I", u.ram(EVLOG + 16 + (i % cap) * 16, 16)))
    boot = next((e for e in ev if e[1] == 12), None)
    base = boot[0] if boot else ev[0][0]
    if not boot: print(f"(the BOOT event is not in the log any more: {n} events since boot; the times below start at the oldest event)")
    print(f"\nevent log: {n} events since boot, {len(ev)} shown (times are ms after the {'BOOT event' if boot else 'oldest event'})")
    longs = 0
    for tk, code, a, b in ev:
        t = ((tk - base) & 0xFFFFFFFF) / 24000.0
        name = NAMES.get(code, f"code {code}")
        if code == 9: longs += 1; detail = f"a block took {a} us (budget 1451)"
        elif code == 8: detail = f"swap applied in {a / 1000:.2f} ms ({'preset' if b == 0xFF else 'slot ' + str(b)})"
        elif code == 7: detail = f"({'preset' if a == 0xFF else 'slot ' + str(a)}, {b})"
        elif code == 4: detail = f"preset P{a + 1:02d} (was {('P%02d' % (b + 1)) if b != 0xFF else 'none'})"
        elif code == 20: detail = f"{BLE.get(a, a)}: {b}"
        elif code == 6: detail = f"a UI tick took {a / 1000:.1f} ms"
        elif code == 21: detail = f"output released at {a} ms: master level {b & 0xFF} % ({'the knob never answered, saved level' if b & 0x100 else 'read from the knob'})"
        elif code == 12: detail = f"boot buttons {a:#x}"
        elif code == 11: detail = f"reason {a} (was {b})"
        elif code == 15: detail = f"level {a}, average block {b} us"
        else: detail = f"a={a} b={b}"
        flag = "  <<<" if code in (9, 10, 11, 15, 17, 18, 19) or (code == 8 and a > 3000) or (code == 6) else ""
        print(f"  {t:9.1f} ms  {name:34s} {detail}{flag}")
    print(f"\nover-budget audio blocks in the log: {longs}.  Anything marked <<< before ~2000 ms is a candidate for the noise; if the log shows none, the cause is not an overloaded block.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
