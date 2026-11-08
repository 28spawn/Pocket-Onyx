#!/usr/bin/env python3
"""Generate custom-fw/src/dsp/drum_patterns.inc: the Groove Station's rhythm library.

The library is written HERE, by hand, in a readable grid notation - it is the project's
own material, not derived from any product's rhythm set. Genre names are generic. The
design rules (three voices per groove, no fills or crashes in a loop) are documented at
the top of the groove list.

    python custom-fw/tools/gen_drum_patterns.py           # writes src/dsp/drum_patterns.inc
    python custom-fw/tools/gen_drum_patterns.py --check   # exit 1 if the file differs

Notation
--------
A style is a list of bars. A bar maps a lane letter to a string, one character per grid
slot; spaces are ignored (they group beats for reading):

    grid 16  = 16ths in a 4/4 bar (each slot = 3 engine steps)
    grid 12  = 8th-note triplets in a 4/4 bar (each slot = 4 engine steps) - shuffle, swing,
               12/8 and 6/8 feels

    X = accent (velocity 120)   x = normal (100)   o = soft (80)   g = ghost (44)   . = rest

    K kick   S snare   R side stick   H closed hat   P pedal hat   O open hat
    C crash  Y ride    B ride bell    L low tom      M mid tom     T high tom

Engine format (pa_drums.h): 48 steps per 4/4 bar; each step word = lane bits 0..11 and the
step's loudest velocity >> 3 in bits 12..15.
"""
import argparse
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
OUT = os.path.join(FW, "src", "dsp", "drum_patterns.inc")

LANES = {"K": 0, "S": 1, "R": 2, "H": 3, "P": 4, "O": 5, "C": 6, "Y": 7, "B": 8,
         "L": 9, "M": 10, "T": 11}
VEL = {"X": 120, "x": 100, "o": 80, "g": 44}
STEPS_PER_BAR = 48


def bar(**lanes):
    return {k: v.replace(" ", "") for k, v in lanes.items() if v}


def with_(base, **over):
    """A copy of a bar with some lanes replaced (the kick variation of bar 2)."""
    b = dict(base)
    b.update({k: v.replace(" ", "") for k, v in over.items()})
    return {k: v for k, v in b.items() if v}


def loop2(main, **bar2):
    """A two-bar loop: the groove, then the same groove with a small change (usually the
    kick). No fills and no crashes inside the loop - a loop repeats forever, and a fill or
    a crash every two bars is what made the old library sound busy."""
    return [main, with_(main, **bar2) if bar2 else main]


# ---- the grooves -------------------------------------------------------------------
# Design rules (learned from how good drum-machine libraries use their kits, e.g. the
# factory set of a commercial guitar-modeler drum machine: 12 of its 16 styles use exactly
# kick + snare + closed hat, toms appear in 2 styles, a crash in 1, the ride in none):
#
#   * three voices per groove: kick, snare (or side stick), ONE timekeeper;
#   * the timekeeper is the closed hat, except where the genre is defined by another one:
#     open hat (punk), low tom (the tom-driven hard rock groove), side stick (reggae,
#     country), ride + pedal hat (swing and jazz only);
#   * toms only in the two tom grooves; no crash, no ride bell, no fills in a loop.
#
# The patterns themselves are this project's own.
G = []          # (name, grid, bars)


def add(name, grid, bars):
    G.append((name, grid, bars))


