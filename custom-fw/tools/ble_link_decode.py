#!/usr/bin/env python3
"""
custom-fw/tools/ble_link_decode.py - the BLE stack's own diagnostic block (ble_dg) decoded on the host, for `jl_ram_read.py blelink`.

The block is the stack's (third_party/optimist_ble/src/ble_diag.h): counters, the last CONNECT_IND, a ring of the last 32 timestamped events and a ring of the last
64 protocol PDUs (ATT, L2CAP signalling, SMP, link-layer encryption control). It costs no extra firmware RAM; the byte layout comes from tools/ble_diag_layout.json
(gen_ble_diag_layout.py compiles the real header). decode(raw, layout) is pure so it can be tested without a unit.
"""
import struct

LL_OPS = {0x00: "CONNECTION_UPDATE_IND", 0x01: "CHANNEL_MAP_IND", 0x02: "TERMINATE_IND", 0x03: "ENC_REQ", 0x04: "ENC_RSP", 0x05: "START_ENC_REQ", 0x06: "START_ENC_RSP",
          0x07: "UNKNOWN_RSP", 0x08: "FEATURE_REQ", 0x09: "FEATURE_RSP", 0x0A: "PAUSE_ENC_REQ", 0x0B: "PAUSE_ENC_RSP", 0x0C: "VERSION_IND", 0x0D: "REJECT_IND",
          0x0E: "PERIPHERAL_FEATURE_REQ", 0x0F: "CONNECTION_PARAM_REQ", 0x10: "CONNECTION_PARAM_RSP", 0x11: "REJECT_EXT_IND", 0x12: "PING_REQ", 0x13: "PING_RSP",
          0x14: "LENGTH_REQ", 0x15: "LENGTH_RSP", 0x16: "PHY_REQ", 0x17: "PHY_RSP", 0x18: "PHY_UPDATE_IND", 0x19: "MIN_USED_CHANNELS_IND"}
ATT_OPS = {0x01: "ERROR_RSP", 0x02: "MTU_REQ", 0x03: "MTU_RSP", 0x04: "FIND_INFO_REQ", 0x05: "FIND_INFO_RSP", 0x06: "FIND_BY_TYPE_REQ", 0x07: "FIND_BY_TYPE_RSP",
           0x08: "READ_BY_TYPE_REQ", 0x09: "READ_BY_TYPE_RSP", 0x0A: "READ_REQ", 0x0B: "READ_RSP", 0x0C: "READ_BLOB_REQ", 0x0D: "READ_BLOB_RSP", 0x10: "READ_BY_GROUP_REQ",
           0x11: "READ_BY_GROUP_RSP", 0x12: "WRITE_REQ", 0x13: "WRITE_RSP", 0x52: "WRITE_CMD", 0x1B: "NOTIFY", 0x1D: "INDICATE", 0x1E: "CONFIRM"}
SMP_OPS = {0x01: "PAIRING_REQUEST", 0x02: "PAIRING_RESPONSE", 0x03: "PAIRING_CONFIRM", 0x04: "PAIRING_RANDOM", 0x05: "PAIRING_FAILED", 0x06: "ENCRYPTION_INFO",
           0x07: "CENTRAL_IDENTIFICATION", 0x08: "IDENTITY_INFO", 0x09: "IDENTITY_ADDRESS_INFO", 0x0A: "SIGNING_INFO", 0x0B: "SECURITY_REQUEST"}
SIG_OPS = {0x01: "COMMAND_REJECT", 0x12: "CONNECTION_PARAMETER_UPDATE_REQ", 0x13: "CONNECTION_PARAMETER_UPDATE_RSP"}
BD_EV = ["-", "enable", "adv_start", "adv_stop", "adv_drop", "cind_rx", "cind_ok", "cind_rej", "conn_set", "first_evt", "first_rx", "rx_bad", "rx_desync", "c3_zero",
         "ctl_rx", "ctl_tx", "instant", "close", "busy", "init", "master", "init_hit"]
