# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
"""Provenance: the firmware and the emulator use only the project's own generated material.

  [1] the generated data in the firmware is exactly what its generator makes:
        src/dsp/drum_kit.inc          tools/gen_drum_kit.py        (the synthesised drum kit)
        src/dsp/drum_patterns.inc     tools/gen_drum_patterns.py   (the rhythm library)
        src/fx/cab_default_table.inc  tools/gen_test_audio.py      (the default cabinet)
      so nobody can paste third-party samples or patterns into them unnoticed;
  [1b] no vendor data (amp blobs, cab IRs, vendor names) in the source tree, built images,
       or fully decoded release packages (.bin and all .fwsc layers) at stride 1;
  [1c] git history, tags, and ALL git objects (reachable or dangling via
       `git cat-file --batch-all-objects` and `git fsck --unreachable`) contain no
       pre-cleanup commits, tags, tainted blobs, or removed paths;
  [2] no firmware, emulator, WASM or test source refers to the third-party material
      these replaced (the extracted drum kit, the imported rhythm set, the V30 table,
      the TONE3000 recordings / IRs / captures);
  [3] the emulator's test files generate and load: six DI phrases, four cabinets, two
      reverbs, the default cab and the A2 test capture.

    python custom-fw/test/test_provenance.py
"""
import glob
import hashlib
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
REPO = os.path.dirname(FW)
fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fails += 1


print("=== [1] the generated files are what their generators make ===")
for tool, extra, what in (("gen_drum_kit.py", [], "src/dsp/drum_kit.inc"),
                          ("gen_drum_patterns.py", [], "src/dsp/drum_patterns.inc"),
                          ("gen_test_audio.py", [], "src/fx/cab_default_table.inc")):
    r = subprocess.run([sys.executable, os.path.join(FW, "tools", tool), "--check", *extra],
                       capture_output=True, text=True)
    check(r.returncode == 0, f"{what} matches tools/{tool}" + ("" if r.returncode == 0 else
                                                             f": {(r.stdout + r.stderr).strip()[-200:]}"))

print("\n=== [1b] no vendor data in the source tree or in the built/release images (stride 1) ===")
sys.path.insert(0, os.path.join(FW, "tools"))
import jl_app_area as _J          # noqa: E402
import lz4blk as _LZ4             # noqa: E402
import pack_ufw as _P             # noqa: E402

check(not os.path.exists(os.path.join(FW, "src", "sys", "stock_assets_embedded.inc")),
      "stock_assets_embedded.inc does not exist")

# All 20 vendor amp names, 20 vendor cab names, and vendor preset/fallback identifiers
VENDOR_NAMES = [
    b"1J900_CL", b"2FD_CL", b"3JC120_CL", b"4BLUE_OD", b"5RAT_OD",
    b"6M-VAVE_OD", b"7SUPA_OD", b"8DARKS_DS", b"9J900_DS", b"10JHS_DS",
    b"11EVH_5150", b"12FRIMAN", b"13XC_DS", b"14ROOM40", b"15JVM",
    b"16AgDb750_BS", b"17ApSVT_BS", b"18PjBuddy_BS", b"19DgXu_BS", b"20MarkLm_BS",
    b"1AC-SeVin", b"2JVM_1960_57", b"3V30_MD421", b"4G12-EVH_CT", b"5HIFI_OK",
    b"6WANGS212_ECM", b"7VOX_AC30", b"8FRMAN112", b"9MeOSick-III", b"10SoldSC412",
    b"11FD_TW1980", b"12MESA_412", b"13HIW412SWF", b"14Recto_112", b"15JC120_BOX",
    b"16Agula410", b"17AgSVT410", b"18DgDG212N", b"19MbSway210", b"20Tace412",
    b"M-VAVE_OD", b"J900", b"JC120", b"RAT_OD", b"DARKS_DS", b"BLUE_OD", b"XC_DS", b"FD_CL",
    b"MV Clean", b"JVM Modern", b"SVT 4x10",
]

