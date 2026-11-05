#!/usr/bin/env python3
"""
custom-fw/tools/pa_import.py - link your own NAM captures and cabinet IRs into the firmware image

The device cannot write its own flash yet, so imported content travels inside the app image: this tool converts
the files on the PC and writes src/sys/pa_user_embedded.inc, which src/sys/stock_assets.c includes. Rebuild
(build.ps1 / qa_gate.py) and flash the pack as usual; the captures show up after the 20 stock amps (AMP 21...) and
the IRs after the 20 stock cabinets.

    custom-fw/user_content/nam/*.nam      NAM captures (WaveNet / LSTM / M-VAVE containers: emu/nam_loader.py decides)
    custom-fw/user_content/cab/*.wav      cabinet impulse responses (any rate / bit depth, first channel is used)

    python custom-fw/tools/pa_import.py                 convert everything in user_content/ and write the .inc
    python custom-fw/tools/pa_import.py --list          show what is in the current .inc inputs without writing
    python custom-fw/tools/pa_import.py --clear         remove the .inc (back to the 20 stock amps and cabinets)

What it does to an IR: mono (first channel), resampled to 44.1 kHz, the silence before the first
peak trimmed (8 samples kept), cut to 1,024 taps with a short fade if it was longer, then scaled so its energy
(L2 norm) is the reference cabinet's, CAB_REF_L2_Q18 = 400150 in Q18 - the rule fx_cab_ir.c applies to every user IR,
so one cabinet is as loud as another.

The files in user_content/ and the generated .inc are local user content.
"""
import argparse
import binascii
import os
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FW = HERE.parent
sys.path.insert(0, str(FW / "emu"))
import nam_loader  # noqa: E402
from nam_loader import parse_nam_file, parse_riff_wav, parse_wav_ir  # noqa: E402

INC = FW / "src" / "sys" / "pa_user_embedded.inc"
CONTENT = FW / "user_content"
MAX_NAM = 20                  # registry slots (PA_USER_NAM_MAX_COUNT)
MAX_CAB = 40                  # imported cabinets; the UI index is a byte (20 stock + these)
CAB_TAPS = 1024
CAB_REF_L2_Q18 = 400150.0
APP_SLOT_MAX = 0x81CD9        # bytes available for app.bin (pack_ufw.APP_SLOT_MAX: the app image ends at 0x85FFF, the user store starts at 0x86000)
NAME_LEN = 23


def clean_name(text: str, fallback: str) -> str:
    t = re.sub(r"[^A-Z0-9 _\-+.]", "", Path(text).stem.upper().replace("_", " "))
    t = re.sub(r"\s+", " ", t).strip()
    return (t or fallback)[:NAME_LEN]


def c_bytes(data: bytes) -> str:
    rows = []
    for i in range(0, len(data), 16):
        rows.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
    return "\n".join(rows)


def convert_nam(path: Path):
    raw = path.read_bytes()
    blob, meta = parse_nam_file(raw, path.name)
    is_wave = blob[:4] == b"WAVE"
    if not is_wave and len(blob) != 3852:
        raise ValueError(f"unexpected blob size {len(blob)} (a GRU blob is 3,852 bytes)")
    knobs = [max(0, min(100, int(k))) for k in meta.get("amp_knobs", [50] * 6)]
    makeup = float(meta.get("makeup_db", 0.0) or 0.0)
    return {
        "name": clean_name(meta.get("name") or path.name, "NAM"),
        "type": 1 if is_wave else 0,
        "voicing": int(meta.get("voicing_id", 0) or 0),
        "has_cab": 1 if meta.get("is_amp_cab") else 0,
        "flags": 1 if meta.get("approximate") else 0,
        "makeup_x4": max(-128, min(127, int(round(makeup * 4)))),
        "knobs": (knobs + [50] * 6)[:6],
        "head_scale": float(meta.get("head_scale", 1.0) or 1.0),
        "input_trim": float(meta.get("input_trim", 1.0) or 1.0),
        "blob": blob,
        "info": f'{meta.get("engine", "GRU")} {meta.get("architecture", "")}'.strip(),
    }