CLOSE_BY = ["local", "the peer", "supervision timeout", "could not establish", "procedure timeout", "protocol error", "terminate not acknowledged"]
LL_ERR = {0x05: "authentication failure", 0x06: "PIN or key missing", 0x08: "connection timeout", 0x13: "remote user terminated", 0x14: "remote low resources",
          0x15: "remote power off", 0x16: "terminated by the local host", 0x1A: "unsupported remote feature", 0x22: "LL response timeout", 0x23: "LL procedure collision",
          0x28: "instant passed", 0x3B: "unacceptable connection parameters", 0x3D: "MIC failure", 0x3E: "connection failed to be established"}
CIND_WHY = ["accepted", "not advertising", "bad format", "RxAdd", "AdvA mismatch", "bad parameters", "bad window size", "bad window offset", "bad hop", "bad channel map",
            "bad access address"]


def pdu_name(ch, b):
    kind = ch & 0x7F
    if not b:
        return "(empty)"
    tab, nm = {1: (ATT_OPS, "att"), 2: (SIG_OPS, "l2cap-sig"), 3: (SMP_OPS, "smp"), 4: (LL_OPS, "ll")}.get(kind, ({}, "?"))
    return "%s %s" % (nm, tab.get(b[0], "op %#04x" % b[0]))


def ll_detail(ch, b, evt):
    """what the kept octets of an LL control PDU mean (see the PA-MOD note in third_party/optimist_ble/README.md for the packing of CONNECTION_UPDATE_IND)"""
    if (ch & 0x7F) != 4 or not b:
        return ""
    op = b[0]
    if op == 0x01 and len(b) >= 8:
        inst = b[6] | b[7] << 8
        ahead = (inst - evt) & 0xFFFF
        return "  [channel map %s, instant %d = %s]" % (b[1:6].hex(), inst, ("%d events ahead of the driver's counter" % ahead) if ahead < 0x8000 else "ALREADY PASSED (%d events behind)" % (0x10000 - ahead))
    if op == 0x00 and len(b) >= 8:
        inst = b[6] | b[7] << 8
        ahead = (inst - evt) & 0xFFFF
        return "  [interval %.2f ms, latency %d, timeout %d ms, instant %d = %s]" % ((b[1] | b[2] << 8) * 1.25, b[3], (b[4] | b[5] << 8) * 10, inst,
                                                                              ("%d events ahead" % ahead) if ahead < 0x8000 else "ALREADY PASSED (%d events behind)" % (0x10000 - ahead))
    if op == 0x02 and len(b) >= 2:
        return "  [error %#04x: %s]" % (b[1], LL_ERR.get(b[1], "?"))
    return ""


def fields(raw, lay):
    g = {}
    for n in lay["u32"]:
        g[n] = struct.unpack_from("<I", raw, lay[n])[0]
    for n in lay["u16"]:
        g[n] = struct.unpack_from("<H", raw, lay[n])[0]
    for n in lay["u8"]:
        g[n] = raw[lay[n]]
    for n in lay["arr"]:
        g[n] = bytes(raw[lay[n]:lay[n] + (5 if n == "cind_chm" else lay["last"])])
    return g


def last_ops(ring, n_total, size):
    """the kept opcodes, oldest first (the ring has 'size' slots, n_total were written)"""
    k = min(n_total, size)
    return [ring[(i % size)] for i in range(n_total - k, n_total)]