# -- straight 8ths / 16ths --------------------------------------------------------------
b = bar(H="xgox xgox xgox xgox", K="X... ..x. X... ....", S=".... X... .... X...")
add("8 BEAT 1", 16, [b, b, b, with_(b, K="X... ..x. X.x. ....")])
b = bar(H="x.x. x.x. x.x. x.x.", K="X... ..x. X.x. ....", S=".... X... .... X...")
add("8 BEAT 2", 16, loop2(b, K="X... ..x. X... ..x."))
b = bar(H="x.x. x.x. x.x. x.x.", K="X... .... X... ..x.", S=".... X... .... X...")
add("8 BEAT 3", 16, loop2(b, K="X... .... X.x. ...."))
b = bar(H="x... x... x... x.o.", K="X... ..x. X... ....", S=".... X... .... X...")
add("8 BEAT 4", 16, loop2(b, K="X... ..x. X..x ...."))
b = bar(H="xoxo xoxo xoxo xoxo", K="X... ...x ..X. ....", S=".... X... .... X...")
add("16 BEAT 1", 16, loop2(b, K="X... ...x ..X. .x.."))
b = bar(H="xoxo xoxo xoxo xoxo", K="X.x. .... ..X. .x..", S="..g. X.g. .g.. X.g.")
add("16 BEAT 2", 16, loop2(b, K="X.x. .... ..X. ...."))
b = bar(H="xgxg xgxg xgxg xgxg", K="X..X ..X. .X.. ..X.", S=".... X... .... X...")
add("16 BEAT 3", 16, loop2(b, K="X..X ..X. .... ..X."))
b = bar(H="xoxo xoxo xoxo xoxo", K="X.xX ..x. X.x. .xX.", S="g..g X.gg .g.g X.gg")
add("16 BEAT 4", 16, loop2(b, K="X.xX ..x. X.x. ...."))

# -- pop / rock ------------------------------------------------------------------------
b = bar(H="x.x. x.x. x.x. x.x.", K="X... ..x. X... ..x.", S=".... X... .... X...")
add("POP 1", 16, loop2(b, K="X... ..x. X.x. ...."))
b = bar(H="..x. ..x. ..x. ..x.", K="X... .... X... ....", S=".... X... .... X...")
add("POP 2", 16, loop2(b, K="X... .... X..x ...."))
b = bar(H="X.x. X.x. X.x. X.x.", K="X... .... X... ....", S=".... X... .... X...")
add("ROCK 1", 16, loop2(b, K="X... .... X.X. ...."))
b = bar(H="X.x. X.x. X.x. X.x.", K="X.X. .... X.X. ....", S=".... X... .... X...")
add("ROCK 2", 16, loop2(b, K="X.X. .... X... ..x."))
b = bar(L="x.x. x.x. x.x. x.x.", K="X... ..X. X... ....", S=".... X... .... X...")
add("HARD ROCK", 16, loop2(b, K="X... ..X. X.X. ...."))       # the low tom keeps time
b = bar(H="x.x. x.x. x.x. x.x.", K="X... ..x. .... ....", S=".... .... X... ....")
add("HALF TIME", 16, loop2(b, K="X... ..x. .... ..x."))
b = bar(O="x.x. x.x. x.x. x.x.", K="X.X. ..X. X.X. ..X.", S=".... X... .... X...")
add("PUNK", 16, loop2(b))                                     # open hat keeps time
b = bar(H="x.x. x.x. x.x. x.x.", K="XXXX XXXX XXXX XXXX", S=".... X... .... X...")
add("METAL 1", 16, loop2(b))
b = bar(H="X.X. X.X. X.X. X.X.", K="X.X. X.X. X.X. X.X.", S="..X. ..X. ..X. ..X.")
add("METAL 2", 16, loop2(b))

# -- dance / funk / soul ------------------------------------------------------------------
b = bar(H="..x. ..x. ..x. ..x.", K="X... X... X... X...", S=".... X... .... X...")
add("DISCO", 16, loop2(b))
b = bar(H="xoxo xoxo xoxo xoxo", K="X... ..X. ..X. ....", S="..g. X..g .g.. X..g")
add("FUNK 1", 16, loop2(b, K="X... ..X. ..X. .X.."))
b = bar(H="x.xo x.xo x.xo x.xo", K="X..X ...X ..X. ....", S=".g.. X.g. .g.g X...")
add("FUNK 2", 16, loop2(b, K="X..X ...X .... ..X."))
b = bar(H="x.x. x.x. x.x. x.x.", K="X... ...x X... ....", S=".... X... .... X..g")
add("SOUL", 16, loop2(b, K="X... ...x X.x. ...."))
b = bar(H="x.x. x.x. x.x. x.x.", K="X... X... X... X...", S="x... X... x... X...")
add("MOTOWN", 16, loop2(b))
b = bar(H="x.x. x.x. x.x. x.xx", K="X... ...X ..X. ....", S=".... X... .... X...")
add("HIP HOP", 16, loop2(b, K="X... ...X .... .X.."))
b = bar(H="..x. ..x. ..x. ..x.", R=".... .... X... ....", K=".... .... X... ....")
add("REGGAE", 16, loop2(b))                                   # one drop: side stick on 3
b = bar(H="x.x. x.x. x.x. x.x.", K="X... .... ..X. ....", S=".... X... .... X...")
add("BALLAD", 16, loop2(b, K="X... .... X... ...."))
b = bar(H="x.x. x.x. x.x. x.x.", K="X... .... X... ....", R=".... X... .... X...")
add("COUNTRY", 16, loop2(b, K="X... .... X..x ...."))         # side stick backbeat
b = bar(S="gxgg gxgg gxgg gxgg", K="X... .... X... ....", H="..x. ..x. ..x. ..x.")
add("TRAIN", 16, loop2(b))

