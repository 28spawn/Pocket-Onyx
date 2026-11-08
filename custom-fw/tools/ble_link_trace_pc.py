#!/usr/bin/env python3
"""
custom-fw/tools/ble_link_trace_pc.py - a live, state-by-state trace of the unit's Bluetooth link, and a gated one-central test run.

Nothing is written to the unit and nothing is flashed: it only READS the unit's RAM over the USB-MIDI diagnostic channel every ~0.25 s (link layer state, counters, the per-event
ring) and, in --attempts mode, drives ONE central (this PC, through bleak) one attempt at a time. An attempt starts only when the previous link has really closed and the unit is
advertising again (the previous runs piled attempts on top of lingering links).

What it records, per attempt, with times relative to the attempt start:
  scan_found       the scanner sees the advertisement
  connect_ind      the unit ACCEPTED a CONNECT_IND (link layer connection CREATED), with the initiator address and type
  first_data       the first data-channel packet was taken from the radio (connection ESTABLISHED, Core Vol 6 Part B 4.5.2: the first packet in a connection event)
  gatt_connected   bleak/WinRT reports the GATT connection (service discovery done on the PC side)
  notify_on        the unit shows the MIDI notifications on (CCCD written)
  midi_reply       the identity reply arrived over BLE-MIDI
  our_disconnect   the PC closed its side
  link_closed      the unit's link layer closed (reason code, who: peer / supervision / establish ...)
  adv_restart      advertising restarted (ble_hw_adv_start count)   adv_event: the first advertising event after it   rescan_found: the scanner sees the unit again
and per connection the reception metrics, with the definitions below. A connection that starts when no attempt is connecting is reported as FOREIGN, with its initiator address.

Reception metric definitions (src/third_party optimist_ble/src/ble_hw_wl82.c, hw_event_service):
  event        one entry of the unit's ring: the engine's connection event counter advanced and the event interrupt ran (the denominator). Counter values the ring never saw are
               counted separately as 'unserviced'.
  heard        a packet was taken from an RX buffer in that event (access address matched, ANY crc). valid = heard and not crc_bad. It is not 'valid LL PDUs' (empty PDUs count).
  latency      the CONNECT_IND latency is 0 and the unit programs the engine to listen at every event, so a central that skips events (peripheral latency) would show as misses;
               the connection parameters are printed so this can be checked.
  max_run_miss the longest run of consecutive events without a valid packet.  max_gap_ms: the longest time between two valid packets (event distance x interval x 1.25 ms).

    python custom-fw/tools/ble_link_trace_pc.py --watch 60            # just trace the unit for 60 s
    python custom-fw/tools/ble_link_trace_pc.py --attempts 3          # gated baseline with this PC as the one known central
Needs: pip install bleak python-rtmidi. Writes qa/ble-gate/linktrace_<time>.json.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import struct
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ble_matrix_pc as m  # noqa: E402

BLL = 0x01C74B68          # the link layer's state struct (static 'bll' in ble_ll.c); field offsets from the compiled struct (2026-11-03, pack 151)
O_STATE, O_INTERVAL, O_ESTABLISHED, O_PEER = 0, 90, 97, 130
DG = 0x01C08008           # ble_dg (the stack's own counters)
LL_STATES = {0: "off", 1: "advertising", 2: "connected", 3: "scanning", 4: "initiating"}
WHO = ["local", "peer", "supervision timeout", "could not establish", "procedure timeout", "protocol error", "terminate not acknowledged"]
RING_OFF, RING_N = 408, 256


class LockedUnit(m.Unit):
    """the same Unit, with one lock around every request so a monitor thread and the test driver can share it"""
    def __init__(self, port):
        self._lk = threading.RLock()
        super().__init__(port)

    def _call(self, inner, rep, timeout=1.5):
        with self._lk:
            return super()._call(inner, rep, timeout)


def mac(b: bytes) -> str:
    return ":".join(f"{x:02X}" for x in reversed(b))


class Monitor(threading.Thread):
    def __init__(self, unit: LockedUnit, period: float = 0.25):
        super().__init__(daemon=True)
        self.u, self.period, self.stop_flag = unit, period, False
        self.t0 = time.time()
        self.events: list[dict] = []
        self.snap: dict = {}
        self.conns: dict[int, dict] = {}            # by accepted-connection count
        self.lock = threading.Lock()
        self.polls = 0
        self.errors = 0
        p = struct.unpack("<8I", unit.ram(m.PTR_ADDR, 32))
        self.st = p[1]
        self.diag = struct.unpack("<I", unit.ram(self.st + 4 * m.status_index("DIAG_PTR"), 4))[0]
        self.ix = {n: m.status_index(n) for n in ("CONNECTED", "MIDI_READY", "LINK_CLOSES", "LINK_SUP_TO", "LINK_ESTAB_FAIL", "LINK_PEER_TERM", "LINK_CLOSE_WHY")}
        assert self.ix["LINK_CLOSE_WHY"] - self.ix["LINK_CLOSES"] == 4, "status words not contiguous: update this tool"
        self.last_wr = struct.unpack("<I", unit.ram(self.diag + 12, 4))[0]
        self.prev: dict = {}
        self.first: dict = {}
        self.last: dict = {}
        self.cur_conn = 0

    def log(self, kind: str, **kw):
        e = dict(t=round(time.time() - self.t0, 3), kind=kind, **kw)
        with self.lock:
            self.events.append(e)

    def sample(self) -> dict:
        u = self.u
        bl = u.ram(BLL + O_STATE, 1)[0]
        c = u.ram(BLL + O_INTERVAL, 16)
        interval, timeout, last_evt = struct.unpack_from("<HHH", c, 0)
        est = c[7]
        win_off = struct.unpack_from("<H", c, 14)[0]
        d = u.ram(BLL + O_PEER, 8)
        adv_starts, adv_events, adv_rx, scan_req = struct.unpack("<4I", u.ram(DG + 4, 16))
        cind_rx, cind_ok, cind_rej = struct.unpack("<3I", u.ram(DG + 36, 12))
        evt_irqs, rx_irqs, conn_events, c3_zero, evt_same, rx_good, rx_crc_bad, rx_repeat, rx_empty, rx_nothing, rx_desync = struct.unpack("<11I", u.ram(DG + 108, 44))
        cc = struct.unpack("<2I", u.ram(self.st + 4 * self.ix["CONNECTED"], 8))
        lk = struct.unpack("<5I", u.ram(self.st + 4 * self.ix["LINK_CLOSES"], 20))
        return dict(state=bl, interval=interval, timeout=timeout, last_evt=last_evt, est=est, win_off=win_off, peer=mac(d[0:6]), peer_rand=d[6],
                    adv_starts=adv_starts, adv_events=adv_events, adv_rx=adv_rx, scan_req=scan_req, cind_rx=cind_rx, cind_ok=cind_ok, cind_rej=cind_rej,
                    conn_events=conn_events, rx_good=rx_good, rx_crc_bad=rx_crc_bad, rx_repeat=rx_repeat, rx_empty=rx_empty, rx_nothing=rx_nothing, rx_irqs=rx_irqs,
                    connected=cc[0], midi_ready=cc[1], closes=lk[0], sup_to=lk[1], estab_fail=lk[2], peer_term=lk[3], close_why=lk[4])

    def cind_details(self) -> dict:
        """what the stack kept of the CONNECT_IND it last accepted (ble_dg.cind_*: header, window, hop, SCA, the INITIAL channel map, access address, CRC init, timing)"""
        a = self.u.ram(DG + 48, 16)
        b = self.u.ram(DG + 64, 16)
        hdr, win_size, hop, sca = a[1], a[2], a[3], a[4]
        chm = int.from_bytes(a[5:10], "little")
        win_off, interval, latency, timeout = struct.unpack_from("<4H", b, 4)
        aa = int.from_bytes(a[12:16], "little")     # cind_aa at DG+60
        return dict(aa="%#010x" % aa, hdr="%#04x" % hdr, chsel=(hdr >> 5) & 1, txadd=(hdr >> 6) & 1, win_size=win_size, hop=hop, sca=sca, initial_map="%#012x" % chm, initial_channels=bin(chm).count("1"),
                    init_interval_ms=interval * 1.25, init_win_offset_ms=win_off * 1.25, init_latency=latency, init_timeout_ms=timeout * 10)

    def read_ring(self) -> list[dict]:
        u = self.u
        wr = struct.unpack("<I", u.ram(self.diag + 12, 4))[0]
        n = (wr - self.last_wr) & 0xFFFFFFFF
        if n == 0:
            return []
        lost = max(0, n - RING_N)
        first = self.last_wr + lost
        out = []
        i = first
        while i < wr:
            idx = i % RING_N
            run = min(wr - i, RING_N - idx)
            raw = u.ram(self.diag + RING_OFF + 8 * idx, 8 * run)
            for k in range(run):
                evt, flags, ch, dt, ch_alt, used = struct.unpack_from("<HBBhBB", raw, 8 * k)
                out.append(dict(evt=evt, heard=bool(flags & 1), crc=bool(flags & 2), zoom=(flags >> 4) & 3, ch=ch, ch_alt=ch_alt, dt=dt))
            i += run
        self.last_wr = wr
        if lost:
            self.log("ring_overrun", lost=lost)
        return out

    def run(self):
        while not self.stop_flag:
            t = time.time()
            try:
                s = self.sample()
                ring = self.read_ring()
            except Exception as e:  # noqa: BLE001
                self.errors += 1
                self.log("read_error", error=str(e)[:80])
                time.sleep(0.5)
                continue
            self.polls += 1
            p = self.prev
            if not self.first:
                self.first = dict(s)
            self.last = dict(s)
            if p:
                if s["closes"] != p["closes"]:               # a close seen in the same poll as the next CONNECT_IND belongs to the PREVIOUS connection: handle it first
                    why = s["close_why"]
                    by = (why >> 8) & 0xFF
                    cl = dict(reason=why & 0xFF, by=WHO[by] if by < len(WHO) else by, evt=why >> 16, closes_seen=s["closes"] - p["closes"])
                    try:    # the stack's own record of that close: ms since the last RX and since the CONNECT_IND (ble_dg close_since_rx_us / close_since_start_us, DG+228 / +232)
                        _, _, _, since_rx, since_start, _ = struct.unpack("<BBHIII", self.u.ram(DG + 224, 16))
                        cl["since_start_ms"] = round(since_start / 1e3, 1)
                        cl["since_rx_ms"] = round(since_rx / 1e3, 1)
                    except Exception:
                        pass
                    self.log("link_closed", n=self.cur_conn, **cl)
                    cn0 = self.conns.get(self.cur_conn)
                    if cn0 and cn0["end"] is None:
                        cn0["end"] = round(time.time() - self.t0, 3)
                        cn0["close"] = cl
                if s["cind_ok"] != p["cind_ok"]:
                    self.cur_conn = s["cind_ok"]
                    self.conns[self.cur_conn] = dict(id=self.cur_conn, start=round(time.time() - self.t0, 3), peer=s["peer"], peer_rand=s["peer_rand"], interval=s["interval"],
                                                     timeout=s["timeout"], win_off=s["win_off"], first_data=None, end=None, close=None, ring=[], midi_on=None)
                    ci = self.cind_details()
                    self.conns[self.cur_conn].update(ci)
                    self.log("connect_ind", n=s["cind_ok"], peer=s["peer"], addr_type="random" if s["peer_rand"] else "public", interval_ms=s["interval"] * 1.25, timeout_ms=s["timeout"] * 10,
                             win_offset_ms=s["win_off"] * 1.25, **{k: ci[k] for k in ("aa", "hdr", "chsel", "hop", "sca", "win_size", "initial_map", "initial_channels")})
                if s["state"] == 2 and p["state"] == 2 and s["interval"] != p["interval"]:     # a connection update took effect: the interval in force changed
                    self.log("interval_changed", n=self.cur_conn, from_ms=p["interval"] * 1.25, to_ms=s["interval"] * 1.25, timeout_ms=s["timeout"] * 10)
                if s["cind_rej"] != p["cind_rej"]:
                    self.log("connect_ind_rejected", n=s["cind_rej"] - p["cind_rej"])
                if s["state"] != p["state"]:
                    self.log("ll_state", state=LL_STATES.get(s["state"], s["state"]))
                cn = self.conns.get(self.cur_conn)
                if cn and cn["first_data"] is None and (s["est"] or s["rx_good"] != p["rx_good"] or s["rx_crc_bad"] != p["rx_crc_bad"]) and s["state"] == 2:
                    cn["first_data"] = round(time.time() - self.t0, 3)
                    self.log("first_data", n=self.cur_conn, established=bool(s["est"]))
                if s["midi_ready"] != p["midi_ready"]:
                    self.log("midi_notifications", on=bool(s["midi_ready"]))
                    if cn and s["midi_ready"] and cn["midi_on"] is None:
                        cn["midi_on"] = round(time.time() - self.t0, 3)
                if s["adv_starts"] != p["adv_starts"]:
                    self.log("adv_restart", count=s["adv_starts"])
                if s["adv_events"] != p["adv_events"] and p["adv_events"] == self.adv_ev_mark():
                    self.log("adv_event", count=s["adv_events"])
            if ring:
                cn = self.conns.get(self.cur_conn)
                if cn is not None:
                    cn["ring"] += ring
            with self.lock:
                self.snap = dict(s, t=time.time())
            self.prev = s
            dt = time.time() - t
            time.sleep(max(0.0, self.period - dt))

    def adv_ev_mark(self):                           # log only the first advertising event after each restart
        last_restart = max((e["t"] for e in self.events if e["kind"] == "adv_restart"), default=-1)
        last_adv_ev = max((e["t"] for e in self.events if e["kind"] == "adv_event"), default=-2)
        return self.prev["adv_events"] if last_restart > last_adv_ev else -1

    def get(self) -> dict:
        with self.lock:
            return dict(self.snap)


def reception(cn: dict) -> dict:
    """the reception metrics of one connection from its ring entries (see the module header for the definitions)"""
    ring = cn.get("ring") or []
    if not ring:
        return dict(events=0, heard=0, heard_pct=0.0, crc_bad=0, valid=0, valid_pct=0.0, unserviced=0, max_run_miss=0, max_gap_ms=0.0, per_channel={}, interval_ms=cn["interval"] * 1.25)
    ivl = cn["interval"] * 1.25
    events = len(ring)
    heard = sum(1 for e in ring if e["heard"])
    crc = sum(1 for e in ring if e["crc"])
    valid = [e for e in ring if e["heard"] and not e["crc"]]
    run = best = 0
    unserviced = 0
    prev = None
    for e in ring:
        if prev is not None:
            unserviced += max(0, ((e["evt"] - prev) & 0xFFFF) - 1) if ((e["evt"] - prev) & 0xFFFF) < 0x8000 else 0
        prev = e["evt"]
        if e["heard"] and not e["crc"]:
            run = 0
        else:
            run += 1
            best = max(best, run)
    gap = 0.0
    last = None
    for e in valid:
        if last is not None:
            d = (e["evt"] - last) & 0xFFFF
            if d < 0x8000:
                gap = max(gap, d * ivl)
        last = e["evt"]
    ch = {}
    for e in ring:
        c = ch.setdefault(e["ch"], [0, 0])
        c[0] += 1
        c[1] += 1 if (e["heard"] and not e["crc"]) else 0
    return dict(events=events, heard=heard, heard_pct=round(100.0 * heard / events, 1), crc_bad=crc, valid=len(valid), valid_pct=round(100.0 * len(valid) / events, 1),
                unserviced=unserviced, max_run_miss=best, max_gap_ms=round(gap, 1), per_channel={k: v for k, v in sorted(ch.items())}, interval_ms=ivl,
                zoom_widened=sum(1 for e in ring if e.get("zoom") == 1), zoom_restored=sum(1 for e in ring if e.get("zoom") == 2), zoom_failed=sum(1 for e in ring if e.get("zoom") == 3))


def wait_for(fn, timeout: float, step: float = 0.2):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return None


async def scan_for(name: str, timeout: float):
    from bleak import BleakScanner
    t = time.time()
    dev = await BleakScanner.find_device_by_filter(lambda d, adv: name.lower() in (adv.local_name or d.name or "").lower(), timeout=timeout)
    return dev, time.time() - t


async def request_pref(addr: str, which: str) -> str:
    """ask Windows (11 22H2+) for a connection-parameter preset on the live link: power = the long-interval preset, balanced, throughput = the short one"""
    from winrt.windows.devices.bluetooth import BluetoothLEDevice, BluetoothLEPreferredConnectionParameters as P
    d = await BluetoothLEDevice.from_bluetooth_address_async(int(addr.replace(":", ""), 16))
    r = d.request_preferred_connection_parameters({"power": P.power_optimized, "balanced": P.balanced, "throughput": P.throughput_optimized}[which])
    return str(r.status)


async def one_attempt(mon: Monitor, name: str, hold: float, connect_timeout: float, scan_timeout: float, k: int, pref: str = "") -> dict:
    from bleak import BleakClient
    row = dict(attempt=k, error="", t={})
    T = row["t"]
    base = time.time()
    rel = lambda: round(time.time() - base, 2)  # noqa: E731
    s0 = mon.get()
    row["conn_before"] = s0.get("cind_ok", 0)
    dev, _ = await scan_for(name, scan_timeout)
    if not dev:
        row["error"] = "not found while scanning"
        row["unit_state_when_not_found"] = LL_STATES.get(mon.get().get("state"), "?")
        return row
    T["scan_found"] = rel()
    got = []
    connecting_from = len(mon.events)
    try:
        client = BleakClient(dev, timeout=connect_timeout)
        async with client as c:
            T["gatt_connected"] = rel()
            _ = list(c.services)
            T["services_done"] = rel()
            await c.start_notify(m.MIDI_CHAR, lambda _h, d: got.append(bytes(d)))
            T["notify_started"] = rel()
            if pref:
                try:
                    T["pref_request"] = f"{pref}: {await request_pref(dev.address, pref)}"
                except Exception as e:  # noqa: BLE001
                    T["pref_request"] = f"{pref}: {type(e).__name__}: {e}"[:80]
            ts = int(time.monotonic() * 1000) & 0x1FFF
            pkt = bytes([0x80 | (ts >> 7), 0x80 | (ts & 0x7F)]) + m.IDENTITY[:-1] + bytes([0x80 | (ts & 0x7F), 0xF7])
            await c.write_gatt_char(m.MIDI_CHAR, pkt, response=False)
            end = time.time() + hold
            while time.time() < end:
                await asyncio.sleep(0.2)
                if "midi_reply" not in T and any(b"Pocket" in g for g in got):
                    T["midi_reply"] = rel()
            T["our_disconnect"] = rel()
    except Exception as e:  # noqa: BLE001
        row["error"] = f"{type(e).__name__}: {e}"[:90]
        T["our_disconnect"] = rel()
    row["events"] = [e for e in mon.events[connecting_from:]]
    return row


def run_attempts(mon: Monitor, args) -> list[dict]:
    rows = []
    for k in range(1, args.attempts + 1):
        # gate: the unit must be advertising (link layer ADV) with advertising events running, before anything starts
        def ready():
            s = mon.get()
            return s.get("state") == 1
        if not wait_for(ready, args.recover_wait):
            s = mon.get()
            print(f"  attempt {k}: NOT STARTED, the unit is still in link-layer state '{LL_STATES.get(s.get('state'))}' after {args.recover_wait:.0f} s "
                  f"(peer {s.get('peer')}, rx_good {s.get('rx_good')}); stopping so no attempt is piled on a lingering link")
            rows.append(dict(attempt=k, error="unit not advertising before the attempt", holder=s.get("peer")))
            break
        time.sleep(1.0)
        a0 = mon.get()["adv_events"]
        wait_for(lambda: mon.get()["adv_events"] > a0 + 1, 5.0)
        ev_mark = len(mon.events)
        t_start = time.time() - mon.t0
        row = asyncio.run(one_attempt(mon, args.name, args.hold, args.timeout, args.scan, k, args.pref))
        row["t_start_abs"] = round(t_start, 2)
        row["our_disconnect_abs"] = round(t_start + row["t"].get("our_disconnect", 0), 2)
        # recovery gate: the link closed, the state is advertising again, advertising events resumed, the scanner sees the unit
        closed = wait_for(lambda: mon.get().get("state") == 1 and any(e["kind"] == "link_closed" and e["t"] >= t_start for e in mon.events), args.recover_wait)
        row["link_closed_after_our_disconnect_s"] = round((max((e["t"] for e in mon.events if e["kind"] == "link_closed" and e["t"] >= t_start), default=0) - row["our_disconnect_abs"]), 2) if closed else None
        if closed:
            a1 = mon.get()["adv_events"]
            wait_for(lambda: mon.get()["adv_events"] > a1 + 1, 5.0)
            dev, dt = asyncio.run(scan_for(args.name, args.scan))
            row["rescan_found"] = bool(dev)
            row["rescan_s"] = round(dt, 2)
        else:
            row["rescan_found"] = None
        row["timeline"] = [e for e in mon.events[ev_mark:]]
        rows.append(row)
        ok = "OK" if not row["error"] else "FAIL"
        print(f"  attempt {k}: {ok} {row['error']}  gatt {row['t'].get('gatt_connected', '-')}s  notify {row['t'].get('notify_started', '-')}s  reply {row['t'].get('midi_reply', '-')}s  "
              f"closed {row['link_closed_after_our_disconnect_s']}s after our disconnect  rescan {row.get('rescan_found')}", flush=True)
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="Custom MIDI")
    ap.add_argument("--name", default="Pocket")
    ap.add_argument("--watch", type=float, default=0.0, help="just trace for this many seconds")
    ap.add_argument("--attempts", type=int, default=0)
    ap.add_argument("--hold", type=float, default=5.0)
    ap.add_argument("--pref", default="", choices=["", "power", "balanced", "throughput"], help="ask Windows for this connection-parameter preset right after the notifications are on")
    ap.add_argument("--timeout", type=float, default=20.0)
    ap.add_argument("--scan", type=float, default=10.0)
    ap.add_argument("--recover-wait", type=float, default=60.0, help="how long to wait for a link to close / advertising to resume before giving up")
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    unit = LockedUnit(a.port)
    print("BUILD", unit.build_id(), flush=True)
    mon = Monitor(unit)
    mon.start()
    time.sleep(1.0)
    rows = []
    try:
        if a.attempts:
            rows = run_attempts(mon, a)
        else:
            time.sleep(a.watch or 30.0)
    finally:
        mon.stop_flag = True
        time.sleep(0.6)
    print(f"\nmonitor: {mon.polls} polls ({mon.polls / max(1e-9, time.time() - mon.t0):.1f}/s), {mon.errors} read errors")
    print("\nTIMELINE (seconds from the start of the trace)")
    for e in mon.events:
        rest = {k: v for k, v in e.items() if k not in ("t", "kind")}
        print(f"  {e['t']:8.2f}  {e['kind']:<22} {rest if rest else ''}")
    print("\nCONNECTIONS (every CONNECT_IND the unit accepted)")
    summary = []
    starts = [row.get("t_start_abs") for row in rows if row.get("t_start_abs") is not None]
    first_init = {}
    for n, cn in sorted(mon.conns.items()):
        r = reception(cn)
        life = (cn["end"] - cn["start"]) if cn["end"] else None
        # an attempt owns every connection from its start until the next attempt starts (Windows retries a failed connection by itself); a connection from another address, or outside any attempt, is foreign
        owner = None
        for i, s0 in enumerate(starts):
            if cn["start"] >= s0 - 0.3 and (i + 1 == len(starts) or cn["start"] < starts[i + 1] - 0.3):
                owner = rows[i]["attempt"]
        if owner is not None:
            first_init.setdefault(owner, cn["peer"])
        who = (f"our attempt {owner}" if owner is not None and first_init[owner] == cn["peer"] else "FOREIGN (different address from our attempt)") if owner is not None else "outside any of our attempts"
        print(f"  #{n}: initiator {cn['peer']} ({'random' if cn['peer_rand'] else 'public'}), interval {cn['interval'] * 1.25:.2f} ms, supervision {cn['timeout'] * 10} ms, {who}")
        print(f"      first data {('%.2f s after CONNECT_IND' % (cn['first_data'] - cn['start'])) if cn['first_data'] else 'NEVER'};  closed {('after %.1f s: ' % life + str(cn['close'])) if life is not None else 'NOT YET (still active)'}")
        if r["events"]:
            print(f"      events {r['events']}  heard {r['heard']} ({r['heard_pct']}%)  valid {r['valid']} ({r['valid_pct']}%)  crc_bad {r['crc_bad']}  unserviced {r['unserviced']}  "
                  f"max run of missed events {r['max_run_miss']}  max gap between valid packets {r['max_gap_ms']} ms"
                  + (f"  autozoom: widened {r['zoom_widened']}, restored {r['zoom_restored']}, refused {r['zoom_failed']}" if r.get("zoom_widened") or r.get("zoom_restored") or r.get("zoom_failed") else ""))
        if "hop" in cn:
            print(f"      CONNECT_IND: ChSel {cn['chsel']} (1 = channel selection algorithm #2), hop {cn['hop']}, SCA code {cn['sca']}, window {cn['win_size'] * 1.25:.2f} ms at offset {cn['init_win_offset_ms']:.2f} ms, "
                  f"initial channel map {cn['initial_map']} = {cn['initial_channels']} channels")
        summary.append(dict(id=n, initiator=cn["peer"], addr_type="random" if cn["peer_rand"] else "public", who=who, owner=owner, start=cn["start"], life_s=life, close=cn["close"],
                            cind={k: cn[k] for k in cn if k in ("aa", "hdr", "chsel", "txadd", "win_size", "hop", "sca", "initial_map", "initial_channels", "init_interval_ms", "init_win_offset_ms", "init_latency", "init_timeout_ms")},
                            first_data_s=(cn["first_data"] - cn["start"]) if cn["first_data"] else None, ring_raw=[[e["evt"], int(e["heard"]) | int(e["crc"]) << 1, e["ch"], e["dt"], e["ch_alt"]] for e in cn["ring"]], **r))
    # validation: the ring (per-event, this tool's numerator and denominator) against the stack's own independent packet counters over the same period
    f, l = mon.first, mon.last
    if f and l:
        rg = [e for cn in mon.conns.values() for e in cn["ring"]]
        d = lambda k: l[k] - f[k]  # noqa: E731
        print("\nRECONCILIATION (this trace vs the stack's own counters, same period)")
        print(f"  events:   ring {len(rg):5d}   stack conn_events {d('conn_events'):5d}")
        print(f"  crc bad:  ring {sum(1 for e in rg if e['crc']):5d}   stack rx_crc_bad  {d('rx_crc_bad'):5d}")
        print(f"  packets:  ring events with a packet {sum(1 for e in rg if e['heard']):5d}   stack rx_good+rx_repeat+rx_crc_bad {d('rx_good') + d('rx_repeat') + d('rx_crc_bad'):5d}  "
              f"(good {d('rx_good')}, repeat {d('rx_repeat')}, bad {d('rx_crc_bad')}; empty PDUs among the good: {d('rx_empty')})")
    print("\nPER-ATTEMPT STATE TRANSITIONS (times in seconds from the attempt's start; 'created' = CONNECT_IND accepted, 'established' = first packet taken from the radio)")
    for row in rows:
        k = row["attempt"]
        T = row.get("t", {})
        print(f"  attempt {k}: PC side  scan_found {T.get('scan_found', '-')}  gatt_connected {T.get('gatt_connected', '-')}  services {T.get('services_done', '-')}  notify {T.get('notify_started', '-')}  "
              f"midi_reply {T.get('midi_reply', '-')}  pref {T.get('pref_request', '-')}  our_disconnect {T.get('our_disconnect', '-')}  result: {row.get('error') or 'OK'}")
        mine = [c for c in summary if c.get("owner") == k]
        s0 = row.get("t_start_abs", 0)
        for c in mine:
            cl = c["close"] or {}
            print(f"      link-layer connection #{c['id']} ({c['initiator']}, {c['addr_type']}): created {c['start'] - s0:6.2f}  established "
                  f"{('%6.2f' % (c['start'] - s0 + c['first_data_s'])) if c['first_data_s'] is not None else 'NEVER '}  closed "
                  f"{('%6.2f' % (c['start'] - s0 + c['life_s'])) if c['life_s'] is not None else 'active'} ({cl.get('by', '-')}, reason {cl.get('reason', '-')})  "
                  f"events {c['events']}, valid packets {c['valid']}, crc-bad {c['crc_bad']}")
        print(f"      recovery: link closed {row.get('link_closed_after_our_disconnect_s')} s after our disconnect; advertising restarts: "
              f"{sum(1 for e in row.get('timeline', []) if e['kind'] == 'adv_restart')}; advertising events seen again: {any(e['kind'] == 'adv_event' for e in row.get('timeline', []))}; scanner sees the unit again: {row.get('rescan_found')}")
    s = mon.get()
    print(f"\nCOUNTERS NOW: accepted {s['cind_ok']}, closed {s['closes']} (supervision {s['sup_to']}, establish-fail {s['estab_fail']}, peer-terminated {s['peer_term']}), advertising starts {s['adv_starts']}, "
          f"link-layer state '{LL_STATES.get(s['state'])}'")
    out = Path(a.out) if a.out else HERE.parent / "qa" / "ble-gate" / f"linktrace_{time.strftime('%Y%m%d_%H%M%S')}.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    json.dump(dict(build=unit.build_id(), events=mon.events, connections=summary, attempts=[{k: v for k, v in r.items() if k != "events"} for r in rows]), open(out, "w"), indent=1, default=str)
    print("wrote", out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
