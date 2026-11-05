# Pocket Amp custom firmware: MIDI map (stock-compatible, with extensions)

Status: DRAFT v1, 2026-11-03. This is a design. Nothing in it is implemented in the firmware except where a line says **[F]**. Nothing was flashed.
Base document: `POCKET_AMP_MIDI_ADDRESS_MAP.md` (the stock map, called "the stock map" below).

Evidence tags used in every table:

| Tag | Meaning |
|---|---|
| **[S]** | from the stock map (derived from the stock firmware); not independently confirmed here |
| **[V]** | verified in this repo: from the stock banks (6 banks, 480 records), from screenshots of the original app (2026-11-03), or by a test |
| **[F]** | the firmware does this today (source line given) |
| **[N]** | new, proposed here |
| **[?]** | open: needs a test or a capture (section 8) |

## 0. Rules

1. **Stock meaning never changes.** Every stock PC, CC, SysEx command and record offset keeps its stock meaning and its stock value range.
2. **New things go only where the stock never goes**: CC numbers the stock map does not list, SysEx command numbers above the stock's 1..48, record bytes that are zero in every stock record we have.
3. **A stock device or editor that meets a new item must be able to ignore it.** A record with extension bytes is still a valid stock record; a new CC or command is simply not answered by stock.
4. **An extension announces itself** (record byte 0x01 has bit 7 set; `EXT_HELLO` answers). A reader that does not see the announcement treats the record as plain stock.
5. **Types beyond the stock lists never replace the stock type byte.** The stock type byte holds the nearest stock model; the real type rides in the extension.
6. **Full fidelity has a second path** that does not squeeze into 92 bytes: the `PA-EXT` SysEx commands (section 3.3).

## 1. Transports (unchanged) [S]

USB-MIDI class device (4-byte event packets) and BLE-MIDI 1.0: service `03B80E5A-EDE8-4B33-A751-6CE34EC4C700`, characteristic `7772E5DB-3868-4112-A1A9-F2669D106BF3` (notify + write without response), CCCD `0x0073`.
Channel: the stock firmware reacts on any channel; so does ours [F] (`usb_ble.c:123`, only `status & 0xF0` is used). Keep it omni.

## 1b. Implementation status (pack 177, 2026-11-04): what the firmware really does

