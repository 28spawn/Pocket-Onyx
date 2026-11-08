#!/usr/bin/env python3
"""
custom-fw/tools/gen_ble_diag_layout.py - the byte layout of the BLE stack's diagnostic block (struct ble_diag in
third_party/optimist_ble/src/ble_diag.h) for jl_ram_read.py's `blelink`.

The struct holds only fixed-width integers (no pointers), so its layout is the same on the host and on the unit's pi32 core:
this compiles a small C program with the host clang against the real header, with the stack's settings from
src/board/ble_radio.c (BLE_CENTRAL 0, FELUCCA_BLE_BOND 1, BLE_DIAG 1), prints offsetof() of every field the decoder
uses, and writes tools/ble_diag_layout.json. Run it again when those settings or the header change; jl_ram_read.py
checks the total size against the unit's own sizeof(ble_dg) and says so when the layout is stale.

    python custom-fw/tools/gen_ble_diag_layout.py
"""
import glob
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "third_party", "optimist_ble", "src")
U32 = ["magic", "adv_starts", "adv_events", "adv_rx", "scan_req", "adv_drop", "busy_timeouts", "cind_rx", "cind_ok", "cind_rej", "cind_aa", "cind_crc", "cind_isr_us",
       "first_rx_us", "evt_irqs", "rx_irqs", "conn_events", "c3_zero", "evt_same", "rx_good", "rx_crc_bad", "rx_repeat", "rx_empty", "rx_nothing", "rx_desync",
       "tx_queued", "tx_acked", "tx_none", "clk_step_max", "ctl_rx_n", "ctl_tx_n", "att_rx_n", "closes", "sup_timeouts", "estab_fails", "peer_terms",
       "close_since_rx_us", "close_since_start_us", "now_us", "ev_n", "pdu_n", "att_ntf_n", "att_wcmd_n", "enc_req_n", "enc_on_n", "isr_max_us"]
U16 = ["adv_drop_stat", "adv_drop_hdr", "busy_max", "cind_win_off", "cind_interval", "cind_latency", "cind_timeout", "first_rx_evt", "first_evt", "rx_bad_stat", "last_evt", "close_evt"]
U8 = ["cind_rej_why", "cind_hdr", "cind_win_size", "cind_hop", "cind_sca", "close_reason", "close_by"]
ARR = ["cind_chm", "ctl_rx", "ctl_tx", "att_rx"]          # byte arrays

HEAD = r'''
#include <stdio.h>
#include <stddef.h>
#define BLE_CENTRAL 0
#define FELUCCA_BLE_BOND 1
#define BLE_DIAG 1
#include "ble_cfg.h"
#include "ble_diag.h"
#define F(n) printf("  \"%s\": %zu,\n", #n, offsetof(struct ble_diag, n))
int main(void) {
    printf("{\n  \"sizeof\": %zu,\n", sizeof(struct ble_diag));
    printf("  \"ring\": %u,\n  \"pdus\": %u,\n  \"last\": %u,\n", (unsigned)BLE_DIAG_RING, (unsigned)BLE_DIAG_PDUS, (unsigned)BLE_DIAG_LAST);
    printf("  \"ev_size\": %zu,\n  \"pdu_size\": %zu,\n", sizeof(((struct ble_diag *)0)->ev[0]), sizeof(((struct ble_diag *)0)->pdu[0]));
    F(ev); F(pdu);
'''
TAIL = r'''
    printf("  \"_end\": 0\n}\n");
    return 0;
}
'''


def find_clang():
    try:
        subprocess.run(["clang", "--version"], capture_output=True, check=True)
        return "clang"
    except Exception:
        pass
    g = glob.glob(os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW.UCRT*\**\clang.exe"), recursive=True)
    if g:
        return g[0]
    sys.exit("no clang found")


def main():
    names = U32 + U16 + U8 + ARR
    c = HEAD + "".join("    F(%s);\n" % n for n in names) + TAIL
    with tempfile.TemporaryDirectory() as td:
        cf, ex = os.path.join(td, "lay.c"), os.path.join(td, "lay.exe")
        open(cf, "w").write(c)
        r = subprocess.run([find_clang(), "-I", SRC, cf, "-o", ex], capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stderr[-2500:])
        out = subprocess.run([ex], capture_output=True, text=True, check=True).stdout
    lay = json.loads(out)
    lay.pop("_end", None)
    lay["u32"], lay["u16"], lay["u8"], lay["arr"] = U32, U16, U8, ARR
    json.dump(lay, open(os.path.join(HERE, "ble_diag_layout.json"), "w"), indent=1)
    print("struct ble_diag:", lay["sizeof"], "bytes; ring", lay["ring"], "pdus", lay["pdus"], "-> tools/ble_diag_layout.json")


if __name__ == "__main__":
    main()

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
