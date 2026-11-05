# Groove Station: implementation spec (option 3, "Big transport")

![layout](GROOVE_SPEC.png)

Reference renders are `groove_px_3_transport.png` (playing) and
`groove_px_3_transport_stopped.png` (stopped). The reference code is `transport()` in
`groove_pixel.py`. If your screen and the PNG differ by even one pixel, the PNG is right.

## 0. Ground rules

- **240×240 screen, 1:1 pixels.** No anti-aliasing, no smoothing, no fractional positions.
- **Coordinates are inclusive.** `x 8–231` means pixel 8 up to and including pixel 231.
- **Font:** the 5×7 table in [`../onyx_font5x7.h`](../onyx_font5x7.h), uppercase only.
  - Each character advances 6 px × scale.
  - A string's width is `(len × 6 − 1) × scale`.
  - At scale *s*, every font pixel is drawn as an *s*×*s* block.
  - "Top" below always means the top row of the glyph cell.
- **Text alignment:**
  - **left:** x is the first pixel.
  - **centre:** `x0 = x − width / 2`, using integer division.
  - **right:** `x0 = x − width + 1`, so x is the last pixel.
- **Boxes have pixel-cut corners.** A box `(x0, y0, x1, y1)` with `cut = c`:
  - Its fill is the union of the rectangles `(x0+c … x1−c, y0 … y1)` and `(x0 … x1, y0+c … y1−c)`.
  - Its outline is four 1 px lines: the top and bottom run `x0+c … x1−c`, and the left and right run `y0+c … y1−c`.
  - If `c = 2`, also set the four diagonal pixels `(x0+1, y0+1)`, `(x1−1, y0+1)`, `(x0+1, y1−1)` and `(x1−1, y1−1)`, in the fill color and then the outline color.
- **The header (region 1, y 0–29) is the existing one.** Don't touch it.

### Colors (exact RGB565)

| Name | Hex | RGB565 | Used for |
|---|---|---|---|
| BG | `#000000` | `0x0000` | screen background |
| PANEL | `#181818` | `0x18C3` | box fills, keycap fills |
| LINE | `#292829` | `0x2945` | box outlines, unlit segments and steps |
| DIM | `#525052` | `0x528A` | chevrons, keycap outlines, idle downbeats, the "VOL-/+" hint |
| TEXT | `#EEEEEE` | `0xEF7D` | values, the style name, lit segments |
| MUTE | `#8B898B` | `0x8C51` | labels |
| SELECT | `#F6C641` | `0xF628` | tempo digits, the current step |
| OK | `#4ADA62` | `0x4ECC` | the Stop button while playing |

### Sprites (`#` = ink)

```
chev_l (3x5)    chev_r (3x5)    play (5x7)
..#             #..             #....
.#.             .#.             ##...
#..             ..#             ###..
.#.             .#.             ####.
..#             #..             ###..
                                ##...
                                #....
```

`play` at scale 2 means every pixel is drawn as a 2×2 block, so the sprite is 10×14.

## 1. Layout (numbers match the picture)

**2. Style card**
- Box `(8, 34, 231, 63)`, cut 2: fill PANEL, outline LINE.
- Style name: 1×, TEXT, centred on x 120, top y 40. Example: `8 BEAT 1`.
- `STYLE nn/40`: 1×, MUTE, centred on x 120, top y 51. Pad the number to two digits.
- `chev_l` at (16, 46) and `chev_r` at (221, 46), both MUTE.

**3. Tempo digits:** the BPM as plain digits, 5×, SELECT, centred on x 112, top y 80, 35 px tall.
- At 3 digits the width is 85 px, so the glyph cells run x 70–154. The ink of `116` starts at x 75, because the font's `1` has empty left columns. Position by the cells, not the ink.
- At 2 digits (40–99 BPM) the width is 55 px and it stays centred on x 112.

**4. BPM label:** `BPM`, 1×, MUTE, top y 108, so it sits on the digits' baseline.
- Left x = `112 + digits_width / 2 + 6`, which is 160 for 3 digits.
- Redraw it whenever the digit count changes.

**5. Tempo chevrons:** `chev_l` at (14, 95) and `chev_r` at (223, 95), DIM. They say "the knob changes this".