Implemented and covered by `test/test_midi_control.py` (gate F-17, which drives the real handler in `usb_ble.c` and the real tick; USB and BLE share the handler): Program Change; CC 7; CC 20..27 (toggle); **CC 28..35 (set on / off)**; CC 64..67; **CC 80 / 81 (next / previous preset), 85 (tuner), 86 (groove play / stop), 87 (play mode), 88 (SAVE the current sound in the current preset)**; **CC 102..118 (direct knobs)**; **NRPN** (CC 99 / 98, data entry CC 6 high half and optional CC 38 low half; RPN null CC 101 / 100 = 127 clears the selection): modules 0..7 knobs (`0..5` and `0x60..0x65` both address the firmware's NATIVE knob 0..5: the stock-record translation of section 5 is not applied), `0x40` type (>= 0x20 = firmware id + 0x20; below 0x20 is taken as the firmware's id too), `0x41` enable; **module 8 GLOBAL parameters 0..17, module 9 GROOVE 0..4, module 10 PRESET 0 (volume) and 3..6 (stomp map)**.
Modules 8..10 take the raw number D in CC 6 alone when D < 128 (or CC 6 = D >> 7 and CC 38 = D & 127 for a larger one, e.g. the BPM). A change of a general setting marks the settings to be saved (about 2 s later, in the settings log of the flash); the master knob and a MIDI / menu level follow the LAST change (the knob wins only when it moves). CC 88 and the NRPN save need a value >= 64.
NOT implemented, still proposals: NRPN PRESET parameters 1 and 2 (the preset's input trim and gate threshold are stored but not used by the DSP), CC 119 (reserved), SysEx `PA-EXT`. The identity query (OTA frame `F0 00 32 45 00 00 00 40 7F F7`) is now answered over BLE as well as USB.

## 2. Channel messages

### 2.1 Program change [S][F]

`Cn pp`, `pp` = 0..79 selects P01..P80 (`usb_ble.c:124`, range-checked `< 80`). Bank = `pp / 4 + 1`, slot = `A..D` by `pp % 4`. Unchanged.
(The stock map's table has two overlapping rows, PC 32..35 and 35..39; the formula is right: PC 35 is P36, bank 9, slot D.)

### 2.2 Control change

Value rule for every toggle and button: **value >= 64 acts, value < 64 is ignored** [F] (`usb_ble.c:126`).

| CC | Function | Tag | Note |
|---|---|---|---|
| 20 | Toggle FX (DYN) | S | |
| 21 | Toggle AMP | S F | fixed 2026-11-03 (D1): the CC number is translated to the firmware slot through a table, `usb_ble.c` `k_cc_module_slot` |
| 22 | Toggle MOD | S | |
| 23 | Toggle DLY | S | |
| 24 | Toggle REV | S | |
| 25 | Toggle CAB | S | |
| **26** | Toggle DRIVE (new module) | N F | continues the stock block; implemented with D1 |
| **27** | Toggle EQ (new module) | N F | implemented with D1 |
| **28..35** | **Set** module on / off (>= 64 on, < 64 off), in the order FX, AMP, MOD, DLY, REV, CAB, DRIVE, EQ | N | the stock toggles; a controller that must not lose sync needs a set |
| **90 / 91** | BENCH ONLY (pack 159): 90 = a 2 kHz timer interrupt also serves the USB recording endpoint, 91 = the main loop only (the default after every boot). Remove when the USB audio service is settled | N F | `usb_ble.c`, `usb_dev.c` |
| 64 | HOME | S F | |
| 65 | PARA click | S F | |
| 66 | PARA CCW | S F | |
| 67 | PARA CW | S F | |
| **7** | Preset volume, 0..127 -> 0..100 % (same range as the VOL bar) | N | standard "channel volume" number |
| **80** / **81** | Next / previous preset | N | wraps within P01..P80 |
| **85** | Tuner on / off | N | |
| **86** | Groove Station play / stop | N | |
| **87** | Play mode: >= 64 STOMP, < 64 PRESET | N | |
| **102..119** | Direct knobs, 0..127 -> 0..100 % (table below) | N | |
| **98 / 99 / 6 / 38** | NRPN, section 2.3 | N | standard numbers |

Direct knob block (the controls worth a plain CC; everything else is NRPN):

| CC | Knob | CC | Knob | CC | Knob |
|---|---|---|---|---|---|
| 102 | AMP Gain | 108 | DRIVE Drive | 114 | MOD Speed |
| 103 | AMP Level | 109 | DLY Time | 115 | MOD Depth |
| 104 | AMP Bass | 110 | DLY Feedback | 116 | CAB Level |
| 105 | AMP Mid | 111 | DLY Mix | 117 | CAB Low Cut |
| 106 | AMP Treble | 112 | REV Decay | 118 | CAB High Cut |
| 107 | AMP Presence | 113 | REV Mix | 119 | reserved |

All other CC numbers stay unused (the stock map lists none), including 0, 32 (bank select: not needed, 80 presets fit program change), 123.

### 2.3 NRPN (every parameter, full resolution) [N]

`CC 99 = module`, `CC 98 = parameter`, then data entry `CC 6` (high 7 bits) and optionally `CC 38` (low 7 bits). 14-bit value `D = CC6 * 128 + CC38` -> knob percent `round(D * 100 / 16383)`; with CC 6 alone: `round(CC6 * 100 / 127)`.

Module numbers follow the stock record order, so the address is the record's own address:

| Module (CC 99) | Name | Firmware slot | | Module | Name |
|---|---|---|---|---|---|
| 0 | FX | DYNAMICS | | 6 | DRIVE (new) |
| 1 | AMP | AMP | | 7 | EQ (new) |
| 2 | MOD | MOD | | 8 | GLOBAL (new) |
| 3 | DLY | DELAY | | 9 | GROOVE (new) |
| 4 | REV | REVERB | | 10 | PRESET (new) |
| 5 | CAB | CAB | | | |

Parameter (CC 98) for modules 0..7:

| CC 98 | Meaning |
|---|---|
| 0..5 | knob `p0..p5` **exactly as the stock record's slot** (section 5 translates it per type to the firmware knob) |
| 0x40 | model / type. Value `< 0x20`: the stock type id. Value `>= 0x20`: the firmware's own type id plus 0x20 (so every extended type is addressable) |
| 0x41 | enable: >= 64 on |
| 0x60..0x65 | firmware-native knob 0..5, no translation (for new tools) |

Modules 8, 9, 10: parameter numbers are in section 6. Sending a parameter the firmware has no knob for is accepted and stored in the preset (the preset already has six parameters per slot), and has no sound until the DSP implements it.

## 3. SysEx

### 3.1 Frame (unchanged) [S][F]

`F0 <8-to-7 packed: 00 59 cmd len_lo len_mid len_hi payload... chk> F7`, `chk = ~(cmd + len bytes + sum(payload)) & 0xFF`. Replies use the same frame.

### 3.2 Stock commands and what the firmware does with them

| Cmd | Stock function [S] | Firmware today |
|---|---|---|
| 1, 17 | device info / editor handshake | [F] answered (`sysex.c:334`) |
| 18 | hardware descriptor (13 B) | [F] answered |
| 21 | drain ring | [F] answered with the state record |
| 23 | read active state record (8 B) | [F] answered, but the record is **constant** (view 0, preset 0, volumes 50 / 100): it does not report the real state (D2) |
| 24 | read pending block (16 B) | [F] same |
| 25 / 34 / 48 | write session begin / chunk / commit | [F] answered; **a begin with only the u24 length defaults to the NAM target** (`sysex.c:309`), so a stock preset or bank upload cannot work (D3) |
| 35 | binary block read | [F] answered |
| **2, 3, 4, 11, 12, 13, 16** | sub-query, record write, echo, engine ready, global config R/W, ack, live DSP parameter set | **no handler**; they are defined as constants (`pa_protocol.h`) but never dispatched |

The payloads of 2, 3, 12 and 16 are not in our materials. They stay unanswered until a capture of the stock editor (test T5) gives their format. **No guessed implementation.**

### 3.3 New commands: family `PA-EXT` [N]

Command numbers `0x40..0x4F` (the stock uses 1..48). Reply command = `0x80 | cmd`, the convention of the existing diagnostic family. Same frame, same checksum, same 8-to-7 packing.

| Cmd | Name | Request payload | Reply payload |
|---|---|---|---|
| 0x40 | `EXT_HELLO` | none | `"PAX1"`, protocol version u8, build id u32, capability bits u32 |
| 0x41 | `EXT_PRESET_GET` | preset index u8 | the 164-byte firmware record (`pa_preset_t`), name included |
| 0x42 | `EXT_PRESET_PUT` | index u8, 164-byte record | status u8 (0 = stored) |
| 0x43 | `EXT_PARAM_SET` | module u8, param u8 (as CC 98), value u16le (14-bit as in 2.3) | status u8 |
| 0x44 | `EXT_PARAM_GET` | module u8, param u8 | value u16le |
| 0x45 | `EXT_NAME` | index u8, op u8 (0 get, 1 set), name (24 bytes) | the name |
| 0x46 | `EXT_SETTINGS` | op u8 (0 get, 1 set), setting id u8 (section 6), value i32le | the value |
| 0x47 | `EXT_STATE` | none | live state: preset index u8, preset volume u8, module-enable mask u8 (bit n = module n), play mode u8, tuner on u8, groove playing u8 |
| 0x48 | `EXT_BANK_GET` / `PUT` | op u8, then (for put) the 7,370-byte bank | the bank: 80 stock records with extensions + `PATCHEND` trailer |
| 0x49..0x4F | reserved | | |

Capability bits (`EXT_HELLO`): bit 0 PA-EXT record extension (section 4.3), bit 1 NRPN, bit 2 direct CC block, bit 3 drive + EQ slots, bit 4 user NAM, bit 5 groove, bit 6 diag family.

### 3.4 Families that stay as they are

| Family | Frame | Status |
|---|---|---|
| Diagnostic | `F0 7D <cmd> ...`: 01 flash read, 02 RAM read, 03 profile (reply `0x80|cmd`) | [F] `pa_diag.h`; not part of this map, never used by the editor |
| JieLi OTA | `F0 00 32 45 ...` query, `F0 22 24 35 ...` upgrade | [F] `pa_ota.c`; unchanged |

## 4. The preset record (92 bytes) and its free bytes

### 4.1 The stock record, as verified

| Offset | Field | Evidence |
|---|---|---|
| 0x00 | preset volume 0..100 (the app's VOL bar) | [V] 60 in 39 of 40 presets, matches the app |
| 0x01 | reserved, 0 | [V] 0 in all 480 records |
| 0x02..0x07 | chain order: the six module codes in chain order: 0 FX, 1 AMP, 2 MOD, 3 DLY, 4 REV, 5 CAB | [S]; [V] presets 1, 33, 37 match the chain the app draws; **preset 26 does not** (bytes put MOD before AMP, the app draws AMP before MOD) [?] |
| 0x08..0x0D | enable, six modules | [V] |
| 0x0E..0x13 | type, six modules | [V] |
| 0x14.. | six `u16le` slots per module, 12 bytes each: FX 0x14, AMP 0x20, MOD 0x2C, DLY 0x38, REV 0x44, CAB 0x50 | [V] |
| trailer | 10 bytes after the 80 records: `PATCHEND` + 2 bytes | [V] **the last byte is 0 in stock-shipped banks and `E` (0x45) in banks the app writes**: a reader must accept both (D7) |

Stock slot meanings, corrected by the screenshots [V] (the stock map had the AMP order right; the DLY Dual row and a few others differ, section 5):

AMP: Gain, **Level**, Bass, Mid, Treble (slot 5 unused). CAB: Level, Low Cut, High Cut. REV (Room, Hall): Decay, Mix, HPass, LPass, Depth. DLY Dual: Time, Fb, Mix, Grit, Speed, Depth. MOD Vibrato: Speed, Depth.

### 4.2 Which bytes are free (proof)

`tools/midi_map_check.py` scans every stock bank it can find (AM3, AM4, AM4 speaker mode, the tour bank, the two custom banks, your export: 6 distinct banks, **480 records**) and reports:

| Bytes | Offsets | Non-zero in 480 records |
|---|---|---|
| reserved byte | 0x01 | **0** |
| AMP slot 5 | 0x2A, 0x2B | **0** |
| MOD slot 5 | 0x36, 0x37 | **0** |
| REV slot 5 | 0x4E, 0x4F | **0** |
| CAB slots 3, 4, 5 | 0x56..0x5B | **0** |
| **FX slots 4, 5** | 0x1C..0x1F | **6: not free**. The custom banks use FX type 4 with slot 5 = 80 (D8) |
| MOD slot 4 | 0x34, 0x35 | 15: not free (Autofilter and stale values) |
| the high byte of every parameter slot | odd offsets from 0x15 | 0 everywhere, but see tier 2 below |

**13 bytes are free: 1 + 6 low bytes (tier 1) + 6 high bytes (tier 2).**

- **Tier 1**: the low byte of a slot the stock models never use, value 0..100, so the 16-bit slot stays within the range the stock app itself writes. Expected to be safe.
- **Tier 2**: the high byte of such a slot. The 16-bit slot then reads `value x 256`, above 100. Safe only if the stock app and device accept it (test T1). If T1 fails, tier 2 simply is not used and those fields fall back to defaults; tier 1 is unaffected.

### 4.3 PA-EXT v1: what goes into the 13 bytes [N]

Reference encoder / decoder and the round-trip test: `tools/midi_map_check.py` (5,000 random records: the extension comes back exactly and no other byte changes; no stock record reads as extended).

| Offset | Tier | Field | Encoding |
|---|---|---|---|
| 0x01 | | header | bit 7 = extension present; bits 6..5 = version (1); bits 4..0 = `dly_ext + 5 x rev_ext + 15 x dyn_ext` (max 29) |
| 0x2A | 1 | AMP presence | 0..100 |
| 0x2B | 2 | per-preset input trim | `50 + dB`, -12..+12 dB -> 38..62 |
| 0x36 | 1 | DRIVE enable and type | `50 x enable + type`, type 0..5 |
| 0x4E | 1 | EQ enable and type | `50 x enable + type`, type 0..2 |
| 0x56 | 1 | DRIVE drive | 0..100 |
| 0x58 | 1 | DRIVE tone | 0..100 |
| 0x5A | 1 | DRIVE level | 0..100 |
| 0x37 / 0x4F / 0x57 / 0x59 / 0x5B | 2 | EQ knobs 0..4 | 0..100 (a 3-knob or 4-knob EQ type leaves the rest 50) |

Extended-type codes in the header, with the stock type written into the stock type byte:

| Module | Extended type (header code) | Stock type byte holds |
|---|---|---|
| DLY (`dly_ext` 1..4) | DIGITAL, PING PONG, REVERSE, SLAPBACK | 0 Analog (DIGITAL, REVERSE, SLAPBACK), 3 Dual (PING PONG) |
| REV (`rev_ext` 1..2) | PLATE, AMBIENCE | 1 Hall (PLATE), 0 Room (AMBIENCE) |
| FX/DYN (`dyn_ext` 1) | COMP without gate | 2 Compress |

What does **not** fit in 92 bytes and travels only through `PA-EXT` (section 3.3): preset name, user NAM and user IR selection (the amp / cab type byte then holds stock amp / cab 0), BPM, genre tag, stomp map, gate threshold, the position of DRIVE and EQ in the chain (rule: DRIVE directly before AMP, EQ directly after CAB), DLY slots 4 / 5 for types other than the four listed, and the exact Q16 values.

## 5. Module tables: stock id and knob -> firmware

Scaling: stock knob 0..100 -> firmware knob percent 1:1 unless a note says otherwise. "-" = no firmware knob (the value is stored in the record's slot and ignored by the DSP).

### FX (stock) -> DYN (firmware slot 0)

| Stock type [S] | Firmware type | Stock slots -> firmware knobs |
|---|---|---|
| 0 Noise Gate++ | 0 GATE | p0 Gate -> THRESH |
| 1 Boost | 1 BOOST+GATE | p0 Gate -> THRESH; p1 Gain -> BOOST (knob 3) |
| 2 Compress | 2 COMP+GATE | p0 -> THRESH; p1 Sustain -> RATIO; p2 Attack -> ATTACK; p3 Level -> LEVEL (stock 50 = unity, firmware x2: importer rule [F]) |
| 3 AI Gate MS | 3 AI GATE MS | p0 Gate -> THRESH; p1 Bias -> - |
| (custom banks) 4 | none | unknown model using slot 5 (D8); leave DYN off |
| firmware only | 4 COMP | extension `dyn_ext` = 1 |

### AMP (slot 2) [V]

Types 0..19 identical (stock baseline models). User NAMs 20..39 are ours: stock type byte 0, selection through `PA-EXT`.

| Stock slot | Stock label | Firmware knob |
|---|---|---|
| p0 | Gain | GAIN |
| p1 | Level | MASTER (1:1; **importer rule today**: not copied, loudness set by calibration; when the record carries the extension, Level := MASTER) |
| p2 | Bass | BASS |
| p3 | Mid | MID |
| p4 | Treble | TREBLE |
| p5 | unused | PRES via extension byte 0x2A |

### CAB (slot 3) [V]

Types 0..19 identical. p0 Level -> LEVEL. p1 Low Cut -> LOWCUT (both `20 + 2.8 x v` Hz, i.e. the same 20..300 Hz linear law). p2 High Cut -> HICUT **inverted** (`fw = 100 - stock`; stock 100 = open = 18 kHz).

### MOD (stock) -> MOD (firmware slot 5)

| Stock type [S] | Stock id | Firmware id | Stock knobs -> firmware knobs |
|---|---|---|---|
| Chorus | 0 | 0 | Speed, Depth, Mix -> RATE, DEPTH, MIX |
| Phaser | 1 | 1 | stock lists Speed, MidCut, Resonance, Feedback; firmware has RATE, DEPTH, MIX, FDBK: **names differ, [?]** |
| Tremolo | **2** | **3** | Speed, Depth -> RATE, DEPTH; stock Level -> - |
| Flanger | **3** | **2** | Speed, Depth, **Feedback, Mix** -> RATE, DEPTH, **FDBK, MIX** (p2 and p3 swapped) |
| Vibrato | 4 | 4 | Speed, Depth [V] |
| Univibe | 5 | 5 | Speed, Depth, Mix |
| Autofilter | 6 | 6 | Speed, Min, Max, Mix, Feedback -> RATE, RANGE, MIX, RESO (Min/Max and RESO only approximate) |

The stock map numbers Tremolo 2 and Flanger 3; the firmware has them the other way round. The bank favours the stock map (preset 28, type 2: `55, 50, 50, 0` reads as a tremolo with level 50; as a flanger it would have mix 0). Only **presets 25 and 28** are affected. **[?] test T4** settles it; until then the importer's index-for-index copy is suspect (D4, D5).

### DLY (slot 6)

Types 0 Analog, 1 Ducking, 2 dTape, 3 Dual, 4 Lofi are the same ids as the firmware's ANALOG, DUCKING, TAPE, DUAL, LO-FI. p0 Time, p1 Feedback, p2 Mix map to TIME, FDBK, MIX.

| Type | stock p3 | stock p4 | stock p5 | Firmware aux knob (knob 3) |
|---|---|---|---|---|
| Analog | Phaser Amt [S] | Detune [S] | - | MOD [?] |
| Ducking | Unpack Dynamic [S] | Chorus Speed [S] | Chorus Depth [S] | RELEASE [?] |
| dTape | Grit [S] | Speed [S] | Depth [S] | GRIT (p3 only) |
| **Dual** | **Grit [V]** | **Speed [V]** | **Depth [V]** | RATIO: **different control**; Grit has no firmware knob |
| Lofi | Grit [S] | Chorus Speed [S] | Chorus Depth [S] | GRIT (p3 only) |

The stock map's Dual row says p3 = "TimeMode (Lag)"; the app shows Grit [V] (D6). Stock p4 / p5 (Speed, Depth) have no firmware knob: stored, no sound. 18 of the 21 presets that use delay use Dual.

### REV (slot 7) [V for Room and Hall]

Types 0..5 Room, Hall, Swell, Spring, Shimmer, Cloud are the same ids as the firmware's first six.

| Stock slot | Stock label | Firmware knob |
|---|---|---|
| p0 | Decay | DECAY |
| p1 | Mix | MIX (knob 3) |
| p2 | HPass | LOWCUT (knob 1) |
| p3 | LPass | TONE (knob 2); the two laws are not compared [?] |
| p4 | Depth (Room, Hall; other types per the stock map) | - |

### DRIVE (slot 1) and EQ (slot 4): new modules [N]

Types: DRIVE 0 TS-808, 1 KLON, 2 OCD, 3 RAT, 4 BIG MUFF, 5 FUZZ FACE; knobs p0 DRIVE, p1 TONE, p2 LEVEL. EQ 0 5-BAND (5 knobs: 100 Hz, 400 Hz, 1 kHz, 3 kHz, 8 kHz), 1 LOW/HIGH CUT (LOWCUT, HICUT, LEVEL), 2 PARAMETRIC-3 (LOW, MID, FREQ, HIGH). In a record they live in the extension bytes (4.3); over NRPN they are modules 6 and 7 with parameters 0..4.

## 6. Modules 8, 9, 10 (NRPN) and `EXT_SETTINGS` ids [N]

Source: `pa_settings_t` [F] (`pa_settings.h`) and the Groove Station state. `EXT_SETTINGS` uses the same id as the NRPN parameter.

| Module | Param | Setting | Range / encoding |
|---|---|---|---|
| 8 GLOBAL | 0 | input gain | -12..+12 dB, value `D = dB + 12` (0..24) |
| | 1 | input gate | -80 (off) .. -30 dB, `D = dB + 80` (0..50) |
| | 2 | output level | 0..100 % |
| | 3 | output type | 0 phones, 1 line |
| | 4 | cab on line | 0 / 1 |
| | 5 | output limiter | 0 brickwall, 1 soft, 2 off |
| | 6 | fizz filter | 0..3 |
| | 7 | stereo width | 0..100 |
| | 8 | output routing | 0 stereo, 1 mono, 2 left, 3 right, 4 swap |
| | 9 | balance | -50..+50, `D = v + 50` |
| | 10 | global EQ on | 0 / 1 |
| | 11..15 | global EQ bands | -12..+12 dB, `D = dB + 12` |
| | 16 | tuner reference | 430..450 Hz, `D = Hz - 430` |
| | 17 | tuner mute | 0 / 1 |
| | 18 | USB loopback (the recording includes what the PC plays: the Settings row, factory ON) | 0 / 1 |
| | 19 | USB audio mode (an audio interface's direct monitoring): 0 AMP, 1 INTERFACE (output = the PC's audio only, the recording is the dry guitar), 2 INTERFACE + DIRECT MONITOR (the dry guitar is also heard) | 0 / 1 / 2 |
| 9 GROOVE | 0 | play | 0 / 1 |
| | 1 | BPM | 40..260, `D = bpm - 40` |
| | 2 | pattern | 0..39 |
| | 3 | volume | 0..100 |
| | 4 | drums on | 0 / 1 |
| 10 PRESET | 0 | preset volume | 0..100 |
| | 1 | input trim | -12..+12 dB, `D = dB + 12` |
| | 2 | gate threshold | 0..100 % |
| | 3..6 | stomp map (VOL-, VOL+, <, >) | module number 0..7 |
| | 7 | chain move (the editor's drag and drop) | `D = slot * 8 + position`: slot id 0..7 (DYN 0, DRIVE 1, AMP 2, CAB 3, EQ 4, MOD 5, DLY 6, REV 7), position 0..7 in the signal order; the others shift as on the unit's own move. Ignored while the unit is in its move mode. Not saved until CC 88 |

In module 8 to 10 the 14-bit value is the **raw number `D`** from the table, not a percentage.

## 7. Compatibility findings (what is wrong or missing today)

| # | Finding | Effect | Where |
|---|---|---|---|
| **D1 (fixed)** | CC 20..27 toggled the firmware slot `CC - 20`, but our slot order is DYN, DRIVE, AMP, CAB, EQ, MOD, DLY, REV | a stock foot controller's CC 21 (AMP) toggled the drive, CC 22 (MOD) the amp, CC 23 (DLY) the cab, CC 24 (REV) the EQ, CC 25 (CAB) the mod. Now translated through `k_cc_module_slot`; test `rx_pedal_module_ccs_follow_the_stock_module_order` (failed before the fix: "CC 21 (AMP) toggles firmware slot 1 (expect 2)") | `usb_ble.c`, `test/test_usb_midi_e2e.c` |
| **D2** | state record (cmd 23 / 24) is constant | an editor that reads it sees preset 0, volumes 50 / 100 whatever the unit does | `sysex.c:157` |
| **D3** | preset upload accepts only the 164-byte "PMP2" record, and a begin with only the u24 length is taken as a NAM upload | the stock editor's preset / bank import cannot work | `sysex.c:192`, `sysex.c:309` |
| **D4** | MOD type ids 2 and 3 are swapped between the stock map and the firmware | presets 25 and 28 may have tremolo and flanger exchanged [?] | `ui_app.c:186` |
| **D5** | Flanger knobs p2, p3 are Feedback, Mix in the stock map, MIX, FDBK in the firmware | preset 25 if D4 is confirmed | same |
| **D6** | the stock map's DLY Dual p3 is "TimeMode (Lag)"; the app shows Grit | map error, corrected here | stock map |
| **D7** | banks written by the app end `PATCHEND\0E`; stock-shipped banks end `PATCHEND\0\0` | tools must accept both trailers | `midi_map_check.py` |
| **D8** | FX type 4 exists in the app's custom banks (slot 5 in use) | unmapped; FX slots 4, 5 are not free | custom banks |
| **D9** | stock Level (amp p1) and cab Level are real knobs the factory import does not copy | loudness is set by calibration instead | `preset.c` |

## 8. Tests that need your stock hardware or app

| Test | What | Why |
|---|---|---|
| **T1** | write a bank whose records have tier-2 bytes set (for example `0x2B = 50`) and import it with the stock app, then to a stock unit if you have one | do the stock app and device accept a 16-bit slot above 100? Decides tier 2 |
| **T2** | export the bank again from the stock app after T1 | does the app keep the unknown bytes or rewrite them? (If rewritten, extensions do not survive a round trip through the app; nothing breaks.) |
| **T3** | the same with only tier 1 and byte 0x01 | the minimum guarantee |
| **T4** | screenshots of the MOD page for presets **25** and **28** | settles D4 / D5 |
| **T5** | a capture of the stock app importing and exporting presets, and turning one knob, with the stock unit connected (`tools/usb_etw_capture.ps1` exists for the USB side) | gives the payloads of cmd 2, 3, 12, 16 and the stock upload target; without it cmd 2 / 3 / 12 / 16 stay unanswered |

## 9. Implementation backlog (not started, each needs approval; none touches the Bluetooth link code)

1. ~~Fix D1~~ done 2026-11-03: the stock CC is translated to the firmware slot (an 8-entry table), CC 26 and 27 added; one host test.
2. Replace the constant state record with the real state; add `EXT_STATE`.
3. Add the CC block of 2.2 (28..35, 7, 80, 81, 85..87, 102..118) and NRPN (2.3).
4. Add `PA-EXT` commands (3.3), starting with `EXT_HELLO`, `EXT_PARAM_*`, `EXT_PRESET_*`.
5. Add the stock-bank path: accept a 7,370-byte bank (and the app's trailer), write and read PA-EXT v1 (reference code: `tools/midi_map_check.py`).
6. Resolve D4 / D5 after T4; add the type and knob translation tables of section 5 to the firmware and the importer.
7. Host tests: a table-driven test that every CC and NRPN reaches the intended firmware control; the record round trip of 4.3 as a C test.