# SHA-256 64-bit prefixes of the 20 vendor 3,852-byte amp blobs and 20 vendor 4,096-byte cab IRs
AMP_BLOB_HASHES = {
    "227d2286872d4cac", "c73c4e305e02a7d3", "6272de95370ca219", "61520c89ef6f2d3c", "65e07521ba9e0f0c",
    "c2452d1a01eb2bec", "c29043b3dc7c52bd", "ed9439b6f99d4626", "0aa03be39f3353aa", "ca8ff85aa2781e88",
    "d3e1c80bc6a12952", "d37b7c6a0994fad0", "c892398a48cd1a1e", "0a7d14f33106c200", "14a055aea5662869",
    "e2db2844f0c31c2b", "692568643537e377", "c8789372ce14c605", "81aa15a57915c62b", "9b57082b05820ae4",
}
CAB_IR_HASHES = {
    "f0c08e5250627cff", "bfcdb56895d0442c", "695484ee86a05c26", "cc5af330d4e041c2", "6d25f5ff0b21af3b",
    "38e8ad9959188f76", "b848ea76d78b0a75", "7a9268474e40ee73", "d812d8faf5d311f1", "4d00a6132b3331a9",
    "38bab00f34b8bd60", "79e3ced9fc7c2f6a", "ae4689f14f49b4a6", "1dcbe81ab14f1f54", "21ac1f335d1c029c",
    "dc5bc3038ee061b3", "8a5b26e4c443524b", "03c5816b6abd5464", "c58a5abdc9a2c747", "d1ef7c7c85e6604e",
}

AMP_BLOB_RAW8 = {bytes.fromhex(h) for h in AMP_BLOB_HASHES}
CAB_IR_RAW8 = {bytes.fromhex(h) for h in CAB_IR_HASHES}
_scanned_payload_digests = {}


def _scan_payload(label: str, data: bytes, check_blobs: bool = True):
    found = [v.decode() for v in VENDOR_NAMES if v in data]
    check(not found, f"{label} contains no vendor model/cab/preset names ({', '.join(found[:5]) or 'none'})")
    if check_blobs:
        digest = hashlib.sha256(data).hexdigest()
        if digest in _scanned_payload_digests:
            amp_hits, cab_hits = _scanned_payload_digests[digest]
        else:
            mv = memoryview(data)
            n = len(data)
            sha256 = hashlib.sha256
            amp_hits = sum(1 for i in range(0, max(0, n - 3852 + 1)) if sha256(mv[i:i + 3852]).digest()[:8] in AMP_BLOB_RAW8)
            cab_hits = sum(1 for i in range(0, max(0, n - 4096 + 1)) if sha256(mv[i:i + 4096]).digest()[:8] in CAB_IR_RAW8)
            _scanned_payload_digests[digest] = (amp_hits, cab_hits)
        check(amp_hits == 0 and cab_hits == 0,
              f"{label} (stride-1 scan, {len(data):,} B) contains 0/20 vendor amp blobs and 0/20 vendor cab IRs")


bin_targets = [os.path.join(FW, "build_app.bin")] + sorted(glob.glob(os.path.join(FW, "release", "**", "*.bin"), recursive=True))
for bp in bin_targets:
    if os.path.isfile(bp):
        rel = os.path.relpath(bp, FW)
        _scan_payload(rel, open(bp, "rb").read(), check_blobs=True)

fwsc_targets = sorted(glob.glob(os.path.join(FW, "release", "**", "*.fwsc"), recursive=True))
check(len(fwsc_targets) >= 1, f"release .fwsc package(s) present ({len(fwsc_targets)} found)")
for fp in fwsc_targets:
    if os.path.isfile(fp):
        rel = os.path.relpath(fp, FW)
        raw = open(fp, "rb").read()
        _scan_payload(rel + " [raw .fwsc container]", raw, check_blobs=True)
        ufw, _ = _P.unwrap_fwsc(raw)
        _scan_payload(rel + " [unwrapped .ufw]", ufw, check_blobs=True)
        flash = _J.flash_from_any(raw)
        flash = flash[0] if isinstance(flash, tuple) else flash
        _scan_payload(rel + " [decoded flash image]", flash, check_blobs=True)
        descrambled_area = _J.scramble(flash[0x4000:0x86000], 0x4000)
        _scan_payload(rel + " [descrambled 0x4000..0x86000 app area]", descrambled_area, check_blobs=True)
        area = _J.decode_area(flash)
        app_in_pkg = _J.file_bytes(area, "app.bin").rstrip(b"\xff")
        _scan_payload(rel + " [decoded app.bin]", app_in_pkg, check_blobs=True)
        # Decompress LZ4 update loader (entry 100 in .ufw)
        head = _P.xor_header(ufw[:64])
        n_entries = int.from_bytes(head[8:10], "little")
        ota_entry = None
        for idx in range(n_entries):
            e = _P.xor_header(ufw[64 + idx * 80:64 + (idx + 1) * 80])
            if int.from_bytes(e[0:2], "little") == 100:
                off, size = int.from_bytes(e[8:12], "little"), int.from_bytes(e[12:16], "little")
                ota_entry = ufw[off:off + size]
        if ota_entry is not None:
            body = ota_entry[0x40:]
            loader_raw = b""
            o = 0
            while o + 8 <= len(body):
                clen, dlen = struct.unpack_from("<II", body, o)
                loader_raw += _LZ4.decompress(body[o + 8:o + 8 + clen])
                o += 8 + clen
            _scan_payload(rel + " [decompressed LZ4 loader.bin]", loader_raw, check_blobs=True)