# -- latin / world -------------------------------------------------------------------
b = bar(H="x.x. x.x. x.x. x.x.", R="X..X ..X. ..X. .X..", K="X..x X..x X..x X..x")
add("BOSSA", 16, loop2(b, R="..X. .X.. X..X ..X."))           # the clave answers in bar 2
b = bar(H="xoox xoox xoox xoox", K="X..x X..x X..x X..x", R="..x. .... ..x. ....")
add("SAMBA", 16, loop2(b, R=".... ..x. .... ..x."))
b = bar(H="x.x. x.x. x.x. x.x.", K="X... ..X. X... ....", S=".... X... ..x. X...")
add("LATIN ROCK", 16, loop2(b, K="X... ..X. X..X ...."))
b = bar(K="X... ..X. X... ..X.", L="..x. .... ..x. ....", M=".... x..x .... x...",
        T=".x.. .... .x.. ....")
add("TRIBAL", 16, loop2(b, T=".x.. .... .x.x ...."))          # the tom groove

# -- triplet feels (12 slots = 4 beats x 3) ----------------------------------------
b = bar(H="x.x x.x x.x x.x", K="X.. ..x X.. ...", S="... X.. ... X..")
add("SHUFFLE 1", 12, loop2(b, K="X.. ..x X.x ..."))
b = bar(H="x.x x.x x.x x.x", K="X.. ... X.x ...", S="..g X.g ..g X.g")
add("SHUFFLE 2", 12, loop2(b, K="X.. ... X.. ..x"))
b = bar(H="x.x x.x x.x x.x", K="X.. ..x ... ...", S="..g ..g X.g ..g")
add("HALF SHUFFLE", 12, loop2(b, K="X.. ..x ... ..x"))
b = bar(H="xxx xxx xxx xxx", K="X.. ... X.. ...", S="... X.. ... X..")
add("BLUES 12/8", 12, loop2(b, K="X.. ..x X.. ..."))
b = bar(Y="x.. x.x x.. x.x", P="... x.. ... x..", K="o.. ... o.. ...")
add("SWING", 12, loop2(b))                                    # ride + pedal hat on 2 and 4
b = bar(Y="x.. x.x x.. x.x", P="... x.. ... x..", S="..g ... ..g ..g")
add("JAZZ", 12, loop2(b, S="... ..g ..g ..."))
b = bar(H="x.x x.x x.x x.x", K="X.x X.x X.x X.x", S="... X.. ... X..")
add("BOOGIE", 12, loop2(b))
b = bar(H="x.x x.x x.x x.x", K="X.. ... ..x ...", S="... X.. ... X.g")
add("GOSPEL", 12, loop2(b, K="X.. ..x ..x ..."))

b = bar(H="x.x. x.x. x.x. x.x.", K="X... .... X... ....", S="..x. X.x. ..x. X.x.")
add("ROCKABILLY", 16, loop2(b))