**6. Step strip:** 16 blocks, one per 16th note of a 4/4 bar.
- Block *i* (0–15) is 10 px wide: `x = 12 + i×13 + (i div 4)×4`, running x … x+9.
- That gives x = 12, 25, 38, 51 · 68, 81, 94, 107 · 124, 137, 150, 163 · 180, 193, 206, 219.
- Downbeats (i = 0, 4, 8, 12) cover y 128–136. The other blocks cover y 130–136.
- Color of block *i*, the first rule that matches wins:
  1. The current step: SELECT.
  2. Playing, in the same beat as the current step (`i div 4 == step div 4`) and before it: TEXT. This makes the beat fill in as it plays.
  3. A downbeat: DIM.
  4. Anything else: LINE.
- Stopped: there is no current step, so only rules 3 and 4 apply.

**7. Play / Stop button**
- Box `(8, 150, 116, 200)`, cut 2, fill PANEL.
- **Playing:**
  - Outline OK.
  - Stop square: solid OK fill `(18, 168, 29, 179)`.
  - `STOP`: 2×, OK, at (38, 162).
  - `PRESS PARA`: 1×, MUTE, at (38, 182).
- **Stopped:**
  - Outline TEXT.
  - `play` sprite at 2×, TEXT, at (18, 167).
  - `PLAY`: 2×, TEXT, at (38, 162).
  - `PRESS PARA`: 1×, MUTE, at (38, 182).

**8. Drum volume box**
- Box `(124, 150, 231, 200)`, cut 2: fill PANEL, outline LINE.
- `DRUM VOL`: 1×, MUTE, at (132, 160).
- The value, e.g. `70%`: 1×, TEXT, right-aligned so its last pixel is x 224, top y 160.
- Segments: 10 blocks of 7×6 px.
  - Block *k* is at `x = 132 + k×9`, covering y 172–177.
  - Lit blocks = `round(volume / 10)`: lit in TEXT, the rest in LINE.
- `VOL-/+`: 1×, DIM, centred on x 178, top y 184.

**9. Key hints:** two keycaps, centred as one group, text top at y 218.
- A keycap with text width *w*, at x:
  - Box `(x, 215, x+w+5, 227)`, cut 1: fill PANEL, outline DIM.
  - Key text: TEXT, at (x+3, 218).
  - Action text: MUTE, at (x+w+10, 218).
- `< >` `STYLE` starts at x 57.
- `TURN` `BPM` starts at x 133.

## 2. Behavior

| Control | Action | Redraw |
|---|---|---|
| PARA turn | BPM ±1 per detent, range 40–240 | regions 3 and 4, as the rect 60–190 × 80–114 cleared to BG |
| `<` / `>` | Previous / next style, wrapping 40 → 1. If playing, the new pattern starts at the next bar (step 0) so the groove doesn't stumble. | region 2 |
| PARA press | Play / stop. Play starts at step 0 immediately. Stop returns the strip to idle. | regions 6 and 7 |
| VOL− / VOL+ | Drum volume −/+ 10 %, range 0–100 % (one segment per press) | region 8 |
| HOME / header `<` | Leave the screen. Playback keeps running (see NAVIGATION.md). | n/a |

Metronome patterns are just styles. There is no drums on/off state and no click mode.

## 3. Step timing

- One step is a 16th note: **`step_ms = 60000 / BPM / 4`**. At 116 BPM that is 129.3 ms.
- **Take the current step from the drum engine's own sample clock**, not from a UI timer. The UI only polls `current_step` and redraws when it changes. That way the strip can never drift from the audio, and a slow frame just skips a step instead of falling behind.
- A step change only needs region 6 redrawn, the rect 12–228 × 128–136. That's cheap enough to do on every step, even at 240 BPM (one step every 62.5 ms).

## 4. Checklist

- [ ] 5×7 font from `onyx_font5x7.h`, whole-pixel scaling only.
- [ ] Box corners cut as in section 0, with no rounded or anti-aliased corners.
- [ ] Colors as RGB565 words from the table, with no extra blending.
- [ ] Every coordinate taken from section 1, inclusive.
- [ ] The step strip driven by the engine clock.
- [ ] Compared pixel by pixel against `groove_px_3_transport.png` and `_stopped.png`.