# Verify SHA256SUMS in every release directory
for sums_path in sorted(glob.glob(os.path.join(FW, "release", "**", "SHA256SUMS"), recursive=True)):
    rel_dir = os.path.dirname(sums_path)
    rel_label = os.path.relpath(sums_path, FW)
    lines = [ln.strip() for ln in open(sums_path, encoding="utf-8").read().splitlines() if ln.strip()]
    all_match = len(lines) > 0
    for ln in lines:
        parts = ln.split(None, 1)
        if len(parts) != 2:
            all_match = False
            break
        expected_hash, fname = parts[0].lower(), parts[1].lstrip("*").strip()
        fpath = os.path.join(rel_dir, fname)
        if not os.path.isfile(fpath) or hashlib.sha256(open(fpath, "rb").read()).hexdigest() != expected_hash:
            all_match = False
            break
    check(all_match, f"{rel_label} matches all release files ({len(lines)} entries)")

print("\n=== [1c] git history, tags, and ALL git objects (reachable & dangling) are clean ===")
BANNED_TAGS = {"v1.0.0", "v1.0.1"}
BANNED_OBJ_PREFIXES = (
    # Pre-cleanup commit SHAs
    "aaead80", "903f164", "9020cf1", "a8a5348", "1865a41", "95573b3",
    "1595e09", "ce398a1", "fd49742", "7d5b1b9", "582b219", "d89446e", "bee56f5",
    # Pre-cleanup tainted git blob OIDs (old factory_presets.inc, Onyx-194-app.bin, Pocket_AMP_Onyx_1.01.bin)
    "f97d2193", "e6fd1c64", "a2ab233",
)
BANNED_HIST_PATHS = (
    "import_stock_assets.py", "import_factory_presets.py", "stock_assets_embedded.inc",
    "Pocket_AMP_Onyx_1.01", "Onyx-194", "test_stock_am4.py", "test_factory_am4.py",
    "test_factory_levels.py", "analyze_ampcab_bundles.py", "factory_bundles.py",
    "amx_gru_convert.py", "download_candidates.py",
)
git_dir_check = subprocess.run(["git", "-C", REPO, "rev-parse", "--git-dir"], capture_output=True, text=True)
if git_dir_check.returncode == 0 and git_dir_check.stdout.strip():
    tags_r = subprocess.run(["git", "-C", REPO, "tag", "-l"], capture_output=True, text=True)
    tags = {t.strip() for t in tags_r.stdout.splitlines() if t.strip()}
    bad_tags = sorted(tags & BANNED_TAGS)
    check(not bad_tags, f"git tags contain no pre-cleanup tags ({', '.join(bad_tags) or 'clean'})")

    revs_r = subprocess.run(["git", "-C", REPO, "rev-list", "--objects", "--all"], capture_output=True, text=True)
    obj_lines = revs_r.stdout.splitlines()
    bad_paths = [ln for ln in obj_lines if any(bp in ln for bp in BANNED_HIST_PATHS)]
    check(not bad_paths, f"git rev-list --objects --all contains no removed vendor/importer paths ({len(bad_paths)} found)")

    all_objs_r = subprocess.run(
        ["git", "-C", REPO, "cat-file", "--batch-check=%(objectname) %(objecttype)", "--batch-all-objects"],
        capture_output=True, text=True,
    )
    all_objs = [ln.strip() for ln in all_objs_r.stdout.splitlines() if ln.strip()]
    bad_objs = [ln for ln in all_objs if ln.split()[0].startswith(BANNED_OBJ_PREFIXES)]
    check(not bad_objs,
          f"git cat-file --batch-all-objects ({len(all_objs)} total objects) contains no pre-cleanup commits or tainted blobs ({len(bad_objs)} found)")

    fsck_r = subprocess.run(["git", "-C", REPO, "fsck", "--unreachable", "--no-progress"], capture_output=True, text=True)
    unreachable = [ln.strip() for ln in (fsck_r.stdout + "\n" + fsck_r.stderr).splitlines() if ln.strip()]
    bad_unreach = [ln for ln in unreachable if any(p in ln for p in BANNED_OBJ_PREFIXES)]
    check(not unreachable and not bad_unreach,
          f"git fsck --unreachable reports 0 dangling/unreachable objects ({len(unreachable)} found)")