# ---- the metronome styles: the click voice; any lane bit plays it ------------------
METRO = [
    ("METRO 0", 16, [bar(K="x... x... x... x...")]),             # quarters, even
    ("METRO 1", 16, [bar(K="X... .... x... ....")]),             # halves
    ("METRO 2", 16, [bar(K="x.x. x.x. x.x. x.x.")]),             # 8ths, even
    ("METRO 3", 16, [bar(K="X... x... x... x...")]),             # quarters, beat 1 accented
    ("METRO 4", 16, [bar(K="X.o. x.o. x.o. x.o.")]),             # 8ths, accented
    ("METRO 5", 16, [bar(K="Xooo xooo xooo xooo")]),             # 16ths
    ("METRO 6", 12, [bar(K="Xoo xoo xoo xoo")]),                 # triplets
    ("METRO 7", 16, [bar(K="X... x... X... x...")]),             # 2 + 2
    ("METRO 8", 16, [bar(K="X... x... x... x..."), bar(K="x... x... x... x...")]),  # 2-bar
]


def encode(grid, bars, name):
    per = STEPS_PER_BAR // grid
    out = []
    for bi, b in enumerate(bars):
        steps = [[0, 0] for _ in range(STEPS_PER_BAR)]   # lane bits, max velocity
        for lane, s in b.items():
            if len(s) != grid:
                raise SystemExit(f"{name} bar {bi + 1} lane {lane}: {len(s)} slots, not {grid}")
            for i, ch in enumerate(s):
                if ch == ".":
                    continue
                if ch not in VEL:
                    raise SystemExit(f"{name} bar {bi + 1} lane {lane}: bad character {ch!r}")
                st = steps[i * per]
                st[0] |= 1 << LANES[lane]
                st[1] = max(st[1], VEL[ch])
        for lanes, vel in steps:
            out.append(lanes | (((vel >> 3) & 0xF) << 12) if lanes else 0)
    return out


def build():
    styles = G + METRO
    if len(G) != 40 or len(styles) != 49:
        raise SystemExit(f"the library must have 40 grooves + 9 metros (has {len(G)} + {len(METRO)})")
    names = [s[0] for s in styles]
    if len(set(names)) != len(names):
        raise SystemExit("duplicate style names")
    return [(n, encode(g, b, n)) for n, g, b in styles]


def emit(lib):
    total = sum(len(s) for _, s in lib)
    b = io.StringIO()
    b.write("/* custom-fw/src/dsp/drum_patterns.inc - the Groove Station's rhythm library\n"
            " *\n"
            " * GENERATED FILE - do not edit by hand.  Regenerate with\n"
            " *\n"
            " *     python custom-fw/tools/gen_drum_patterns.py\n"
            " *\n"
            " * The library is the project's own, written in that tool's grid notation.\n"
            " * 48 steps per 4/4 bar; per step: bits 0..11 = PA_DRUM_L_* lanes, bits 12..15 =\n"
            f" * velocity >> 3.  {len(lib)} styles, {total} steps, {total * 2} bytes.\n"
            " */\n\n")
    b.write("static const uint16_t k_groove_steps[] = {\n")
    for name, steps in lib:
        b.write(f"    /* {name} */\n")
        for i in range(0, len(steps), 12):
            b.write("    " + ", ".join(f"0x{w:04X}" for w in steps[i:i + 12]) + ",\n")
    b.write("};\n\n")
    off, offs = 0, []
    for _, s in lib:
        offs.append(off)
        off += len(s)
    b.write("static const uint16_t k_groove_off[PA_DRUM_STYLE_COUNT] = {\n")
    for i in range(0, len(offs), 10):
        b.write("    " + ", ".join(str(o) for o in offs[i:i + 10]) + ",\n")
    b.write("};\n\n")
    b.write("static const uint16_t k_groove_len[PA_DRUM_STYLE_COUNT] = {\n")
    lens = [len(s) for _, s in lib]
    for i in range(0, len(lens), 10):
        b.write("    " + ", ".join(str(n) for n in lens[i:i + 10]) + ",\n")
    b.write("};\n\n")
    b.write("static const char * const k_groove_names[PA_DRUM_STYLE_COUNT] = {\n")
    for name, _ in lib:
        b.write(f'    "{name}",\n')
    b.write("};\n")
    return b.getvalue()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--out", default=OUT)
    a = ap.parse_args()
    text = emit(build())
    if a.check:
        cur = open(a.out, encoding="utf-8").read() if os.path.exists(a.out) else ""
        print(f"{a.out}: {'matches the generator' if cur == text else 'DIFFERS from the generator'}")
        return 0 if cur == text else 1
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"wrote {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