def decode(raw, lay):
    """-> list of text lines"""
    g = fields(raw, lay)
    if g["magic"] != 0x4C454C42:
        return ["blelink: the diagnostic block does not carry its magic (overwritten?)"]
    now = g["now_us"]
    out = []
    ago = lambda t: "%+9.3f s" % ((t - now) / 1e6)
    out.append("BLE LINK DIAGNOSTICS (the stack's clock is %.3f s since the radio started; the times below are relative to the moment of the readout)" % (now / 1e6))
    out.append("  connections: CONNECT_IND heard %d, accepted %d, rejected %d; closed %d (supervision timeouts %d, failed to establish %d, terminated by the peer %d)"
               % (g["cind_rx"], g["cind_ok"], g["cind_rej"], g["closes"], g["sup_timeouts"], g["estab_fails"], g["peer_terms"]))
    if g["cind_rx"]:
        out.append("  last CONNECT_IND: %s; interval %.2f ms, latency %d, supervision timeout %d ms, window %.2f ms at offset %.2f ms, hop %d, SCA %d, access address %#010x, handled in %d us"
                   % (CIND_WHY[min(g["cind_rej_why"], len(CIND_WHY) - 1)], g["cind_interval"] * 1.25, g["cind_latency"], g["cind_timeout"] * 10, g["cind_win_size"] * 1.25,
                      g["cind_win_off"] * 1.25, g["cind_hop"], g["cind_sca"], g["cind_aa"], g["cind_isr_us"]))
    if g["closes"]:
        out.append("  last close: reason %#04x (%s), by %s, at event %d, %.0f ms after the CONNECT_IND, %.0f ms after the last packet received"
                   % (g["close_reason"], LL_ERR.get(g["close_reason"], "see the Core error codes"), CLOSE_BY[min(g["close_by"], 6)], g["close_evt"],
                      g["close_since_start_us"] / 1e3, g["close_since_rx_us"] / 1e3))
    rx = last_ops(g["ctl_rx"], g["ctl_rx_n"], lay["last"])
    tx = last_ops(g["ctl_tx"], g["ctl_tx_n"], lay["last"])
    out.append("  link-layer control PDUs received %d, sent %d. Last received: %s. Last sent: %s"
               % (g["ctl_rx_n"], g["ctl_tx_n"], " ".join(LL_OPS.get(b, "%#04x" % b) for b in rx) or "-", " ".join(LL_OPS.get(b, "%#04x" % b) for b in tx) or "-"))
    out.append("  data PDUs: queued for sending %d, acknowledged %d, nothing to send %d; received good %d, repeats %d, bad CRC %d, empty %d"
               % (g["tx_queued"], g["tx_acked"], g["tx_none"], g["rx_good"], g["rx_repeat"], g["rx_crc_bad"], g["rx_empty"]))
    out.append("  encryption requested %dx, links encrypted %d; MIDI notifications sent %d, write commands received %d; connection events %d (CRC bad %d, empty %d, desync %d); longest BLE interrupt %d us"
               % (g["enc_req_n"], g["enc_on_n"], g["att_ntf_n"], g["att_wcmd_n"], g["conn_events"], g["rx_crc_bad"], g["rx_empty"], g["rx_desync"], g["isr_max_us"]))
    rows = []
    n = min(g["ev_n"], lay["ring"])
    for i in range(g["ev_n"] - n, g["ev_n"]):
        o = lay["ev"] + (i % lay["ring"]) * lay["ev_size"]
        t, code, _pad, arg = struct.unpack_from("<IBBH", raw, o)
        rows.append((t, 0, "event %s %#06x" % (BD_EV[code] if code < len(BD_EV) else "?", arg)))
    n = min(g["pdu_n"], lay["pdus"])
    for i in range(g["pdu_n"] - n, g["pdu_n"]):
        o = lay["pdu"] + (i % lay["pdus"]) * lay["pdu_size"]
        t, evt, ch, ln = struct.unpack_from("<IHBB", raw, o)
        b = bytes(raw[o + 8:o + 16])[:min(ln, 8)]
        rows.append((t, 1, "%s %s%s   (event %d, %d bytes: %s)" % ("TX" if ch & 0x80 else "RX", pdu_name(ch, b), ll_detail(ch, b, evt), evt, ln, b.hex(" "))))
    rows.sort(key=lambda r: (r[0], r[1]))
    out.append("  TIMELINE, oldest first (%d events and %d PDUs recorded in all; the rings keep the last %d and %d):" % (g["ev_n"], g["pdu_n"], lay["ring"], lay["pdus"]))
    for t, _k, txt in rows:
        out.append("    %s  %s" % (ago(t), txt))
    return out

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
