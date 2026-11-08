#!/usr/bin/env python3
"""Analyse the reference rhythm library: the bar structure of every style, whether the
content is a repeating phrase, and how much silence is left at the end of the loop.

The point: the Groove Station must loop seamlessly.  The exported MIDI has to tell us
what the original device's loop length actually is, and whether the content is one
phrase or a repeated unit plus a fill.
"""
import os, sys, collections
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import gen_drum_patterns as G

BAR = G.BAR_TICKS


def analyse(idx, base, data):
    division, events = G.parse_smf(data)
    notes = {n for _t, n, _v in events}
    has_snare = bool(notes & {38, 40})
    hits = []
    for t, n, v in events:
        lane = G.lane_for(n, has_snare)
        if lane:
            hits.append((t, lane, v))
    hits.sort()
    if not hits:
        return None
    last = hits[-1][0]

    # how long does the last hit still ring?  the exporter gives every note a length
    # (note_len in the exporter); assume the shortest plausible ring, 60 ticks.
    ring_end = last + 60

    # periodicity: does the hit list repeat with period P?
    def sig(t, lane, v):
        return (G.LANE_NAMES[lane], v)

    def matches(P):
        by_t = collections.defaultdict(list)
        for t, lane, v in hits:
            by_t[t].append(sig(t, lane, v))
        for t in by_t:
            if t + P <= last:
                if sorted(by_t[t]) != sorted(by_t.get(t + P, [])):
                    return False
        return True

    periods = []
    for bars in (1, 2, 3, 4, 6, 8):
        P = bars * BAR
        if last >= P and matches(P):
            periods.append(bars)

    # bars that carry content
    bar_hits = collections.Counter(t // BAR for t, _l, _v in hits)
    return {
        "idx": idx, "base": base, "last": last, "ring_end": ring_end,
        "bars": bar_hits,
        "periods": periods,
        "hits": hits,
    }


def main():
    srcs = G.load_sources(G.DEFAULT_MIDS)
    print("%-16s %6s %8s %9s  %-28s %s" %
          ("style", "last", "file", "after", "repeats every", "hits per bar"))
    for idx, base, data in srcs:
        r = analyse(idx, base, data)
        if not r:
            continue
        name = G.STYLE_NAMES[idx]
        last, ring_end = r["last"], r["ring_end"]
        # the "gap" = how long after the last note's onset the record continues
        after = ring_end - last
        per = ", ".join("%d bar%s" % (p, "" if p == 1 else "s") for p in r["periods"]) or "-"
        bars = " ".join("%d:%d" % (b, c) for b, c in sorted(r["bars"].items()))
        print("%-16s %6d %8.2f %9d  %-28s %s" %
              (name, last, last / float(BAR), after, per, bars))


if __name__ == "__main__":
    main()

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
