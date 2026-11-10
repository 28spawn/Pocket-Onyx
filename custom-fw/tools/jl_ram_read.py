#!/usr/bin/env python3
"""
custom-fw/tools/jl_ram_read.py - READ-ONLY RAM reader for the Pocket Amp in the ROM download mode,
over the direct USB cable (no RP2040).

The firmware keeps its logs in SRAM, which survives the reset into the mask-ROM download mode:
the black box (0x01C7C000), the crash record (0x01C7C300), the USB driver's debug words (0x01C7C3C0)
and the audio-interrupt stats (0x01C7C250). The RP2040 dongle reads them with its `memr` command;
this tool does the same through jl-uboot-tool's ROM-stage READ_MEMORY, so a unit that is powered and
connected by one USB-C cable can be inspected after a stall, a fault or a deliberate return.

It ONLY issues READ_MEMORY. It never writes memory, never uploads a loader, never jumps and never
touches flash (owner's rule: nothing here can change the unit). It must run with Administrator
rights on Windows (raw SCSI access), like jl_safe_flash.py.

    python custom-fw/tools/jl_ram_read.py --jl-tool <dir> --device "\\\\.\\PHYSICALDRIVE2" usb
    python custom-fw/tools/jl_ram_read.py ... stats
    python custom-fw/tools/jl_ram_read.py ... bb --out blackbox.bin
    python custom-fw/tools/jl_ram_read.py ... raw 0x01C7C300 256

Needs jl-uboot-tool (MIT, kagaimiq): --jl-tool <dir> or JL_UBOOT_TOOL=<dir>.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
from pathlib import Path

CHIP_FAMILY = "wl82"        # AC791N
CHUNK = 256                 # the ROM's READ_MEMORY is used in pieces of at most 512 bytes; stay well below


class Rom:
    def __init__(self, jl_tool: Path, device: str | None):
        if not (jl_tool / "jltech" / "uboot.py").exists():
            raise SystemExit(f"jl-uboot-tool not found at {jl_tool} (use --jl-tool or JL_UBOOT_TOOL)")
        sys.path.insert(0, str(jl_tool))
        import yaml
        from jltech.uboot import JL_MSCDevice, JL_UBOOT
        from jltech.cipher import cipher_bytes, jl_crc_cipher
        self._cipher_bytes, self._cipher = cipher_bytes, jl_crc_cipher
        data = jl_tool / "data"
        chips = yaml.safe_load(open(data / "chips.yaml"))
        loaders = yaml.safe_load(open(data / "usb-loaders.yaml"))
        if device is None:
            from jldevfind import choose_jl_device
            device = choose_jl_device(venfilter=CHIP_FAMILY)
            if device is None:
                raise SystemExit("no JieLi device in download mode found")
        self.msc = JL_MSCDevice(device)
        self.msc.open()
        vendor, product, _rev = self.msc.inquiry()
        if vendor.strip().lower() != CHIP_FAMILY:
            raise SystemExit(f"the device says it is '{vendor.strip()}', not {CHIP_FAMILY.upper()} (AC791N)")
        if product.strip() != "UBOOT1.00":
            raise SystemExit(f"the device is '{product.strip()}': RAM can only be read in the ROM stage (UBOOT1.00), "
                             "before a loader runs. Power-cycle into the download mode and try again.")
        # WL82 UBOOT1.00 processes memory reads through the standard JieLi CRC stream encoding
        # (key 0xFFFFFFFF) per hardware specification. The encoding is a symmetric stream,
        # keyed afresh for each READ_MEMORY command, so it is applied per command.
        self.rom = JL_UBOOT(self.msc.dev)

    def read(self, addr: int, n: int) -> bytes:
        out = bytearray()
        while n > 0:
            k = min(n, CHUNK)
            raw = bytes(self.rom.mem_read(addr, k))
            out += self._cipher_bytes(self._cipher, raw)
            addr += k
            n -= k
        return bytes(out)

    def close(self):
        try:
            self.msc.close()
        except Exception:
            pass


def words(raw: bytes) -> tuple:
    return struct.unpack(f"<{len(raw) // 4}I", raw)


def show_usb(rom: Rom):
    w = words(rom.read(0x01C7C3C0, 32))
    flags, resets, setups, rx, tx, timeouts = w[0], w[1], w[2], w[3], w[4], w[5]
    setup = struct.pack("<II", w[6], w[7])
    print("USB device driver (SRAM 0x01C7C3C0):")
    print(f"  up={flags & 1}  configured={(flags >> 1) & 1}  suspended={(flags >> 2) & 1}   (raw flags {flags:#x})")
    print(f"  bus resets seen        : {resets}")
    print(f"  SETUP packets handled  : {setups}")
    print(f"  MIDI OUT packets in    : {rx}   (host -> unit)")
    print(f"  MIDI IN packets sent   : {tx}   (unit -> host)")
    print(f"  SIE timeouts (current) : {timeouts}")
    print(f"  last SETUP packet      : {setup.hex(' ')}  "
          f"(bmRequestType {setup[0]:#04x}, bRequest {setup[1]:#04x}, wValue {setup[2] | setup[3] << 8:#06x}, "
          f"wIndex {setup[4] | setup[5] << 8:#06x}, wLength {setup[6] | setup[7] << 8})")


def show_usb_tx(rom: Rom):
    w = words(rom.read(0x01C7C400, 32))
    print("USB IN (TX) diagnostics (SRAM 0x01C7C400):")
    print(f"  TXCSR1 before last send : {w[0]:#04x}")
    print(f"  TXCSR1 right after send : {w[1]:#04x}   (bit0 TxPktRdy, bit1 FIFONotEmpty, bit2 UnderRun, bit7 IncompTx)")
    print(f"  sends completed         : {w[2]}   (TxPktRdy seen cleared)")
    print(f"  polls still pending     : {w[3]}   (last TXCSR1 then: {w[7]:#04x})")
    print(f"  TXCSR2 / TXMAXP / CNT   : {w[4]:#04x} / {w[5]:#04x} / {w[6]}")


def show_slots(rom: Rom, compact: bool = False):
    w = words(rom.read(0x01C7C500, 128))
    names = ["DYN", "DRV", "AMP", "CAB", "EQ", "MOD", "DLY", "REV"]
    print("measured time per effect slot (SRAM 0x01C7C500; budget per 64-sample block = 1451 us):")
    print("  slot   calls   avg us   max us   last us")
    for i, n in enumerate(names):
        mx, sm, calls, last = w[4 * i:4 * i + 4]
        if compact and not calls:
            continue
        avg = sm * 16 / calls / 24 if calls else 0          # the firmware sums in 16-tick units and halves the sum and the call count before the sum can wrap (builds before 2026-11-02 wrapped it after ~1 h of the amp / ~7 h of the others: their averages after a long run are wrong)
        print(f"  {n:4} {calls:7} {avg:8.0f} {mx / 24:8.0f} {last / 24:9.0f}")


def show_battery(rom: Rom):
    w = words(rom.read(0x01C7D000, 224))
    cp = words(rom.read(0x01C7C3F4, 4))[0]
    if w[0] != 0x54414256:
        print(f"main-loop checkpoint word (0x01C7C3F4): {cp:#x}  (0x71 mux write, 0x72 P33 reg 4 write, 0x73 written, 0x74 converting, 0x75 done)")
        print("battery diagnostics: not present (build without the SARADC battery reader)")
        return
    sw = lambda v: v - (1 << 32) if v & 0x80000000 else v
    if w[39] == 2:
        print(f"battery / SARADC (SRAM 0x01C7D000), {w[1]} polls, {w[34]} conversion timeouts, ADC dead flag {w[35]}; reader WITHOUT the PMU helper:")
        print(f"  channel 15 as the boot code left it: {w[2]}   (not a calibrated reading; there is no battery voltage in this build)")
        print("  raw by plain channel    : " + "  ".join(f"{c}:{w[16 + c]}" for c in range(15)))
        return
    print(f"battery / SARADC (SRAM 0x01C7D000), {w[1]} polls, {w[34]} conversion timeouts, ADC dead flag {w[35]}:")
    tb, trim = w[3] & 0xFF, (w[3] >> 8) & 0xFFFF
    trim = trim - 0x10000 if trim & 0x8000 else trim
    print(f"  stage marker 0x01C7C3F4 : {cp:#x}   P33 reg 4 as found {w[40]:#04x}, with channel selected {w[41]:#04x}, after restore {w[42]:#04x}")
    print(f"  bandgap raw (PMU ch 0)  : {w[2]}   trim byte {tb:#04x} -> {trim:+d} mV, Vbg {w[4]} mV")
    print(f"  ADC reference (supply)  : {w[5]} mV   (= 1023 * Vbg / raw, stock boot formula)")
    print(f"  VBAT raw (PMU ch 3)     : {w[32]}   -> {w[33]} mV unfiltered (assumes VBAT/4); rejected as implausible: {w[37]}")
    print(f"  filtered battery        : {w[38]} mV" if w[38] else "  filtered battery        : no valid fix yet (plat_battery_mv() = 0)")
    print("  raw by PMU channel      : " + "  ".join(f"{k}:{w[6 + k]}" for k in range(8)))
    print("  raw by plain channel    : " + "  ".join(f"{c}:{w[16 + c]}" for c in range(15)))
    if w[6] == 0 and w[7] == 0:
        print("  (build without the PMU helper: \"bandgap raw\" is channel 15 as the boot code left it, the supply and Vbg lines mean nothing, no battery voltage)")
    if w[43]:
        print("  first sweep after boot (bandgap raw %d): PMU raw " % w[44] + "  ".join(f"{k}:{w[45 + k]}" for k in range(8)))
        print("  PMU channel -> mV relative to the bandgap (Vbg %d mV), first sweep | last sweep   (x1 / x3 / x4):" % w[4])
        for k in range(6):
            f, l = w[45 + k] * w[4] / max(w[44], 1), w[6 + k] * w[4] / max(w[2], 1)
            print(f"    ch{k}: {f:6.0f} {3 * f:6.0f} {4 * f:6.0f}  |  {l:6.0f} {3 * l:6.0f} {4 * l:6.0f}")
    print("  hint: a PMU channel reading ~ VBAT/4 gives raw ~= 1023 * (VBAT/4) / supply (VBAT 3.7 V, supply 3.4 V -> ~278)")


def show_clock(rom: Rom):
    w = words(rom.read(0x01C7C5A0, 12))
    if w[2] != 0x4B4C4352:
        print("app core clock: not measured (build without the clock probe)")
        return
    print(f"app core clock (64 dependent adds, run by the firmware at boot): {w[1]} MHz   ({w[0]} TIMER4 ticks)")


def show_pll(rom: Rom):
    w = words(rom.read(0x01C7DB00, 16))
    if w[0] != 0x324C4C50:
        print("system PLL: not read (older build, or '<' held at power-on)")
        return
    note = {270: "= the 360 MHz the FM-1 boot loader sets", 240: "= 320 MHz", 297: "= 396 MHz"}.get(w[2], "(not one of the known FM-1 values)")
    print(f"system PLL (PLL2_CON1 at 0x119AC, read only): raw {w[1]:#x}, multiplier {w[2]} -> SYS clock {w[3]} MHz {note}")


def show_clock_dump(rom: Rom):
    w = words(rom.read(0x01C7E900, 320))
    if w[0] != 0x444B4C43:
        print("clock dump: not taken (older build, '<' held at power-on, or a previous dump did not finish)")
        return
    names = ["0x119A0 (PLL2 and its neighbours)", "0x16A00 (the PLL the stock start-up configures)", "0x10000 (clock gate; +8 is the core 1 gate)",
             "0x40200 (read by the stock's clock table)", "0x40400 (read by the stock's clock table)", "0x11C00"]
    sizes = [16, 8, 8, 4, 4, 4]
    print("clock tree registers (read only; compare with the stock):")
    o = 8
    for n, sz, base in zip(names, sizes, w[1:7]):
        print(f"  {n}:")
        print("    " + "  ".join(f"+{4 * i:02X}:{w[o + i]:08X}" for i in range(sz)))
        o += sz
    p33 = w[7]
    print("  P33 registers 0..15: " + " ".join(f"{w[p33 + i]:02X}" for i in range(16)) + f"   0x80: {w[p33 + 16]:02X}  0xA0: {w[p33 + 17]:02X}")


def show_contention(rom: Rom):
    w = words(rom.read(0x01C7E800, 128))
    if w[0] != 0x544E4F43:
        print("core contention test: not run (older build, '<' held at power-on, or not yet 30 s after boot)")
        return
    names = ["idle", "ALU only", "SRAM reads", "SRAM writes", "flash (XIP) reads", "peripheral register reads", "idle again"]
    done = w[2] == 0x444F4E45
    print(f"core contention test: core 0 busy 12 of every 16 ms while core 1 runs the amp; {w[1]} of 7 phases done{'' if done else ' (still running)'}")
    base = None
    for i in range(min(w[1], 7)):
        avg, calls, it, wn = w[4 + 4 * i: 8 + 4 * i]
        us = avg / 24.0
        if i == 0:
            base = us
        flag = "  <-- the WaveNet engine ran, not comparable" if wn else ""
        rel = f"{(us / base - 1) * 100:+5.1f} %" if base else "   -"
        print(f"  {names[i]:26s} amp slot avg {us:7.1f} us ({rel}), {calls} blocks, core 0 loop passes {it}{flag}")


def show_accel(rom: Rom):
    w = words(rom.read(0x01C7C5B0, 20))
    print(f"vector accelerator (0x01EE0400): jobs done {w[4]}, timeouts {w[2]}, marked broken {w[3]}, "
          f"last job {w[0] / 24:.1f} us, longest {w[1] / 24:.1f} us")


def show_boot(rom: Rom):
    w = words(rom.read(0x01C7C600, 96))
    if w[5] != 0x544F4F42:
        print("boot trace: not present (build without it)")
        return
    bl = 1 << 9
    is_input = bool(w[1] & bl)                     # DIR bit set = input: the level is in IN, not OUT
    level = bool(w[4] & bl) if is_input else bool(w[0] & bl)
    print(f"  PORTA as the app starts: OUT {w[0]:#x} DIR {w[1]:#x} PU {w[2]:#x} PD {w[3]:#x} IN {w[4]:#x}")
    print(f"  backlight pin PA9 at start: {'HIGH (light off)' if level else 'LOW (light ON)'}, "
          f"{'input' if is_input else 'output'}")
    names = {8: "app start (timer running)", 9: "before clock calibration", 10: "after calibration / before clock probe",
             11: "before gpio scan init", 12: "before LCD init", 13: "LCD init table sent", 14: "panel blanked",
             15: "LCD init done (light still off)", 16: "LCD init returned", 17: "first main-loop pass", 18: "backlight switched on"}
    if w[19] or w[18]:
        names19 = [n for b, n in ((0, "HOME"), (1, "VOL-"), (2, "VOL+"), (3, "<"), (4, ">"), (5, "PARA")) if w[19] & (1 << b)]
        print(f"  buttons seen at power-on: {w[19]:#x} ({', '.join(names19) or 'none'})")
    t0 = w[8]
    for i in range(8, 19):
        if w[i]:
            print(f"  +{(w[i] - t0) / 24000:8.2f} ms  {names[i]}")


def show_gru(rom: Rom):
    hw = words(rom.read(0x01C7C800, 24))
    mode_hw = False
    if hw[0] == 0x54535748:
        mode_hw = bool(hw[5])
        jobs, ticks = hw[1], hw[2]
        avg = (ticks / 24.0 / jobs) if jobs else 0.0
        print(f"accelerator matvec self-test: {jobs} jobs, mismatches {hw[3]}, passed {hw[4]}, "
              f"avg {avg:.2f} us per 9-row job; GRU mode: {'ACCELERATOR matvec' if mode_hw else 'CPU float'}")
    w = words(rom.read(0x01C7C660, 32))
    n = w[7]
    if not n:
        print("GRU step profile: no samples recorded")
        return
    if mode_hw:
        names = ["six accelerator jobs (submit + wait)", "activations (32 sigmoid, in float)", "combine, tanh, state, head", "post filters (defizz, DC)"]
    else:
        names = ["gates (2 x 16x17 matvec + 32 sigmoids)", "candidate (16x16 matvec)", "combine, tanh, state, head", "post filters (defizz, DC)"]
    total = sum(w[:4])
    print(f"float GRU step profile ({n} samples; timer-read overhead is included, so trust the proportions):")
    for i, nm in enumerate(names):
        print(f"  {nm:42} {w[i] / 24 / n:7.2f} us/sample  {100 * w[i] / total:5.1f}%")
    print(f"  {'sum':42} {total / 24 / n:7.2f} us/sample")


def show_mem(rom: Rom):
    w = words(rom.read(0x01C7C780, 64))
    if w[15] != 0x504D454D:
        print("memory probe: not present (build without it)")
        return
    clk = words(rom.read(0x01C7C5A0, 12))[1] or 328        # MHz, from the boot clock probe
    cyc = lambda ticks, count: ticks * clk / 24.0 / count   # core cycles per element
    print(f"memory probe (cycles per element at {clk} MHz; one TIMER4 tick = {clk / 24:.1f} cycles):")
    print(f"  independent loads, pool SRAM      cold {cyc(w[0], 4096):6.1f}   warm {cyc(w[1], 4096):6.1f}")
    print(f"  dependent loads (latency), pool   pass1 {cyc(w[2], 8192):6.1f}  pass2 {cyc(w[3], 8192):6.1f}")
    print(f"  dependent loads, .bss-side SRAM   {cyc(w[4], 8192):6.1f}")
    print(f"  sequential loads from XIP flash   cold {cyc(w[5], 4096):6.1f}   warm {cyc(w[6], 4096):6.1f}")
    print(f"  float dot product (2 accumulators) {cyc(w[7], 2048):6.1f} per pair of multiply-adds")
    print(f"  64-bit pair loads, pool SRAM       {cyc(w[9], 4096):6.1f} per 32-bit word")
    print(f"  low SRAM 0x01C01000: dependent {cyc(w[10], 8192):6.1f}, sequential {cyc(w[11], 4096):6.1f}, 64-bit pairs {cyc(w[13], 4096):6.1f} (per word)")
    print(f"  gap SRAM 0x01C06000: sequential {cyc(w[12], 2048):6.1f}")
    r = words(rom.read(0x01C7C7C0, 32))
    if r[2] == 0x52544D50:
        print(f"  amp engine flag {r[4]} (0 GRU, 1 WaveNet); WaveNet model active {r[5]}, GRU model active {r[6]}")
    else:
        print("  runtime probe: not recorded yet (it runs about 3 s after the main loop starts)")


def show_ram(rom: Rom):
    w = words(rom.read(0x01C7C700, 32))
    if w[0] != 0x504D4152:
        print("RAM probe: not present (build without it)")
        return
    print(f"RAM probe (SRAM 0x01C7C700): tested {w[1]:#x}..{w[2]:#x} ({(w[2] - w[1]) // 1024} KB), every 64th word, 2 patterns")
    print(f"  words tested {w[3]}, failures {w[4]}" + (f" (first {w[5]:#x}, last {w[6]:#x})" if w[4] else ""))
    print(f"  proven good (sampled): about {w[7] // 1024} KB of {(w[2] - w[1]) // 1024} KB")


def show_decode(rom: Rom):
    base = 0x01C7C840
    names = ["r gate, rows 0..8", "r gate, rows 7..15", "z gate, rows 0..8", "z gate, rows 7..15", "n gate, rows 0..8", "n gate, rows 7..15"]
    any_ = False
    for k, nm in enumerate(names):
        w = words(rom.read(base + 64 * k, 56))
        if w[0] != 0x434F4544:
            continue
        if not any_:
            print("accelerator matvec output decode (a job was given a 200-word sentinel buffer; which words did it write?):")
            any_ = True
        idx = [(x >> 24, x & 0xFFFFFF) for x in w[2:2 + min(w[1], 12)]]
        print(f"  {nm:22} words changed {w[1]:4}   first (index:value) " + " ".join(f"{i}:{v:#08x}" for i, v in idx))


def show_cpu1(rom: Rom):
    w = words(rom.read(0x01C7CA00, 320))   # 8 header words + up to 32 (address, value) pairs
    if w[0] != 0x52503143:
        print("CPU1 probe: not run (hold VOL- and VOL+ while switching on to run it)")
        return
    done = w[3] == 0xC1D0
    print(f"CPU1 probe (read-only): {w[2]} reads completed, {'FINISHED' if done else 'STOPPED - last address being read: ' + hex(w[1])}")
    for i in range(w[2]):
        print(f"  [{w[8 + 2 * i]:#010x}] = {w[9 + 2 * i]:#010x}")
    sv = words(rom.read(0x01C7CE00, 80))
    if sv[18] == 0x50333353:
        print("  P33 register survey: " + " ".join(f"{i:02x}={sv[i]:02x}" for i in range(16)) + f"  80={sv[16]:02x} a0={sv[17]:02x}")
    if done:
        print(f"  supply levels now: VDC14 {w[4]} (3 = 1.40 V), SYSVDD {w[5]} (11 = 1.26 V)")


def show_identity(rom: Rom):
    w = words(rom.read(0x01C7CD00, 164))
    if w[40] != 0x44454D49:
        return
    print("accelerator identity test (W = identity, v[c] = (c+1)*4096; a job should give out[r] = (r+1)*4096 = 4096, 8192, ...):")
    print("  first 40 output words:", " ".join(f"{x:#x}" if x != 0x7FEEDBAC else "." for x in w[:40]))


def show_c1test(rom: Rom):
    w = words(rom.read(0x01C7CC00, 256))
    if w[0] != 0x54533143:
        print("CPU1 test: not run (hold HOME + VOL- + VOL+ while switching on; with '<' instead of HOME it raises the core supply first)")
        return
    prog = {1: "entered", 2: "voltages done", 3: "guards set", 4: "released, waiting for core 1", 5: "core 1 alive", 9: "FINISHED"}
    st = w[1]
    print(f"CPU1 test: progress {st} ({prog.get(st, 'DEAD: core 1 did not come alive' if st == 0xDEAD0001 else 'stopped in the loops')})")
    print(f"  supply levels before: VDC14 {w[16]} (3 = 1.40 V), SYSVDD {w[17]} (11 = 1.26 V); after: VDC14 {w[20]}, SYSVDD {w[21]}")
    print(f"  core 1 control register {w[18]:#x}, clock register {w[19]:#x} as the bootloader left them")
    print(f"  entry word readback {w[28]:#x} (should be {w[29]:#x}); PC limit 0 readback L {w[30]:#x} H {w[31]:#x}")
    print(f"  core 1 stack-limit bank readback: EMU_CON {w[32]:#x}, limit {w[33]:#x}; DBG_MSG {w[34]:#x}, EMU_MSG {w[35]:#x} (before the start)")
    print(f"  core 1 heartbeat from the flash landing stub: {w[61]} (2000 = it ran the whole flash loop; 0 = never got there)")
    print(f"  RAM image at 0x01C00000 vs its flash copy just before the start: {w[57]} of {w[58]} words differ (first word RAM {w[59]:#x}, flash {w[60]:#x})")
    print(f"  stack-limit bank readback after the stock-order writes: CON {w[52]:#x} (+0), +8 {w[53]:#x}, +12 {w[54]:#x}, +16 {w[55]:#x}, +20 {w[56]:#x}")
    for k, nm in enumerate(["STOCK replica: flash stub, no clock write", "flash stub + clock bit", "stub in RAM, no clock write"]):
        if w[36 + k] or w[39 + k] or w[42 + k]:
            print(f"  start ordering {k} [{nm}]: alive {w[36 + k]}, breadcrumb {w[39 + k]:#x}, control register after {w[42 + k]:#x}, DBG_MSG {w[45 + k]:#x}, EMU_MSG {w[48 + k]:#x}")
    print(f"  alive {w[2]}, core 1 breadcrumb {w[3]:#x}" + (f", started by ordering {w[51]}" if st >= 5 else ""))
    if st < 5:
        return
    hz = lambda ticks: (20000 * 64 * 24.0 / ticks) if ticks and ticks != 0xFFFFFFFF else 0
    print(f"  A clock: core 1 {hz(w[4]):.0f} MHz, core 0 {hz(w[5]):.0f} MHz  (64 dependent adds x 20000)")
    cyc = lambda ticks, mhz: (ticks * mhz / 24.0 / 4096) if ticks and ticks != 0xFFFFFFFF else 0
    c0 = hz(w[5]) or 328
    print(f"  B 4096 loads alone:   core 1 {cyc(w[6], c0):.1f} cycles/word, core 0 {cyc(w[7], c0):.1f}   (sums {w[22]:#x} / {w[23]:#x}, must be equal)")
    print(f"  C 4096 loads at once: core 1 {cyc(w[9], c0):.1f} cycles/word, core 0 {cyc(w[8], c0):.1f}   (alone: see B)")
    print(f"  D integrity: {w[10]} jobs sent, {w[11]} done, {w[12]} refused by the check word, {w[13]} wrong results, {w[14]} timeouts   -> {'CLEAN' if w[15] == 0xC1D0 and not (w[12] or w[13] or w[14]) else 'PROBLEMS'}")
    print(f"  core 1 stack used: {w[24]} bytes")


def show_c1diag(rom: Rom):
    w = words(rom.read(0x01C7CF00, 128))
    if w[0] != 0x44473143:
        return
    names = ["STOCK replica: flash stub, no clock write", "flash stub + clock bit", "stub in RAM, no clock write"]
    print(f"CPU1 exception diagnostics (variant {w[30]}: {names[w[30]] if w[30] < 3 else '?'}):")
    print(f"  breadcrumb {w[29]:#x} (1 = flash stub ran, 3 = stacks set, 4 = landed in RAM, 2 = C main ran); core 1 control register {w[28]:#x}")
    print("  core 1 EMU bank (0x01EEF2D0..): " + " ".join(f"{x:#x}" for x in w[1:9]) + "   <- word 1 (+4) is its EMU_MSG, the exception reason")
    print(f"  core 1 ETM_CON {w[9]:#x}; last branch targets: " + " ".join(f"{x:#x}" for x in w[10:16]))
    print("  debug window 0x01EEE240..: " + " ".join(f"{x:#x}" for x in w[16:24]))
    print(f"  PC limits now: 0 H {w[24]:#x} L {w[25]:#x}, 1 H {w[26]:#x} L {w[27]:#x}")


def show_smp(rom: Rom):
    w = words(rom.read(0x01C7D200, 64))
    if w[0] != 0x53504D53:
        print("second core (audio pipeline): no status (build without it)")
        return
    why = {0: "running", 1: "not started", 2: "skipped by the buttons (> held, or the test combination)", 3: "start FAILED (core 1 did not answer)",
           4: "TIMED OUT (missed a whole block) and was switched off", 5: "refused a job (bad memory read) and was switched off",
           6: "stack nearly full and was switched off"}
    print(f"second core (audio pipeline): {'ONLINE' if w[1] else 'off'} - {why.get(w[6], w[6])}")
    if w[1] or w[2]:
        print(f"  hand-overs {w[2]}; core 1's front-of-chain (amp) work: last {w[3] / 24:.0f} us, longest {w[4] / 24:.0f} us (budget 1451 us); "
              f"core 0 waited for it {w[7]} times, longest {w[5] / 24:.1f} us")
        f = words(rom.read(0x01C7E7C0, 20))
        if f[0] == 0x544E5246:
            print(f"  inside that job (last block): before the slot loop {f[4] / 24.0:.0f} us, the slot loop {f[3] / 24.0:.0f} us, all of run_front {f[1] / 24.0:.0f} us (longest {f[2] / 24.0:.0f} us)")
        print(f"  core 1 stack used {w[8]} of {w[9]} bytes")


def show_levels(rom: Rom):
    w = words(rom.read(0x01C7D300, 128))
    if w[31] != 0x4C564C53:
        print("signal levels: not recorded (the chain has not run since boot, or a build without the meters)")
        return
    one = 2097152.0
    names = ["raw input", "after input conditioning (trim, gate)", "after the front: amp on core 1", "as received by core 0", "after cab/delay/reverb", "after the tail (EQ, volume, limiter)"]
    print("signal level along the chain (largest sample since boot / in the last block, % of full scale; play the guitar first):")
    print(f"  stereo at the output: right channel peak {100 * w[6] / one:.3f}% (last block {100 * w[14] / one:.3f}%); "
          f"largest left-right difference {100 * w[7] / one:.3f}% (last block {100 * w[15] / one:.3f}%)  [0 = dual mono]")
    for i, nm in enumerate(names):
        print(f"  {i}  {nm:40} {100 * w[i] / one:8.3f}%   last block {100 * w[8 + i] / one:8.3f}%")
    s = lambda v: v - (1 << 32) if v & 0x80000000 else v
    print(f"  chain gains: input trim {s(w[16]) / one:.3f}, global input gain {s(w[17]) / one:.3f}, preset volume {s(w[18]) / one:.3f} (smoothed {s(w[19]) / one:.3f}), "
          f"noise-gate threshold {s(w[20])}, gate gain {s(w[21]) / one:.3f}")
    slots = ["DYN", "DRV", "AMP", "CAB", "EQ", "MOD", "DLY", "REV"]
    print("  active slots: " + (" ".join(n for i, n in enumerate(slots) if w[22] >> i & 1) or "none") + f"; force_cab_off {w[23]}; output limiter mode {w[24]}")
    sp = words(rom.read(0x01C7D400, 64))
    print("  output peak of each slot (since boot / last block, % of full scale):  " +
          "  ".join(f"{slots[i]} {100 * sp[i] / one:.0f}/{100 * sp[8 + i] / one:.0f}" for i in range(8) if sp[i] or sp[8 + i]))


def show_xip(rom: Rom):
    w = words(rom.read(0x01C7D580, 16))
    print(f"flash read check (constants read twice): {w[0]} reads disagreed, {w[2]} never agreed" +
          (f", last bad address {w[1]:#x}" if w[0] else " (the reads of fonts, strings and tables were reliable)"))


FP_BASE, FP_WORDS, FP_NWIN, FP_WW = 0x01C72000, 1680, 72, 16     # flash_probe.c: the probe block (PA_FLASH_PROBE), mirror of its struct
FP_DATA = 528
FP_STATUS = {0: "pending", 1: "ok", 2: "FAULT", 3: "skipped", 0x5052: "in progress (died here)"}


def fp_decode(w, app: bytes | None = None, head: bytes | None = None) -> list:
    """Pure decoding of the probe block words (also used by test_flash_probe.py). Returns lines of text."""
    out = []
    if w[0] != 0x42525046:
        return ["flash probe: no block (build without PA_FLASH_PROBE=1, '<' held at power-on, or not yet 3 s after boot)"]
    out.append(f"flash probe: build {w[2]}, runs {w[5]}, windows faulted {w[6]}, pool end {w[7]:#x}, "
               f"sweep {'COMPLETE' if w[3] == 0x444F4E45 else 'not finished'} in {w[9] / 24000:.2f} ms")
    xip0, xip1 = struct.pack("<16I", *w[16:32]), struct.pack("<16I", *w[32:48])
    out.append(f"  XIP reads that disagreed between two passes: {w[8]}")
    out.append(f"  XIP 0x02000000: {xip0[:32].hex(' ')}")
    out.append(f"  XIP 0x02000120: {xip1[:32].hex(' ')}")
    if app is not None:
        out.append("  0x02000120 == build_app.bin[0:64]: " + ("YES -> the XIP window shows the app at the link origin" if xip1 == app[:64] else "NO"))
    if head is not None:
        out.append("  0x02000000 == unpacked area head of the pack (flash 0x4000): " +
                   ("YES -> the XIP base is flash 0x4000" if xip0 == head[:64] else "NO (so the base is not 0x4000 for this pack, or the head differs)"))
    out.append("  windows (16 words each); '!' marks a word that may hold the XIP offset (0x4000/0x5000, or >>8, >>12) or a read command (03 0B 3B 6B EB):")
    for i in range(FP_NWIN):
        st, base = w[128 + i], w[208 + i]
        d = w[FP_DATA + FP_WW * i: FP_DATA + FP_WW * (i + 1)]
        name = FP_STATUS.get(st, hex(st))
        if st != 1:
            out.append(f"  {base:#010x}  {name}")
            continue
        if not any(d) and False:
            continue
        hits = [k for k, v in enumerate(d) if v in (0x4000, 0x5000, 0x40, 0x50, 0x4, 0x5) or (v & 0xFF) in (0x03, 0x0B, 0x3B, 0x6B, 0xEB) and v < 0x100]
        hits = [k for k in hits if d[k] in (0x4000, 0x5000, 0x40, 0x50)] + [k for k in hits if d[k] in (0x03, 0x0B, 0x3B, 0x6B, 0xEB)]
        out.append(f"  {base:#010x}  " + " ".join(f"{v:08X}" + ("!" if k in hits else " ") for k, v in enumerate(d)))
    return out


def show_flash_probe(rom: Rom, pack: str | None = None):
    w = words(rom.read(FP_BASE, 4 * FP_WORDS))
    app = head = None
    try:
        app = Path(__file__).resolve().parent.parent.joinpath("build_app.bin").read_bytes()
    except OSError:
        pass
    if pack:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import jl_app_area
        head = jl_app_area.scramble(Path(pack).read_bytes()[:64], jl_app_area.AREA_START)
    for line in fp_decode(w, app, head):
        print(line)


def show_fir(rom: Rom, full=False):
    names = ["A: coefficients = impulse at 0, history = ramp (i+1)*4096",
             "B: coefficients = ramp (k+1)*4096, history = impulse (1.0) at index 10",
             "C: coefficients = impulse at 63, history = ramp (i+1)*4096"]
    w = words(rom.read(0x01C7DC00, 3 * 144 * 4))
    any_ = False
    for ex in range(3):
        e = w[144 * ex: 144 * (ex + 1)]
        if e[0] != 0x46495230 + ex:
            continue
        if not any_:
            print("cabinet FIR experiment (accelerator FIR job, 64 taps, count 65; Q18 values: 1.0 = 262144):")
            any_ = True
        print(f"  {names[ex]}")
        print(f"    words written {e[1]}, first {e[2]}, one past the last {e[3]}")
        sv = lambda v: v - (1 << 32) if v & 0x80000000 else v
        out = e[4:144]
        shown = range(len(out)) if full else list(range(0, 12)) + list(range(60, 72)) + list(range(124, 134))
        line = []
        for i in shown:
            line.append(f"{i}:{'.' if out[i] == 0x7FEEDBAC else sv(out[i])}")
        print("    " + "  ".join(line))
    if not any_:
        print("cabinet FIR experiment: not run (the accelerator self-test did not pass, or an older build)")


def show_firbench(rom: Rom):
    w = words(rom.read(0x01C7E400, 40))
    if w[0] == 0x42524946:
        us = lambda t: t / 24.0
        print("accelerator spectral multiply vs the CPU (one 65-bin block, 256 repeats, accelerator otherwise idle):")
        print(f"  accelerator, overwrite job : {us(w[1]) / 256:.2f} us average, {us(w[4]):.2f} us longest")
        print(f"  accelerator, accumulate job: {us(w[2]) / 256:.2f} us average, {us(w[9]):.2f} us longest")
        print(f"  CPU float multiply-accumulate of the same 65 bins: {us(w[3]) / 256:.2f} us")
        print(f"  accuracy over {w[8]} words of random 24-bit spectra: {w[5]} differ from floor(a*b >> 18), {w[6]} differ from round-to-nearest; largest difference from the rounded value {w[7]}")
    else:
        print("accelerator spectral multiply benchmark: not run")
    p = words(rom.read(0x01C7E440, 40))
    if p[0] == 0x52504346 and p[1]:
        n = p[1]
        f = lambda t: t / 24.0 / n
        tot = f(p[2]) + f(p[3]) + f(p[4]) + f(p[5])
        print(f"the CPU cabinet in the real audio path, averaged over {n} blocks ({p[6]} of {p[7]} partitions summed):")
        print(f"  shift/copy {f(p[2]):.1f} us   forward FFT {f(p[3]):.1f} us   multiply-accumulate {f(p[4]):.1f} us   inverse FFT {f(p[5]):.1f} us   = {tot:.1f} us (before the filters and the level)")
    else:
        print("the CPU cabinet profile: no blocks counted (cabinet off, or an older build)")


def show_wnop(rom: Rom):
    desc = ["rows 8, cols 48, stride 0x40000C: A = ramp (j+1)*4096, B = impulse 4096 at word 10",
            "rows 8, cols 48, stride 0x40000C: A = impulse 4096 at 5, B = ramp (i+1)*4096",
            "rows 8, cols 48, stride 0x00000C: A = ramp, B = impulse at 10",
            "rows 8, cols 20, stride 0xC00050: A = ramp, B = impulse at 10",
            "rows 8, cols 20, stride 0xC00050: A = impulse at 5, B = ramp",
            "rows 35, cols 20, stride 0xC00050: A = ramp, B = ramp",
            "rows 8, cols 48, shift 24, stride 0x40000C: A = (j+1)<<20, B = impulse 1024 at 10",
            "rows 72, cols 20, stride 0xC00050: A = ramp, B = ramp"]
    w = words(rom.read(0x01C7F000, 8 * 84 * 4))
    shown = False
    for ex in range(8):
        e = w[84 * ex: 84 * (ex + 1)]
        if e[0] != 0x574E3000 + ex:
            continue
        if not shown:
            print("accelerator layer-job probe (stock A2-Lite op; matvec config 0xAA1 with a time-window matrix), output words as written:")
            shown = True
        print(f"  {ex}: {desc[ex]}")
        print(f"     words changed {e[1]} (first {e[2]}, one past the last {e[3]})")
        vals = [(i, v - (1 << 32) if v >= (1 << 31) else v) for i, v in enumerate(e[4:84])]
        print("     " + "  ".join(f"{i}:{v}" for i, v in vals[:40] if v != 0x7FEEDBAC))
    if not shown:
        print("accelerator layer-job probe: not run")


def show_wntm(rom: Rom):
    w = words(rom.read(0x01C7FB00, 52))
    if w[0] != 0x4D544E57:
        print("accelerator conv-mode job timing: not run")
        return
    names = ["64 x 20  (k=6 convolution, step 80 B, out 12 B)", "64 x 47  (k=15 convolution)", "64 x 3   (1x1 mix)", "64 x 48  (head, sliding, out 4 B)",
             "64 x 256 (sliding FIR)", "64 x 1024 (a 1024-tap FIR block: the cabinet question)"]
    print("accelerator conv-mode job timing (16 jobs each, accelerator otherwise idle):")
    for e in range(6):
        t, mac = w[1 + 2 * e], w[2 + 2 * e]
        us = t / 24.0 / 16
        print(f"  {names[e]:58s} {us:7.2f} us per job, {us * 1000.0 / mac:6.2f} ns per multiply-add ({mac} MACs)")


def show_a81(rom: Rom):
    names = ["V0: src_a = ramp (i+1)*4096, dst before = 1.0 (262144) everywhere",
             "V1: src_a = 1.0 everywhere, dst before = ramp (i+1)*4096",
             "V2: src_a = ramp (i+1)*4096, dst before = 0"]
    w = words(rom.read(0x01C7E500, 3 * 144 * 4))
    any_ = False
    for ex in range(3):
        e = w[144 * ex: 144 * (ex + 1)]
        if e[0] != 0x31384130 + ex:
            continue
        if not any_:
            print("config 0x0A81 probe (shift 18, count 64, in place; Q18: 1.0 = 262144):")
            any_ = True
        sv = lambda v: v - (1 << 32) if v & 0x80000000 else v
        out = e[4:144]
        print(f"  {names[ex]}")
        print(f"    words changed {e[1]}, first {e[2]}, one past the last {e[3]}")
        print("    " + "  ".join(f"{i}:{'.' if out[i] == 0x7FEEDBAC else sv(out[i])}" for i in list(range(0, 10)) + list(range(60, 70))))
    if not any_:
        print("config 0x0A81 probe: not run")


def show_bootmax(rom: Rom):
    w = words(rom.read(0x01C7EC00, 96))
    if w[0] != 0x544F4F42:
        print("maxima: not split into boot and steady state (older build, or less than 8 s of uptime)")
        return
    names = ["DYN", "DRV", "AMP", "CAB", "EQ", "MOD", "DLY", "REV"]
    us = lambda t: t / 24.0
    print("longest times in the first 8 s (the boot phase; the 'max' and 'longest' figures elsewhere in this readout are since 8 s, the steady state):")
    print("  slots: " + "  ".join(f"{n} {us(w[1 + i]):.0f}" for i, n in enumerate(names)) + " us")
    print(f"  core 1's longest job {us(w[9]):.0f} us, core 0's longest wait for it {us(w[10]):.0f} us, the audio interrupt's longest {us(w[11]):.0f} us")
    tn = ["tail", "whole chain call", "before the hand-over", "the hand-over", "after the amp", "the chain", "crossfade", "drums + sum limiter", "USB copy", "ceiling + DC blocker"]
    print("  chain-call parts: " + "  ".join(f"{n} {us(w[12 + i]):.0f}" for i, n in enumerate(tn)) + " us")


def show_gru_rows(rom: Rom):
    w = words(rom.read(0x01C7ED00, 96))
    if w[0] != 0x52555247:
        print("accelerator job size probe: not run")
        return
    print("accelerator matvec jobs by row count (17 columns; each count run on random data against the exact reference; time of 64 jobs):")
    for n in range(5, 16):
        ok, t = w[1 + 2 * (n - 5)], w[2 + 2 * (n - 5)]
        print(f"  {n:2} rows: {'exact' if ok else 'NOT exact'}, {t / 24.0 / 64:.2f} us per job")
    print(f"  largest row count up to which every size from 5 was exact: {w[23]}")
    a = words(rom.read(0x01C7EE00, 256))
    if a[0] == 0x32555441:
        import statistics
        n, r = a[2], a[3]
        print(f"the amp's split tuner ({r} visits per size, {a[4]} blocks each, alternating direction; core 1's time per 64-sample block, us):")
        print("  rows    mean      sd      min      max   (visit means)")
        best = None
        for i in range(n):
            rows = a[8 + n + i]
            v = [a[8 + 2 * n + i * r + k] / 24.0 for k in range(r)]
            m, sd = statistics.mean(v), statistics.stdev(v)
            print(f"  {rows:4}  {m:7.1f}  {sd:6.1f}  {min(v):7.1f}  {max(v):7.1f}   " + " ".join(f"{x:.0f}" for x in v))
        print(f"  chosen: {a[1]} rows ({a[5]} blocks counted)")
    else:
        print("the amp's split tuner: not finished (it starts about 9 s after the amp starts and takes about 8 s)")


def show_wavenet(rom: Rom):
    w = words(rom.read(0x01C7EF00, 128))
    if w[0] != 0x474E4457:
        print("WaveNet engine: never ran (no WaveNet capture was played since boot)")
        return
    print("WaveNet engine (SRAM 0x01C7EF00; block = 64 samples, budget 1451 us):")
    print(f"  blocks {w[3]}, last {w[1] / 24.0:.0f} us, longest {w[2] / 24.0:.0f} us ({w[2] / 24.0 / 64:.1f} us per sample; the sample period is 22.7 us)")
    print(f"  NaN resets {w[4]}, largest head output {w[6] / 1e6:.4f}, largest output {w[5] / 1e6:.4f}")
    print(f"  makeup x{w[7] / 1000.0:.3f}, voicing {w[8]}, amp+cab capture {w[9]}, input trim x{w[10] / 1000.0:.3f}")
    stall, jobs, total = w[11] / 24.0, w[12], w[13] / 24.0
    ptot, tp, tg = w[19] / 24.0, w[17] / 24.0, w[18] / 24.0
    print(f"  engine: {'BLOCK, stock-style pipeline' if w[16] else 'not running'}; last normal block {total:.0f} us in {jobs} accelerator jobs; given up by the guard: {'YES' if w[14] else 'no'} (slow-block count {w[15]}; budget 1451 us, the guard counts blocks over 1458)")
    if w[16] and ptot:
        print(f"  one block in 256 is profiled (one in 32 up to Onyx 1.18) (its timer reads add about 100 us, so its total is {ptot:.0f} us): waiting inside the accelerator driver {stall:.0f} us, post-processing {tp:.0f} us, gather {tg:.0f} us, the rest {ptot - stall - tp - tg:.0f} us")
        print(f"  layer 1 chunk (32 rows) timed in the pipeline: gather {w[20] / 24.0:.1f} us, post-processing {w[22] / 24.0:.1f} us")
        print(f"  matrices and job results in the low SRAM: {'YES' if w[24] else 'no (self-test failed or off)'}")
        print(f"  CPU stage: {'the STOCK-STYLE assembly (self-test passed)' if w[24] & 2 else 'the C version (the assembly self-test FAILED or this is an older build)'}")


def show_build(rom: Rom):
    w = words(rom.read(0x01C7D5B0, 24))
    if w[0] != 0x49444C42:
        print("BUILD: no build id in this firmware (older than the build-id feature)")
        return
    txt = b"".join(x.to_bytes(4, "little") for x in w[2:6]).split(bytes([0]))[0].decode("ascii", "replace")
    print(f"BUILD {w[1]}  (built {txt})")


def show_frames(rom: Rom):
    w = words(rom.read(0x01C7D5A0, 16))
    if w[0] == 0 or w[0] > 0x7FFFFFFF:
        print("screen frames: none counted (build without the frame timer)")
        return
    x = words(rom.read(0x01C7D590, 16))
    if x[2] + x[3] == 20:
        print(f"  the last frame: drawing + comparing {x[0] / 1000:.1f} ms, sending to the panel {x[1] / 1000:.1f} ms; {x[2]} bands sent, {x[3]} unchanged and skipped")
    print(f"screen frames: {w[0]} full redraws; the last took {w[1] / 1000:.1f} ms in {w[3]} band slices, the slowest {w[2] / 1000:.1f} ms "
          f"(about {1000 / max(1.0, w[1] / 1000):.0f} redraws a second at that speed)")


def show_ui_smooth(rom: Rom):
    u = words(rom.read(0x01C7E740, 64))
    if u[0] != 0x53485855 or not u[11]:
        print("UI smoothness: not counted (older build)")
        return
    n = u[11]
    fr = u[1:6]
    gp = u[6:11]
    tot = sum(fr)
    pct = lambda a, t: f"{100.0 * a / t:4.1f}%" if t else "  -  "
    print(f"UI smoothness: {n} ticks of the 60 Hz loop; gaps <=17 ms {pct(gp[0], n)}, <=20 {pct(gp[1], n)}, <=33 {pct(gp[2], n)}, <=50 {pct(gp[3], n)}, over 50 {pct(gp[4], n)} (longest {u[12]} ms)")
    print(f"  {tot} frames drawn; latency from start to last band: <17 ms {pct(fr[0], tot)}, <34 {pct(fr[1], tot)}, <67 {pct(fr[2], tot)}, <100 {pct(fr[3], tot)}, 100+ {pct(fr[4], tot)}")
    if u[13] or u[14]:
        print(f"  display list: {u[13]} frames replayed from it, {u[14]} too big for it (drawn the old way), the most commands any frame asked for {u[15]} (the list holds 1372)")
    bq = words(rom.read(0x01C7E7D8, 4))[0]
    if bq >> 16 == 0x4251:
        print(f"  biquad kernel (fizz filter): {'the assembly (self-test passed)' if bq & 1 else 'the C version (the assembly self-test FAILED)'}")


def show_pool(rom: Rom):
    w = words(rom.read(0x01C7E780, 32))
    if w[0] != 0x4C4F4F50:
        print("effect memory: not counted (older build)")
        return
    print(f"effect memory: {w[1]:,} of 147,456 bytes in use (peak {w[5]:,}), largest free block {w[2]:,}; requests that failed: {w[3]}" + (f" (the last wanted {w[4]:,} bytes)" if w[3] else ""))


def show_text_selftest(rom: Rom):
    w = words(rom.read(0x01C7D440, 4 * (1 + 28)))
    if w[0] != 0x54585354:
        print("text self-test: not present")
        return
    names = ["Onyx font, string from flash", "Onyx font, string copied to RAM", "5x7 font, string from flash", "5x7 font, string copied to RAM"]
    print("text self-test (what the banded drawing makes of \"ABC\" on this unit):")
    for k in range(4):
        print(f"  {names[k]}:")
        for r in range(7):
            v = w[1 + k * 7 + r]
            print("    " + "".join("#" if v >> x & 1 else "." for x in range(20)))


def show_pot(rom: Rom):
    w = words(rom.read(0x01C7D4D0, 16))
    if w[0] != 0x504F5453:
        print("master knob (SARADC channel 2): never read (skipped at power-on, or not yet 4 s up)")
        return
    raw = w[1]
    print(f"master knob (SARADC channel 2): raw {raw if raw != 0xFFFF else 'FAILED'} of 1023" +
          (f" = {100 * raw // 1023} %" if raw != 0xFFFF else "") + f"; ADC marked dead {w[2]}, timeouts {w[3]}")


def show_opcost(rom: Rom):
    w = words(rom.read(0x01C7CB00, 64))
    if w[15] != 0x5343504F:
        print("op cost probe: no data (build before 2026-11-02, or the probe is off)")
        return
    n = w[14] or 2048
    base = w[0]
    names = ["float add (dependent)", "float multiply (dependent)", "float divide (dependent)", "int -> float (+ multiply, add)", "float -> int (+ multiply)",
             "word in a static struct, read-modify-write", "word on the stack, read-modify-write", "fabsf as a library call (+ add)", "__builtin_fabsf (+ add)",
             "sqrtf (library, + multiply)", "64-bit multiply-accumulate", "integer divide", "call of an empty function"]
    print(f"op cost probe (one operation, ns; {n} of each at boot; the empty loop that is subtracted takes {base / 24.0 / n * 1000:.1f} ns a pass):")
    for i, nm in enumerate(names):
        t = w[1 + i]
        print(f"  {nm:46} {max(t - base, 0) / 24.0 / n * 1000:7.1f} ns   (loop total {t / 24.0 / n * 1000:6.1f} ns)")


def show_drum_parts(rom: Rom):
    w = words(rom.read(0x01C7D540, 64))
    if w[0] != 0x544D5244:
        print("drum engine timing: no data (the drums have not run since power-on, or an older build)")
        return
    us = lambda t: t / 24.0
    print(f"drum engine (last / longest, us per call): voices {us(w[1]):.0f} / {us(w[2]):.0f}; bus {us(w[3]):.0f} / {us(w[4]):.0f}; "
          f"whole engine {us(w[5]):.0f} / {us(w[6]):.0f}; guitar + drums sum limiter {us(w[7]):.0f} / {us(w[8]):.0f}")
    print(f"  voices sounding: {w[9]} now, at most {w[10]} at once (ADPCM {w[11]}, mu-law {w[12]}, synthesised {w[13]}); "
          f"{w[14]} of {w[15]} calls had a voice sounding")


def show_isr_parts(rom: Rom):
    w = words(rom.read(0x01C7D4E0, 96))
    print(f"inside the chain call (last / longest, us): before the hand-over (conditioning + slots in front of the amp, on this core) "
          f"{w[4] / 24:.0f} / {w[5] / 24:.0f}; the hand-over itself (waiting for core 1, swap, submit) {w[6] / 24:.0f} / {w[7] / 24:.0f}; "
          f"slots after the amp {w[8] / 24:.0f} / {w[9] / 24:.0f}")
    print(f"after the chain, in the same call (last / longest, us): the chain itself {w[10] / 24:.0f} / {w[11] / 24:.0f}; swap crossfade {w[12] / 24:.0f} / {w[13] / 24:.0f}; "
          f"drums + sum limiter {w[14] / 24:.0f} / {w[15] / 24:.0f}; USB copy {w[16] / 24:.0f} / {w[17] / 24:.0f}; soft ceiling + output DC blocker {w[18] / 24:.0f} / {w[19] / 24:.0f}")
    print(f"audio interrupt, besides the effects (last / longest, us): tail (volume, speaker mode, limiter) {w[0] / 24:.0f} / {w[1] / 24:.0f}; "
          f"whole chain call incl. drums, mix limiter, USB, output filters {w[2] / 24:.0f} / {w[3] / 24:.0f}")


def show_governor(rom: Rom):
    v = words(rom.read(0x01C7D4C0, 4))[0]
    print(f"load governor: level {v & 0xFF} (0 full quality, 1 drive clipper at 2x, 2 also a shorter cabinet); average audio time {v >> 8} us (block = 1451 us)")
    s = words(rom.read(0x01C7D4C4, 12))
    print(f"stereo cabinet: average audio time with it {s[0]} us, without it {s[1]} us (the stereo costs the difference); allowed now: {s[2] & 1}; "
          f"switched off by the governor {s[2] >> 8} time(s)")


def show_crash(rom: Rom):
    w = words(rom.read(0x01C7C300, 140))
    if w[0] != 0x52434150:
        print("crash record (CPU fault): none since the memory was last powered off")
        return
    print(f"crash record (CPU FAULT): {w[1]} fault(s) recorded; the last: PC {w[2]:#010x}, return {w[3]:#010x}, rets {w[5]:#010x}, "
          f"at {w[12] / 1000:.1f} s after boot, DBG_MSG {w[10]:#x} EMU_MSG {w[9]:#x}, user sp {w[8]:#010x}, sp {w[10 - 0]:#x}")
    print("  r0..r15: " + " ".join(f"{v:08x}" for v in w[19:35]))


BLE_STATES = {0: "IDLE (compiled in, not asked to start: hold VOL+ and VOL- at power-on)", 1: "REQUESTED", 2: "NO CAL (no radio calibration in this build: run tools/gen_ble_trims.py and rebuild)",
              3: "STARTING (the radio start-up program: if the unit stays here it HUNG in it)", 4: "UP (radio and baseband up, interrupts masked)",
              5: "ADVERTISING", 6: "CONNECTED", 7: "FAILED"}
BLE_ERRS = {0: "none", 1: "no calibration", 2: "the start-up program hit a bounded wait that expired (BBP / RF-die SPI timeout or a bad op)", 3: "advertising was enabled but no advertising event was counted within 2 s",
            4: "a BLE interrupt took over 300 us", 5: "the hardware random source gave no address",
            6: "INTERRUPT STORM: the BLE interrupts fired 100,000 times a second and the guard masked them (without it the main loop starves and the watchdog resets the unit)"}
BLE_STEPS = {0: "none", 1: "SET_ON", 2: "STACK_INIT", 3: "RF_INIT (the radio start-up program)", 4: "BB_INIT (the baseband window)", 5: "STARTED", 6: "ENABLE (advertising programmed)",
             7: "LINK_STOP", 8: "LINK_OPEN", 9: "ADV_PROG", 10: "ADV_STARTED", 11: "IRQS_ON (BLE interrupts unmasked)", 12: "RUNNING", 13: "SET_OFF"}


def ble_event_text(a, b):
    t = {1: lambda: f"boot decision: {BLE_STATES.get(b, b)}", 2: lambda: f"radio start-up began (TIMER4 {b} ms after power-on)", 3: lambda: f"radio + baseband up in {b} us",
         4: lambda: "advertising enabled, BLE interrupts let go", 5: lambda: f"FIRST ADVERTISING EVENT counted {b} ms after enable: the radio is on the air",
         6: lambda: f"A CENTRAL CONNECTED (connections accepted so far {b})", 7: lambda: "the central left", 8: lambda: f"ERROR: {BLE_ERRS.get(b, b)}",
         9: lambda: f"longest BLE interrupt so far {b} us", 10: lambda: "the central turned the MIDI notifications on", 11: lambda: f"first BLE-MIDI message received (USB-MIDI packet {b:#010x})"}
    return "BLE radio: " + t.get(a, lambda: f"code {a} detail {b:#x}")()


VCO_MODES = {1: 'original 2/50 us (VOL- held)', 2: 'stock-scaled x2 (default)', 3: 'stock-scaled x2 + measured band latched', 4: 'stock-scaled x2, CRYSTAL TRIM -3 (VOL- held)', 5: 'stock-scaled x2, CRYSTAL TRIM +3 (VOL+ held)'}
BLE_ST = ["MAGIC", "STATE", "STEP", "ERR", "START_MS", "START_US", "BUTTONS", "IRQS", "ADV_STARTS", "ADV_EVENTS", "ADV_RX", "SCAN_REQ", "CIND_RX", "CIND_OK", "CIND_REJ",
          "ISR_MAX_US", "CONNECTED", "MIDI_READY", "RF_OPS", "RF_BBP_TO", "RF_SPI_TO", "RF_BADOP", "RF_SECTIONS", "RF_BAND", "POLLS", "MIDI_IN",
          "RST_SRC", "WDT", "ALIVE_MS", "N_EVENT", "N_RX", "STORM_IEN", "STORM_IPND", "STORM_G2PND", "STORM_STAT", "STORM_MS", "WDT_AFTER_FEED", "WDT_REARM", "FEED_US", "FEED_US_MAX", "SUP_R0", "SUP_RES", "SUP_REGS", "VCO_RES", "VCO_DELAYS", "VCO_REF", "VCO_MODE", "VCO_BAND_REG", "VCO_BAND_REG2", "REGS_PTR"] + [f"PLL_T{n}" for n in range(12)] + ["PLL_STAT", "RATE_MAX", "FAST_SECS", "FIRST_FAST_MS", "RATE_RESETS", "LINK_CLOSES", "LINK_SUP_TO", "LINK_ESTAB_FAIL", "LINK_PEER_TERM", "LINK_CLOSE_WHY", "LINK_CLOSE_MS", "LINK_ENC_REQ", "LINK_ENC_ON", "LINK_PDU_N", "LINK_NTF_N", "LINK_WCMD_N", "LINK_CTL_RX", "HIST_N"] + [f"HIST{i}" for i in range(48)] + ["FL_PTR", "PM_DT_US", "PM_DE", "MIDI_OUT", "MIDI_DROP"] + [f"CLS{i}" for i in range(8)] + ["LATE_CHM", "LATE_UPD", "DIAG_PTR", "MIDI_SUBS"]    # include/pa_ble_radio.h, same order


def _print_ble_block(st: dict, indent: str = "  ") -> None:
    state = st["STATE"]
    print(f"{indent}state            : {BLE_STATES.get(state, state)}")
    step = st["STEP"]
    print(f"{indent}stack step       : {BLE_STEPS.get((step >> 16) & 0xFF, (step >> 16) & 0xFF)} (breadcrumb {step:#010x})" + ("" if (step >> 24) == 0xB1 else "  [no breadcrumb written]"))
    print(f"{indent}last error       : {BLE_ERRS.get(st['ERR'], st['ERR'])}")
    if state >= 4:
        print(f"{indent}start-up         : began {st['START_MS']} ms after power-on, took {st['START_US']} us ({st['START_US'] / 1000:.1f} ms)")
        print(f"{indent}start-up program : {st['RF_OPS']} ops run, {st['RF_BBP_TO']} BBP timeouts, {st['RF_SPI_TO']} RF-die SPI timeouts, bad op {st['RF_BADOP']}, "
              f"groups entered {st['RF_SECTIONS']:#x}, VCO band found/band {st['RF_BAND'] >> 8}/{st['RF_BAND'] & 0xFF}")
        print(f"{indent}on the air       : advertising starts {st['ADV_STARTS']}, ADVERTISING EVENTS {st['ADV_EVENTS']}" + ("  <-- the radio is transmitting" if st["ADV_EVENTS"] else "  <-- none counted: not advertising"))
        print(f"{indent}                   packets heard while advertising {st['ADV_RX']} (scan requests {st['SCAN_REQ']}); CONNECT_IND heard {st['CIND_RX']}, accepted {st['CIND_OK']}, rejected {st['CIND_REJ']}")
        print(f"{indent}link             : connected {st['CONNECTED']}, MIDI notifications {'ON' if st['MIDI_READY'] else 'off'}; BLE-MIDI messages received {st['MIDI_IN']}, event packets queued for the central {st['MIDI_OUT']}, dropped {st['MIDI_DROP']}")
        print(f"{indent}late instants    : {st['LATE_CHM']} channel-map and {st['LATE_UPD']} connection-update PDUs arrived after their instant and were applied late (before pack 145 each one closed the link)")
        for _a, _lab in ((0, "this counter"), (4, "counter - 1")):
            _ds, _dh, _rs, _rh = (st[f"CLS{_a + j}"] for j in range(4))
            if _ds + _rs:
                print(f"{indent}hearing by class : [{_lab}] on channels the map uses {_dh}/{_ds} events heard ({100 * _dh / max(_ds, 1):.0f}%); on channels the remap moved {_rh}/{_rs} ({100 * _rh / max(_rs, 1):.0f}%)  <-- a big gap between the two = the remap")
        print(f"{indent}BLE interrupts   : {st['IRQS']} taken, longest {st['ISR_MAX_US']} us")
    print(f"{indent}main-loop polls  : {st['POLLS']} (one per main-loop pass; the poll stops when the unit does)")
    print(f"{indent}why this boot began: reset-source register P33[0x12] = {st['RST_SRC']:#04x}   (compare: a power-on, a watchdog restart and our own download-mode reset each leave a different value)")
    print(f"{indent}watchdog register  : {st['WDT']:#04x} (0x5D = armed, about 8 s)   last sign of life: TIMER4 {st['ALIVE_MS']} ms after power-on")
    print(f"{indent}watchdog just after a feed: {st['WDT_AFTER_FEED']:#04x}  ({'feed bit still set: the feed was NOT taken' if st['WDT_AFTER_FEED'] & 0x40 else 'feed bit cleared: the feed was taken'}); "
          f"{'QUIET MODE: the radio start-up ran, no advertising, BLE interrupts masked (HOME held at power-on)' if st['WDT_REARM'] == 3 else ('TRANSMIT LEVEL 4 (VOL- held at power-on; default and stock = 6)' if st['WDT_REARM'] == 4 else ('TRANSMIT LEVEL 8 (VOL+ held at power-on; default and stock = 6)' if st['WDT_REARM'] == 5 else ('ADVERTISING WITH THE BLE INTERRUPTS MASKED (< held at power-on): the engine transmits, no handler runs' if st['WDT_REARM'] == 6 else ('watchdog DISARMED before the start-up' if st['WDT_REARM'] == 2 else 'watchdog left as the boot armed it'))))}")
    print(f"{indent}one watchdog feed (3 P33 bus transfers) takes {st['FEED_US']} us now, {st['FEED_US_MAX']} us at the longest (normal: a few us; seconds would mean the bus is stalling)")
    r0 = st['SUP_R0']
    if r0 == 0xFFFF: print(f"{indent}supply start-up: SKIPPED (HOME+VOL- at power-on)")
    else:
        g = st['SUP_REGS']
        print(f"{indent}supply start-up: P33 reg0 {r0 >> 8:#04x} -> {r0 & 255:#04x} (bits 3:2 want 0b10 = DC-DC), checks {'ALL GOOD' if st['SUP_RES'] == 0 else 'FAILED mask ' + str(st['SUP_RES'])}; reg5 {g >> 24:#04x} (stock 0x74) reg7 {(g >> 16) & 255:#04x} reg8 {(g >> 8) & 255:#04x} reg2 {g & 255:#04x}")
    d = st['VCO_DELAYS']
    print(f"{indent}VCO search: mode {VCO_MODES.get(st['VCO_MODE'], 'stock-scaled')}, edge {(d >> 16) & 0x7FFF} us, settle {d & 0xFFFF} us, reference reg {st['VCO_REF']:#x}, last comparator word {st['VCO_RES']:#010x} ({'LOW' if st['VCO_RES'] & (1 << 17) else ''}{'HIGH' if st['VCO_RES'] & (1 << 18) else ''}{'' if st['VCO_RES'] & 0x60000 else 'neither low nor high'})")
    print(f"{indent}ADVERTISING RATE: busiest second {st['RATE_MAX']} events (configured about {1600 // 400 if True else 0} per second at 250 ms); {st['FAST_SECS']} too-fast seconds, first at {st['FIRST_FAST_MS']} ms (0 = none); advertising restarted {st['RATE_RESETS']} time(s) by the watchdog")
    cw = st['LINK_CLOSE_WHY']
    ERR = {0x08: 'connection timeout', 0x13: 'remote user terminated', 0x16: 'terminated by the local host', 0x22: 'LL response timeout', 0x28: 'instant passed', 0x3B: 'unacceptable connection parameters', 0x3D: 'MIC failure', 0x3E: 'connection failed to be established', 0x05: 'authentication failure', 0x06: 'PIN or key missing', 0x1A: 'unsupported remote feature'}
    WHO = ['local', 'the peer', 'supervision timeout', 'could not establish', 'procedure timeout', 'protocol error', 'terminate not acknowledged']
    print(f"{indent}LINKS: {st['LINK_CLOSES']} closed ({st['LINK_SUP_TO']} supervision timeouts, {st['LINK_ESTAB_FAIL']} failed to establish, {st['LINK_PEER_TERM']} terminated by the peer); encryption requested {st['LINK_ENC_REQ']}x, links encrypted {st['LINK_ENC_ON']}; PDUs seen {st['LINK_PDU_N']}, link-layer control PDUs {st['LINK_CTL_RX']}, MIDI notifications sent {st['LINK_NTF_N']}, write commands received {st['LINK_WCMD_N']}")
    if st['LINK_CLOSES']:
        print(f"{indent}  LAST CLOSE: reason {cw & 255:#04x} ({ERR.get(cw & 255, 'see the Core spec error codes')}), by {WHO[min((cw >> 8) & 255, 6)]}, at event {cw >> 16}, the connection lasted {st['LINK_CLOSE_MS']} ms")
    tb = bytes(b for n in range(12) for b in st[f'PLL_T{n}'].to_bytes(4, 'little'))
    ps = st['PLL_STAT']
    print(f"{indent}PLL calibration: {'MEASURED (stock algorithm)' if ps & 1 else ('SKIPPED (VOL- held, captured constants)' if ps & 2 else 'not run')}; {ps >> 8 & 255} LUT word-1 entries patched (want 128), {ps >> 16} comparator probes; bank in use {(st['VCO_BAND_REG2'] >> 19) & 0x7F}")
    print(f"{indent}  table (47 points + bound): " + ' '.join(str(b) for b in tb))
    x = st['VCO_BAND_REG2']
    print(f"{indent}VCO bank register 0x11938 = {x:#010x}: bank field {(x >> 19) & 0x7F} (the one in use after the start-up)")
    ev = st['ADV_EVENTS']
    print(f"{indent}HIT RATE: {st['SCAN_REQ']} scan requests, {st['N_RX']} RX interrupts in {ev} advertising events = {1000.0 * st['SCAN_REQ'] / ev if ev else 0:.1f} per 1000 (a phone scanning nearby should answer far more often; run the scanner next to the unit for the whole count)")
    print(f"{indent}interrupts at the vector: {st['N_EVENT']} event (IRQ 45), {st['N_RX']} RX (IRQ 29)")
    if st["STORM_MS"]:
        print(f"{indent}INTERRUPT STORM at {st['STORM_MS']} ms: engine IEN {st['STORM_IEN']:#x}, IPND {st['STORM_IPND']:#x}, G2PND {st['STORM_G2PND']:#x}, STAT {st['STORM_STAT']:#x}")


def show_reset_cause(rom: Rom):
    w = words(rom.read(0x01C7D5C8, 8))
    caller = w[1]
    if (caller >> 24) == 0x02:
        print(f"DOWNLOAD MODE WAS REQUESTED BY the code at {caller:#010x} (the return address of rc_recover_uboot, from the run that ended; if a boot rescue followed, its own call no longer overwrites this; look it up in the build's map: llvm-nm -n build_pi32/pocket_amp_custom.elf)")
    if (w[0] >> 8) != 0x525354:
        print("reset record: none (the last boot did not go to the download mode through the freeze rescue, or a later boot reached the main loop)")
        return
    src = w[0] & 0xFF
    print(f"RESET RECORD: the freeze rescue found the chip's reset-source register P33[0x12] = {src:#04x} when it took the unit to the download mode; "
          f"(see the checkpoints line for where the main loop and the audio interrupt were)")
    print("  (known values so far: 0x01 = a power-on; 0x04 = a boot after our own reset into the download mode. Anything else is the cause of the reset we are hunting.)")


def _print_ble_regs(rom: Rom, ptr: int) -> None:
    """The two register read-back snapshots (ble_radio.c snap_regs): right after the radio start-up, and about 3 s later."""
    w = words(rom.read(ptr, 2 * 72 * 4))
    names = ([f"BT {0x2FC00 + 4 * i:#07x}" for i in range(19)] + ["BT 0x2fc7c", "BT 0x2fc80", "BT 0x2fc84", "BT 0x2fcbc"]
             + [f"BB {0x28000 + 4 * i:#07x}" for i in range(14)] + [f"RF {0x11900 + 4 * i:#07x}" for i in range(32)] + ["-", "-", "-"])
    expect = {"BT 0x2fc00": 0x00FFD144, "BT 0x2fc04": 0x4E143CDF, "BT 0x2fc0c": 0x80808080, "BT 0x2fc10": 0x80008080}   # what the driver writes (fm1_ble.h)
    print("  register read-back (after start-up | 3 s later); a block that is off or loses its settings reads as noise, 0 or all-ones:")
    for i in range(69):
        a, b = w[i], w[72 + i]
        note = ""
        if names[i] in expect:
            note = "  expected " + format(expect[names[i]], "#010x") + (" OK" if a == expect[names[i]] else " DIFFERENT")
        print(f"    {names[i]:12s} {a:#010x} | {b:#010x}{'  (changed)' if a != b else ''}{note}")

sys.path.insert(0, str(Path(__file__).resolve().parent))


def show_blelink(rom: Rom):
    import json
    import ble_link_decode
    p = words(rom.read(0x01C7D5D0, 32))
    if p[0] != 0x50454C42:
        print("blelink: no BLE pointer block (this build has no Bluetooth)")
        return
    lay = json.load(open(Path(__file__).with_name("ble_diag_layout.json")))
    if p[4] != lay["sizeof"]:
        print(f"blelink: the unit's ble_dg is {p[4]} bytes but tools/ble_diag_layout.json says {lay['sizeof']}: run tools/gen_ble_diag_layout.py (the stack settings changed)")
        return
    raw = rom.read(p[2], p[4])
    for line in ble_link_decode.decode(raw, lay):
        print(line)
    g = ble_link_decode.fields(raw, lay)
    st0 = read_ble_status(rom, p)
    if st0.get('PM_DE', 0) >= 8 and g['cind_interval']:
        per = st0['PM_DT_US'] / st0['PM_DE']
        want = g['cind_interval'] * 1250.0
        print(f"  EVENT PERIOD of the last link: {st0['PM_DE']} events in {st0['PM_DT_US'] / 1000:.1f} ms = {per:.1f} us each; the central asked for {want:.0f} us: our clock runs {(per / want - 1) * 1e6:+.0f} ppm against it (the interrupt jitter adds a few hundred ppm over a few seconds; a steady value of several hundred or more over a long link is a real clock error)")
    st = read_ble_status(rom, p)
    if st.get("MAGIC") == 0x31454C42 and st["HIST_N"]:
        n = st["HIST_N"]
        print(f"  CLOSE HISTORY kept by the firmware ({n} closes seen, the last {min(n, 8)} shown, oldest first; the stack clock below is seconds since the radio started):")
        prev_events = 0
        prev = None
        for i in range(max(0, n - 8), n):
            k = (i & 7) * 6
            t, why, dur, iv, wn, irq = (st[f"HIST{k + j}"] for j in range(6))
            ev_irq, rx_irq = irq & 0xFFFF, irq >> 16
            d_ev = d_rx = '?'
            if prev is not None:
                d_ev, d_rx = (ev_irq - prev[0]) & 0xFFFF, (rx_irq - prev[1]) & 0xFFFF
            prev = (ev_irq, rx_irq)
            print(f"    #{i + 1}: at {t / 1000:8.3f} s  reason {why & 255:#04x} ({ble_link_decode.LL_ERR.get(why & 255, '?')}) by {ble_link_decode.CLOSE_BY[min((why >> 8) & 255, 6)]}, at event {why >> 16}, lasted {dur} ms; asked interval {(iv & 0xFFFF) * 1.25:.2f} ms latency {iv >> 16}, window {(wn >> 16) * 1.25:.2f} ms at offset {(wn & 0xFFFF) * 1.25:.2f} ms; event IRQs {d_ev}, RX IRQs {d_rx} during the connection (since the previous close)")


def _print_flash_probe(rom: Rom, ptr: int) -> None:
    """the flash controller probe (src/board/flash_ram.c): it runs after a software reset (start reason 0x04 / 0x40) or with < held at power-on"""
    w = words(rom.read(ptr, 24 * 4))
    if w[0] != 0x52504C46:
        print("  flash probe: not run (it runs after a software reset: start reason 0x04 or 0x40, e.g. right after flashing; or with < held at power-on)")
        return
    RC = {0: "all read", -1: "the cache never went idle (nothing changed)", -2: "continuous-read XIP mode (refused, nothing changed)", -3: "SFC_CON bit 31 never cleared (nothing changed)", -4: "a transfer timed out (restored anyway)"}
    rc = w[1] - (1 << 32) if w[1] & 0x80000000 else w[1]
    STAGES = {1: "entered the RAM routine, waiting for the cache to go idle", 2: "about to switch the flash controller to command mode (nothing changed yet)", 0x14: "XIP is OFF, sending the JEDEC-id command",
              0x15: "id read, reading the status registers", 0x16: "status read, reading flash", 0x17: "first flash read done", 0x18: "second flash read done", 0x19: "third flash read done",
              0x1A: "pins and SPI restored, about to restore the controller", 3: "finished and restored"}
    if rc == -16:
        print(f"  FLASH PROBE: THE PREVIOUS RUN DID NOT FINISH. It stopped at stage {w[11] & 0xFF:#04x}: {STAGES.get(w[11] & 0xFF, '?')}. The unit was restarted (by the watchdog) and the probe was skipped this time so the memory could be read.")
        return
    print(f"  FLASH PROBE (read-only): result {rc} = {RC.get(rc, '?')}; stage {w[11] & 0xFF:#04x} = {STAGES.get(w[11] & 0xFF, '?')} (+0x100 = back in the app); took {w[10]} us in total")
    print(f"    JEDEC id {w[2]:#08x} (want 0x856014: Puya P25Q80, 1 MB), status SR1 {w[3] & 255:#04x} SR2 {w[3] >> 8:#04x} (SR1 bit0 = busy, bit1 = write enable: both should be 0)")
    print(f"    SFC_CON {w[4]:#010x} -> {w[5]:#010x}, SPI0_CON {w[6]:#06x} -> {w[7]:#06x}, IOMAP_CON0 {w[8]:#010x} -> {w[9]:#010x}   (after must equal before)")
    EXP = ["38 71 8f 4a f9 b3 64 ca a2 69 cf 88 93 fe 23 46", "55 aa aa 55 da 2b c0 00 2a 24 71 49 00 00 50 31", "ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff"]
    for k, a in enumerate((0x4000, 0xB5000, 0xB6000)):
        got = b"".join(w[12 + 4 * k + j].to_bytes(4, "little") for j in range(4)).hex(" ")
        print(f"    flash {a:#08x}: {got}  {'= the backup, the read path is right' if got == EXP[k] else 'DIFFERENT from the backup (' + EXP[k] + ')'}")


def read_ble_status(rom: "Rom", p) -> dict:
    """the Bluetooth status block, read with the number of words the FIRMWARE says it has (pointer block word 6, bits 15:8; 0 = an older firmware that does not say).
    A reader that assumed its own count read the memory behind a shorter block and printed another structure's words as counters (the 'late instants' of the pack 148 readout
    were the first register snapshot words)."""
    n_fw = (p[6] >> 8) & 0xFF
    n = n_fw or len(BLE_ST)
    raw = words(rom.read(p[1], n * 4))
    st = dict(zip(BLE_ST, raw))
    if n_fw and n_fw < len(BLE_ST):
        print(f"  NOTE: this firmware's status block has {n_fw} words, this tool knows {len(BLE_ST)}: the newer counters ({', '.join(BLE_ST[n_fw:])}) are not in the firmware and show 0 (the unit runs an older build than this tool)")
    elif n_fw > len(BLE_ST):
        print(f"  NOTE: this firmware's status block has {n_fw} words, this tool knows {len(BLE_ST)}: update the tool (BLE_ST) to see the newer ones")
    elif not n_fw:
        print("  NOTE: the firmware does not publish its status block size (built before pack 149): counters beyond MIDI_DROP may belong to another structure and are not shown")
        for k in BLE_ST[BLE_ST.index("MIDI_DROP") + 1:]:
            st[k] = 0
    for k in BLE_ST:
        st.setdefault(k, 0)
    return st

def show_ble(rom: Rom):
    p = words(rom.read(0x01C7D5D0, 32))
    if p[0] != 0x50454C42:
        print("Bluetooth radio: not in this build (no 'BLEP' pointer block)")
        return
    st = read_ble_status(rom, p)
    print(f"Bluetooth radio (status block {p[1]:#x}, stack counters {p[2]:#x}, breadcrumb {p[3]:#x}; calibration compiled in: {'YES' if p[6] & 1 else 'NO'}; boot buttons {p[7]:#x}):")
    if st["MAGIC"] != 0x31454C42:
        print("  status block does not carry its magic: RAM was overwritten or this is a different build")
        return
    _print_ble_block(st)
    if st.get('FL_PTR', 0) > 0x1000:
        _print_flash_probe(rom, st['FL_PTR'])
    if st['REGS_PTR'] > 0x1000:
        _print_ble_regs(rom, st['REGS_PTR'])
    bc = words(rom.read(p[3], 48))
    print(f"  stack breadcrumb : now {bc[0]:#010x}, interrupts {bc[1]}, last register access {bc[6]:#x} (op {bc[4]}, write {bc[5]}); the boot before: {bc[2]:#010x}, interrupts {bc[3]}, last access {bc[9]:#x}")
    if p[5] > 0x1000:                                   # the previous run's block (a reset keeps it; a power cycle does not)
        pv = words(rom.read(p[5], (len(BLE_ST) + 1) * 4))
        if pv[len(BLE_ST)] == 0x56455250 and pv[0] == 0x31454C42:
            print("  THE RUN BEFORE THIS RESET (what it had reached when it ended):")
            _print_ble_block(dict(zip(BLE_ST, pv)), "    ")
        else:
            print("  the run before: no record (a power cycle, or the radio was not running)")


def show_stack(rom: Rom):
    lo, top = 0x01C76D00, 0x01C7A000       # the user stack is _ustack_lo..0x01C7A000 (app.ld). Below it: BLERAM 0x01C74100..0x01C764FF (the radio's state) and FLASHFN 0x01C76500..0x01C768FF (the flash driver's RAM code). The scan used to start at 0x01C75E00, inside those two, which are never the paint pattern: it always read "deepest use = all of it, NEARLY FULL" (the real deepest use on pack 160: 5,496 of 14,080 bytes)
    mem = rom.read(lo, top - lo)
    n = (top - lo) // 4
    ws = words(mem)
    first = next((i for i in range(n) if ws[i] != 0xA5A5A5A5), n)       # lowest address that is not the pattern = deepest use
    deepest = (top - (lo + 4 * first))
    print(f"user stack (main loop + audio interrupt): deepest use {deepest} of {top - lo} bytes (lowest touched address {lo + 4 * first:#x}; "
          f"BLERAM starts at 0x01C74100)" + ("  <-- NEARLY FULL" if deepest > (top - lo) - 1024 else ""))


def show_cp(rom: Rom):
    w = words(rom.read(0x01C7C3F0, 16))
    print(f"checkpoints: rescue mark {'SET (this boot ended through the one-shot rescue)' if w[0] == 0x52455343 else 'clear'}; "
          f"main loop last at {w[1]:#x}{'' if w[1] < 0x100 else ' (garbage: no marker written)'}; audio interrupt last step {w[2]:#x} (5 = it finished)")
    print("  main-loop markers: 0x10 loop head, 0x12 before the UI tick, 0x20/0x21 around the battery poll, 0x22 watchdog fed, 0x50-0x52 audio-core tick, 0x71-0x75 inside the SARADC reader")


def show_tsta(rom: Rom):
    w = words(rom.read(0x01C7C390, 48))
    if w[0] != 0x41545354:
        print("test state (TSTA): not present")
        return
    view, preset, mvol, mode = w[3] & 0xFF, (w[3] >> 8) & 0xFF, (w[3] >> 16) & 0xFF, (w[3] >> 24) & 0xFF
    inp, outp = (w[5] >> 16) & 0xFF, (w[5] >> 24) & 0xFF
    print(f"what the unit was doing (TSTA): up {w[2]} s, main-loop passes {w[1]}, preset {preset + 1}, master volume {mvol}, view {view}")
    print(f"  input peak {inp}%, OUTPUT peak {outp}%, audio interrupts run {w[9]}, bypass {'ON (dry pass-through)' if w[10] & 0xFF else 'off'}, "
          f"accelerator broken {(w[10] >> 8) & 0xFF}, auto-bypass {(w[10] >> 16) & 0xFF}")


def show_events(rom: Rom, compact: bool = False):
    w = words(rom.read(0x01C7D600, 16 + 64 * 16))
    if w[0] != 0x474C5645:
        print("event log: not present (build without it)")
        return
    total, cap = w[1], w[2] or 64
    n = min(total, cap)
    first = total - n
    btn = ["HOME", "VOL-", "VOL+", "<", ">", "PARA"]
    slot = ["DYN", "DRV", "AMP", "CAB", "EQ", "MOD", "DLY", "REV"]
    mask = lambda m: "+".join(btn[i] for i in range(6) if m >> i & 1) or "none"
    sl = lambda v: "preset" if v == 0xFF else (slot[v] if v < 8 else str(v))
    def text(code, a, b):
        return {
            1: lambda: f"buttons now {mask(a)} (before: {mask(b)})",
            2: lambda: f"PARA encoder {a - (1 << 32) if a & 0x80000000 else a:+d} steps",
            3: lambda: f"screen (view) {b} -> {a}",
            4: lambda: f"preset {b + 1} -> {a + 1}",
            5: lambda: f"{sl(a)} model on screen -> index {b}",
            6: lambda: f"SLOW UI tick: {a / 1000:.1f} ms, last main-loop marker {b:#x}",
            7: lambda: f"swap requested: {sl(a)} -> {b}" + (f" ({'a NAM capture, number ' + str(b - 20) if b >= 20 else 'a stock amp'})" if a == 2 else ""),
            8: lambda: f"swap applied by the audio thread in {a / 1000:.2f} ms ({sl(b)})",
            9: lambda: f"audio block took {a} us (the log fires above 1350 us; the real limit is 1451 us){' - OVER THE LIMIT' if a > 1451 else ''}",
            10: lambda: f"AUTO-BYPASS: 8 over-budget blocks in a row (last {a} us)",
            19: lambda: f"NAM CAPTURE {b} DID NOT LOAD (reason {a}: 1 slot empty, 2 flash read failed, 3 the WaveNet was rejected, 4 bad size or checksum): the stock amp 0 was loaded instead",
            18: lambda: f"EFFECT MEMORY FULL: a request for {a} bytes failed (the largest free block is {b} bytes): the effect or model that asked stays off",
            11: lambda: f"second core status {b} -> {a} (0 running, 3 start failed, 4 timeout, 5 bad job, 6 stack)",
            12: lambda: f"event log started (boot buttons {mask(a)})",
            13: lambda: f"RUNAWAY SIGNAL after the amp ({a / 100:.0f}x full scale) made by slot {slot[b] if b < 8 else '?'}: effects after the amp cleared, block silenced",
            14: lambda: "AMP STATE RESET (the GRU state left [-1, 1]: NaN or corrupt weights), block silenced",
            17: lambda: f"FREEZE: {'the MAIN LOOP stopped' if a == 1 else 'the AUDIO INTERRUPT stopped'} (main-loop passes {b}); the unit sent itself to the download mode",
            16: lambda: f"SLOW AMP BLOCK: {a} us (limit for core 1 is about 1450); input peak {b >> 16}% of full scale, output peak {b & 0x7FFF}%; the screen's SPI transfer was {'RUNNING' if b & 0x8000 else 'idle'}",
            20: lambda: ble_event_text(a, b),
            15: lambda: f"LOAD GOVERNOR: quality level {a} (0 full, 1 drive clipper at 2x, 2 also a shorter cabinet), average audio time {b} us",
        }.get(code, lambda: f"code {code} a={a:#x} b={b:#x}")()
    if compact:
        # the buttons, the encoder and the screen changes are noise; the over-limit blocks are summarised, the rest keeps its time
        rows, blocks, slow_amp = [], [], []
        t_prev, t_acc = None, 0
        for k in range(n):
            o = 4 + ((first + k) % cap) * 4
            t, code, a, b = w[o:o + 4]
            if t_prev is not None:
                t_acc += ((t - t_prev) & 0xFFFFFFFF) / 24000.0
            t_prev = t
            if code in (1, 2, 3, 12) or (code in (5, 7) and (code == 5 or a != 2)):     # swap requests of the AMP slot stay: they say which model (>= 20: a NAM capture) was asked for
                continue
            if code == 9:
                blocks.append(a)
            elif code == 16:
                slow_amp.append(a)
            else:
                rows.append((t_acc, text(code, a, b)))
        print(f"events (last {n} of {total} since boot; buttons and screen changes hidden; --full shows all); longest UI tick {w[3] / 1000:.1f} ms:")
        if blocks:
            over = sum(1 for x in blocks if x > 1451)
            print(f"  {len(blocks)} audio blocks above 1350 us: {over} over the 1451 us limit, range {min(blocks)}-{max(blocks)} us, mean {sum(blocks) // len(blocks)} us")
        if slow_amp:
            print(f"  {len(slow_amp)} slow amp blocks (core 1 over 1450 us): {min(slow_amp)}-{max(slow_amp)} us")
        for t_, tx in rows[-12:]:
            print(f"  {t_:9.1f} ms  {tx}")
        if not (blocks or slow_amp or rows):
            print("  nothing notable")
        return
    print(f"event log (SRAM 0x01C7D600, newest last; {total} events since boot, showing {n}; times are ms since the first shown); longest UI tick since boot {w[3] / 1000:.1f} ms:")
    t_prev, t_acc = None, 0
    for k in range(n):
        o = 4 + ((first + k) % cap) * 4
        t, code, a, b = w[o:o + 4]
        if t_prev is not None:
            t_acc += ((t - t_prev) & 0xFFFFFFFF) / 24000.0
        t_prev = t
        print(f"  {t_acc:9.1f} ms  {text(code, a, b)}")


def show_stats(rom: Rom):
    w = words(rom.read(0x01C7C250, 24))
    print("audio interrupt stats (SRAM 0x01C7C250):")
    print(f"  entries {w[0]}, last {w[1] // 24} us, max {w[2] // 24} us (budget 1451 us), overruns {w[3]}, "
          f"auto-bypass {w[4]}, magic {w[5]:#x}")



def show_bleev(rom: Rom):
    import ble_diag_decode
    p = words(rom.read(0x01C7D5D0, 32))
    if p[0] != 0x50454C42:
        print("Bluetooth radio: not in this build (no 'BLEP' pointer block)")
        return
    st = read_ble_status(rom, p)
    ptr = st.get("DIAG_PTR", 0)
    if ptr < 0x1000:
        print("this firmware has no link instrumentation block (built before pack 149)")
        return
    raw = bytearray()
    for off in range(0, ble_diag_decode.SIZE, 1024):
        raw += rom.read(ptr + off, min(1024, ble_diag_decode.SIZE - off))
    print(f"Bluetooth link instrumentation (block at {ptr:#x}); MIDI notifications turned on {st['MIDI_SUBS']} time(s), late channel maps {st['LATE_CHM']}, late connection updates {st['LATE_UPD']}")
    print(ble_diag_decode.report(ble_diag_decode.decode(bytes(raw))))

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--jl-tool", default=os.environ.get("JL_UBOOT_TOOL") or
                    str(Path.home() / "Downloads" / "jl-uboot-tool-main" / "jl-uboot-tool-main"))
    ap.add_argument("--device", help='download-mode disk, e.g. "\\\\.\\PHYSICALDRIVE2" (default: ask jldevfind)')
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("usb", help="the USB driver's debug words")
    fp = sub.add_parser("flashprobe", help="decode the flash controller probe block (flash_ram.c) at an address: the flash test image leaves it in RAM and goes to download mode")
    fp.add_argument("addr", help="the address of pa_flprobe in that build (llvm-nm build_pi32/pocket_amp_custom.elf | findstr pa_flprobe)")
    sub.add_parser("blelink", help="the BLE stack's own diagnostics decoded on the host: the last CONNECT_IND, the last close and why, and a timeline of events and ATT / SMP / link-layer PDUs")
    sub.add_parser("bleev", help="the Bluetooth link instrumentation: the last 256 connection events, per-channel heard/seen tables, interrupt durations, the experiment profile (pack 149 on)")
    sub.add_parser("ble", help="the Bluetooth radio: state, start-up result, advertising counters, errors (the build with PA_BLE)")
    sub.add_parser("stats", help="the audio interrupt statistics")
    sl = sub.add_parser("slots", help="the unit's state: timing per slot, the WaveNet engine, the second core, levels (compact; --full adds every experiment and self-test)")
    sl.add_argument("--full", action="store_true", help="every section: probes, benchmarks, font and clock dumps, the old experiments")
    sub.add_parser("battery", help="raw SARADC channels, supply and battery millivolts")
    sub.add_parser("fir", help="the cabinet FIR accelerator experiment: all output words")
    b = sub.add_parser("bb", help="the black box + crash record + debug words (0x01C7C000, 1 KiB) to a file")
    b.add_argument("--out", default="blackbox.bin")
    fpp = sub.add_parser("flash_probe", help="the PA_FLASH_PROBE survey (read-only peripheral dump + XIP check), see FLASH-ACCESS.md")
    fpp.add_argument("--pack", help="the app-area pack that is on the unit (app_area_packNN.bin): checks the XIP base against its unpacked head")
    r = sub.add_parser("raw", help="hex dump of SRAM")
    r.add_argument("addr", type=lambda s: int(s, 0))
    r.add_argument("length", type=lambda s: int(s, 0))
    a = ap.parse_args()

    if os.name == "nt":
        import ctypes
        if not ctypes.windll.shell32.IsUserAnAdmin():
            print("[!] WARNING: not running as Administrator: raw SCSI access may hang.", file=sys.stderr)
    rom = Rom(Path(a.jl_tool), a.device)
    try:
        if a.cmd == "flash_probe":
            show_flash_probe(rom, a.pack)
        elif a.cmd == "flashprobe":
            _print_flash_probe(rom, int(a.addr, 0))
        elif a.cmd == "blelink":
            show_build(rom)
            show_blelink(rom)
        elif a.cmd == "bleev":
            show_build(rom)
            show_bleev(rom)
        elif a.cmd == "ble":
            show_build(rom)
            show_reset_cause(rom)
            show_ble(rom)
            show_events(rom, compact=True)
        elif a.cmd == "usb":
            show_usb(rom)
            show_usb_tx(rom)
        elif a.cmd == "slots":
            full = a.full
            show_build(rom)
            show_tsta(rom)
            show_crash(rom)
            show_reset_cause(rom)
            show_ble(rom)
            if full:
                show_stack(rom)
            show_events(rom, compact=not full)
            if full:
                show_xip(rom)
                show_frames(rom)
            show_governor(rom)
            show_isr_parts(rom)
            show_drum_parts(rom)
            if full:
                show_opcost(rom)
            show_ui_smooth(rom)
            show_pool(rom)
            if full:
                show_pot(rom)
                show_text_selftest(rom)
            show_levels(rom)
            if full:
                show_cp(rom)
                show_boot(rom)
                show_battery(rom)
                show_fir(rom)
                show_firbench(rom)
                show_a81(rom)
                show_wnop(rom)
                show_wntm(rom)
                show_ram(rom)
                show_clock(rom)
                show_pll(rom)
                show_clock_dump(rom)
                show_contention(rom)
                show_mem(rom)
            show_accel(rom)
            show_slots(rom, compact=not full)
            if full:
                show_gru_rows(rom)
            show_wavenet(rom)
            if full:
                show_bootmax(rom)
            show_smp(rom)
            if full:
                show_gru(rom)
                show_decode(rom)
                show_cpu1(rom)
                show_c1test(rom)
                show_c1diag(rom)
                show_identity(rom)
            show_stats(rom)
        elif a.cmd == "fir":
            show_build(rom)
            show_fir(rom, full=True)
            show_firbench(rom)
            show_a81(rom)
            show_wnop(rom)
            show_wntm(rom)
        elif a.cmd == "battery":
            show_build(rom)
            show_battery(rom)
        elif a.cmd == "stats":
            show_build(rom)
            show_stats(rom)
        elif a.cmd == "bb":
            raw = rom.read(0x01C7C000, 0x400)
            Path(a.out).write_bytes(raw)
            print(f"saved {len(raw)} bytes of SRAM 0x01C7C000..0x01C7C3FF to {a.out} "
                  "(decode with the same layout as pa_transporter.py blackbox)")
            show_usb(rom)
            show_stats(rom)
        else:
            raw = rom.read(a.addr, a.length)
            for off in range(0, len(raw), 16):
                chunk = raw[off:off + 16]
                print(f"{a.addr + off:#010x}  {chunk.hex(' ')}")
    finally:
        rom.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