def convert_cab(path: Path):
    import numpy as np
    raw = path.read_bytes()
    info = parse_riff_wav(raw)
    sr = int(info.get("sample_rate", 44100))
    # up to 1 s of the file is looked at: enough for any cabinet, and a resample of a long file stays cheap
    taps, meta = parse_wav_ir(raw, max_taps=max(sr, 1))
    x = np.asarray(taps, dtype=np.float64)
    if x.size == 0:
        raise ValueError("empty IR")
    if sr != 44100:
        try:
            from scipy.signal import resample_poly
            from math import gcd
            g = gcd(sr, 44100)
            x = resample_poly(x, 44100 // g, sr // g)
        except ImportError:
            n = int(round(x.size * 44100.0 / sr))
            x = np.interp(np.linspace(0, x.size - 1, n), np.arange(x.size), x)
    peak = float(np.max(np.abs(x))) or 1.0
    onset = int(np.argmax(np.abs(x) >= 0.05 * peak))
    x = x[max(0, onset - 8):]
    truncated = x.size > CAB_TAPS
    x = x[:CAB_TAPS]
    if truncated:
        n = 128
        x[-n:] *= 0.5 * (1.0 + np.cos(np.linspace(0, np.pi, n)))
    q = x * 262144.0
    l2 = float(np.sqrt(np.sum(q * q)))
    if l2 <= 0.0:
        raise ValueError("silent IR")
    q = np.rint(q * (CAB_REF_L2_Q18 / l2)).astype(np.int64)
    if int(np.max(np.abs(q))) >= (1 << 23):
        print(f"  warning: {path.name} has a very sharp peak ({int(np.max(np.abs(q)))} in Q18); it may clip the cabinet stage")
    taps_q18 = [int(v) for v in q]
    return {
        "name": clean_name(path.name, "CAB"),
        "taps": taps_q18,
        "info": f"{info.get('sample_rate')} Hz, {info.get('bits', info.get('bit_depth', '?'))} bit, {len(taps)} taps read"
                + (", resampled" if sr != 44100 else "") + (", cut to 1024 with a fade" if truncated else ""),
    }


def emit(nams, cabs, allow_wavenet=False) -> str:
    out = ["/*",
           " * src/sys/pa_user_embedded.inc - user NAM captures and cabinet IRs linked into the image",
           " *",
           " * GENERATED FILE - do not edit. Regenerate with",
           " *     python custom-fw/tools/pa_import.py",
           " * from custom-fw/user_content/. Included by src/sys/stock_assets.c.",
           " */",
           f"#define PA_EMB_NAM_N {len(nams)}u",
           f"#define PA_EMB_CAB_N {len(cabs)}u"]
    if allow_wavenet:
        out.append("#define PA_EMB_ALLOW_WAVENET 1      /* --allow-wavenet: the firmware loads WaveNet captures although they overrun the block */")
    out.append("")
    for i, n in enumerate(nams):
        out.append(f"/* NAM {i:02d}: {n['name']} ({len(n['blob']):,} bytes, {n['info']}) */")
        out.append(f"static const uint8_t s_emb_nam_{i:02d}_blob[{len(n['blob'])}] __attribute__((aligned(4))) = {{")
        out.append(c_bytes(n["blob"]))
        out.append("};")
    out.append("static const pa_emb_nam_t s_emb_nam[%d] = {" % max(1, len(nams)))
    for i, n in enumerate(nams):
        k = ", ".join(str(v) for v in n["knobs"])
        crc = binascii.crc32(n["blob"]) & 0xFFFFFFFF
        out.append(f'    {{ "{n["name"]}", {n["type"]}, {n["voicing"]}, {n["has_cab"]}, {n["flags"]}, {n["makeup_x4"]}, {{ {k} }}, '
                   f'{n["head_scale"]!r}f, {n["input_trim"]!r}f, {len(n["blob"])}u, 0x{crc:08x}u, s_emb_nam_{i:02d}_blob }},')
    if not nams:
        out.append("    { 0 },")
    out.append("};\n")
    for i, c in enumerate(cabs):
        out.append(f"/* CAB {i:02d}: {c['name']} ({c['info']}) */")
        out.append(f"static const int32_t s_emb_cab_{i:02d}_taps[{CAB_TAPS}] __attribute__((aligned(4))) = {{")
        rows = [", ".join(str(v) for v in c["taps"][j:j + 12]) + "," for j in range(0, CAB_TAPS, 12)]
        out.append("    " + "\n    ".join(rows))
        out.append("};")
    out.append("static const pa_emb_cab_t s_emb_cab[%d] = {" % max(1, len(cabs)))
    for i, c in enumerate(cabs):
        out.append(f'    {{ "{c["name"]}", {CAB_TAPS}u, s_emb_cab_{i:02d}_taps }},')
    if not cabs:
        out.append("    { 0 },")
    out.append("};")
    return "\n".join(out) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nam-dir", default=str(CONTENT / "nam"))
    ap.add_argument("--cab-dir", default=str(CONTENT / "cab"))
    ap.add_argument("--out", default=str(INC), help="where to write the .inc (tests use this)")
    ap.add_argument("--allow-wavenet", action="store_true", help="link WaveNet captures and let the firmware load them (about 2x too slow on the unit: a block takes 3.1 ms and core 1 gets switched off - experiments only)")
    ap.add_argument("--ref-dbu", type=float, default=nam_loader.ADC_REF_DBU,
                    help="the unit's ADC full scale in dBu (default %(default)s, an assumption); a capture's input trim is 10 ** ((this - its level) / 20): raise it for a hotter input, e.g. 15 gives trim 1.0 for a 15 dBu capture")
    ap.add_argument("--list", action="store_true", help="convert and report, write nothing")
    ap.add_argument("--clear", action="store_true", help="delete the generated .inc")
    a = ap.parse_args()

    if a.clear:
        if INC.exists():
            INC.unlink()
            print(f"removed {INC}")
        else:
            print("nothing to remove")
        return 0

    nam_loader.ADC_REF_DBU = a.ref_dbu
    nams, cabs, failed = [], [], 0
    nd, cd = Path(a.nam_dir), Path(a.cab_dir)
    for p in sorted(nd.glob("*.nam")) if nd.is_dir() else []:
        try:
            n = convert_nam(p)
            print(f"NAM  {p.name:40s} -> {n['name']:23s} {'WaveNet' if n['type'] else 'GRU':8s} {len(n['blob']):6,d} B  {n['info']}")
            nams.append(n)
        except Exception as e:  # noqa: BLE001
            failed += 1
            print(f"NAM  {p.name}: SKIPPED - {e}")
    for p in sorted({q.resolve(): q for q in list(cd.glob("*.wav")) + list(cd.glob("*.WAV"))}.values()) if cd.is_dir() else []:
        try:
            c = convert_cab(p)
            print(f"CAB  {p.name:40s} -> {c['name']:23s} {CAB_TAPS * 4:6,d} B  {c['info']}")
            cabs.append(c)
        except Exception as e:  # noqa: BLE001
            failed += 1
            print(f"CAB  {p.name}: SKIPPED - {e}")
    if len(nams) > MAX_NAM:
        print(f"only the first {MAX_NAM} captures fit the registry; dropping {len(nams) - MAX_NAM}")
        nams = nams[:MAX_NAM]
    if len(cabs) > MAX_CAB:
        cabs = cabs[:MAX_CAB]

    used = sum(len(n["blob"]) for n in nams) + sum(CAB_TAPS * 4 for _ in cabs)
    app = FW / "build_app.bin"
    print(f"\n{len(nams)} captures, {len(cabs)} cabinets, {used:,} bytes of data")
    if app.exists():
        base = app.stat().st_size
        # build_app.bin already holds the previous .inc, so the free space is only an estimate
        print(f"the last build's app image is {base:,} bytes of {APP_SLOT_MAX:,}; the data above is about {used:,} of the {APP_SLOT_MAX - base:,} that were free then")
    if a.list:
        return 0
    if not nams and not cabs:
        print("nothing converted: put .nam files in custom-fw/user_content/nam and .wav files in custom-fw/user_content/cab")
        return 1 if failed else 0
    out = Path(a.out)
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(emit(nams, cabs, a.allow_wavenet))
    print(f"wrote {out}\nnow rebuild: python custom-fw/tools/qa_gate.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