else:
    print("  info not a git checkout; skipping git history check")

print("\n=== [2] nothing refers to the third-party material that was replaced ===")
# file names / identifiers of what was replaced; matched case-insensitively
FORBIDDEN = [
    r"drum_samples\.inc", r"cab_v30_table", r"k_default_v30_ir",
    r"\bt3k_", r"/api/t3k_ir", r"sample_4x12_v30_ir", r"GE100 ?Pro",
    r"FNDR BFSR", r"MRSH JM50LD", r"BOG UU II",
    r"(?<![\w])rock_riff\.wav", r"(?<![\w])clean_chords\.wav", r"(?<![\w])metal_chug\.wav",
    r"1J900_CL", r"2FD_CL", r"3JC120_CL", r"4BLUE_OD", r"5RAT_OD", r"6M-VAVE_OD",
    r"1AC-SeVin", r"11FD_TW1980", r"12MESA_412", r"14Recto_112", r"15JC120_BOX",
    r"import_stock_assets", r"import_factory_presets",
]
# the replaced files themselves: no longer used by anything; deleted in phase 2 of the
# clean-up (until then they are listed, not failed)
REPLACED = {"drum_samples.inc", "cab_v30_table.inc"}
SCAN_DIRS = ["src", "include", "emu", "wasm", "test", "bench"]
SCAN_EXT = (".c", ".h", ".inc", ".py", ".js", ".html", ".ps1", ".S", ".ld")
SKIP = {os.path.abspath(__file__), os.path.abspath(os.path.join(FW, "src", "sys", "pa_user_embedded.inc"))}
hits, still = [], []
for d in SCAN_DIRS:
    for root, dirs, files in os.walk(os.path.join(FW, d)):
        dirs[:] = [x for x in dirs if x not in ("__pycache__", "out", "node_modules")]
        for fn in files:
            p = os.path.join(root, fn)
            if not fn.endswith(SCAN_EXT) or os.path.abspath(p) in SKIP:
                continue
            if fn in REPLACED:
                still.append(os.path.relpath(p, FW))
                continue
            try:
                text = open(p, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            for pat in FORBIDDEN:
                for m in re.finditer(pat, text, re.I):
                    line = text.count("\n", 0, m.start()) + 1
                    hits.append(f"{os.path.relpath(p, FW)}:{line}: {m.group(0)}")
for h in hits[:20]:
    print("       " + h)
check(not hits, f"no source refers to the replaced material ({len(hits)} references)")
if still:
    print(f"  info still on disk, unused (removed in phase 2): {', '.join(sorted(still))}")

print("\n=== [3] the emulator's test files generate and load ===")
sys.path.insert(0, os.path.join(FW, "tools"))
sys.path.insert(0, os.path.join(FW, "emu"))
import gen_test_audio            # noqa: E402
import nam_loader                # noqa: E402
gen_test_audio.ensure_emu_assets(os.path.join(FW, "emu"), log=lambda s: None)
emu = os.path.join(FW, "emu")
for fn in gen_test_audio.PHRASES:
    p = os.path.join(emu, "samples", fn)
    check(os.path.getsize(p) > 100_000, f"samples/{fn} ({os.path.getsize(p):,} B)")
for name in list(gen_test_audio.CABS) + list(gen_test_audio.REVERBS):
    p = os.path.join(emu, "irs", f"{name}.wav")
    taps, _ = nam_loader.parse_wav_ir(open(p, "rb").read(), max_taps=1024)
    pk = max(abs(t) for t in taps)
    check(len(taps) == 1024 and 0.5 < pk <= 1.0, f"irs/{name}.wav loads ({len(taps)} taps, peak {pk:.2f})")
blob, meta = nam_loader.parse_nam_file(open(os.path.join(emu, "test_capture.nam"), "rb").read(),
                                       "test_capture.nam")
check(len(blob) == 7504 and meta.get("weights_count") == 1871,
      f"test_capture.nam parses as an A2-Lite capture ({len(blob)} B, {meta.get('weights_count')} weights)")

print("\n" + ("PROVENANCE OK" if fails == 0 else f"{fails} FAILURE(S)"))
sys.exit(0 if fails == 0 else 1)
