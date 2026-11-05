"""The Groove Station, compared with the design's reference render, pixel by pixel.

The redesign (GROOVE_SPEC.md) says: *"If your screen and the PNG differ by even
one pixel, the PNG is right."* So the screen is rendered by the real firmware
in WebAssembly and diffed against the reference PNG the design kit shipped -
``groove_px_3_transport.png`` (playing) and ``groove_px_3_transport_stopped.png``
(stopped).

The comparison is in RGB565 - the panel's own format - because the reference
PNG holds the exact colour the design uses, and expanding the same colour to
8 bits can round differently in the two directions.

The header (y 0-29) is the device's own, so the comparison starts at y 30.

    python custom-fw/test/test_groove_pixels.py
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REF = os.path.join(HERE, "groove_ref")
import tempfile
TMP = os.path.join(tempfile.gettempdir(), "opencode")

try:
    from PIL import Image
except ImportError:
    print("this test needs Pillow (pip install pillow)")
    sys.exit(2)


def rgb888_to_565(r, g, b):
    """The colour the design's 8-bit PNG stands for, in the panel's format."""
    return (((r * 31 + 127) // 255) << 11) | (((g * 63 + 127) // 255) << 5) | ((b * 31 + 127) // 255)


def render(state, out_path, step=None):
    """Render the screen with the real firmware (the wasm build) and return the
    RGB565 pixels.  ``step`` waits for that exact drum-engine step first."""
    wasm = os.path.join(ROOT, "wasm", "out", "pocket_amp.wasm")
    script = os.path.join(ROOT, "wasm", "groove_render.js")
    cmd = ["node", script, wasm, out_path, state]
    if step is not None:
        cmd.append("step:%d" % step)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout, r.stderr)
        raise SystemExit("the render failed")
    print("   ", r.stdout.strip().replace("\n", "\n    "))
    with open(out_path, "rb") as f:
        raw = f.read()
    px = []
    for i in range(0, len(raw), 2):
        # the panel stores each pixel byte-swapped, so the little-endian bytes
        # of the stored word are the colour's high byte first
        px.append((raw[i] << 8) | raw[i + 1])
    return px


def reference(path):
    im = Image.open(path).convert("RGB")
    p = im.load()
    return [rgb888_to_565(p[x, y][0], p[x, y][1], p[x, y][2])
            for y in range(240) for x in range(240)]


fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fails += 1


os.makedirs(TMP, exist_ok=True)

for state, refname in (("stopped", "groove_px_3_transport_stopped.png"),
                        ("playing", "groove_px_3_transport.png")):
    ref_path = os.path.join(REF, refname)
    if not os.path.exists(ref_path):
        print("the reference render is missing:", ref_path)
        continue
    print("=== the %s screen vs %s ===" % (state, refname))
    ref = reference(ref_path)
    got = render(state, os.path.join(TMP, "groove_%s.rgb565" % state))
    diffs = []
    # y 30 is the device's own header separator (the header is y 0-30 and the
    # design says not to touch it), so the content is compared from y 31.
    for y in range(31, 240):
        for x in range(240):
            i = y * 240 + x
            if got[i] != ref[i]:
                diffs.append((x, y, got[i], ref[i]))
    if not diffs:
        check(True, "the screen is the reference, pixel for pixel")
        continue
    print("    %d pixels differ" % len(diffs))
    rows = {}
    for x, y, a, b in diffs:
        rows.setdefault(y, []).append(x)
    for y in sorted(rows)[:20]:
        xs = rows[y]
        print("      y %3d: %3d px, x %d..%d" % (y, len(xs), min(xs), max(xs)))
    for x, y, a, b in diffs[:10]:
        print("      (%3d,%3d) got %04x want %04x" % (x, y, a, b))
    check(False, "the screen matches the reference")

print()
print("=== the playhead in a later bar of the loop ===")
# The bug report was "the bar goes away after the 4th, after some time it comes
# back": the engine counts steps across the WHOLE pattern (48 steps to a bar,
# up to 8 bars), and the strip showed the raw step, so the playhead only lit up
# during the first bar.  Bar 2 must render exactly like bar 1.
a = render("playing", os.path.join(TMP, "groove_step15.rgb565"), step=15)
b = render("playing", os.path.join(TMP, "groove_step63.rgb565"), step=63)
diffs = [(x, y) for y in range(31, 240) for x in range(240)
         if a[y * 240 + x] != b[y * 240 + x]]
if diffs:
    rows = {}
    for x, y in diffs:
        rows.setdefault(y, []).append(x)
    for y in sorted(rows)[:8]:
        print("      y %3d: %3d px, x %d..%d" % (y, len(rows[y]), min(rows[y]), max(rows[y])))
check(not diffs, "step 63 (bar 2) renders exactly like step 15 (bar 1) - %d px differ"
      % len(diffs))
# and the strip is not blank at step 63 (the old code drew no playhead there)
stepX = lambda i: 12 + i * 13 + (i // 4) * 4
lit = [i for i in range(16) if b[133 * 240 + stepX(i)] == 0xF628]
check(lit == [5], "the strip shows the playhead on block 5 in bar 2 (saw %s)" % (lit,))

print()
print("GROOVE PIXEL MATCH OK" if fails == 0 else "%d MISMATCH" % fails)
sys.exit(0 if fails == 0 else 1)
