#!/usr/bin/env python3
"""custom-fw/tools/get_sdk_files.py - fetch the three JieLi SDK files an Onyx package is built from (tools/onyx_pkg_make.py): uboot.boot, cfg_tool.bin, cfg/eq_cfg_hw.bin.

They are from the public JieLi AC79 SDK (Apache-2.0, https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK, the commit NOTICE.md names), not from M-VAVE, and are not stored in this repository: this tool
downloads them into third_party/jieli_sdk_files/ (ignored by git) and checks each against the SHA-256 recorded here. Ship LICENSES/Apache-2.0.txt with every package.

    python custom-fw/tools/get_sdk_files.py            fetch what is missing, verify, print the folder
"""
import hashlib, sys, urllib.request
from pathlib import Path

FW = Path(__file__).resolve().parent.parent
DEST = FW / "third_party" / "jieli_sdk_files"
COMMIT = "5fa84a9b2f723a5e9337ad79dc2ad741fc98fe2d"
BASE = f"https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK/raw/{COMMIT}/cpu/wl82/tools/"
FILES = {
    "uboot.boot": "64ccc63e4df3238046fadb200f1b134311d8fe6ec6e3ccb8c8b81379231b867e",
    "cfg_tool.bin": "276579954f076886a6a7694f65dc71c034a63a2c204b76749065c0ac7b010d1b",
    "cfg/eq_cfg_hw.bin": "179ac9f4501d2cf8c3bdbc06fcda1e46228b7ace7e339342c767bb6568de57d8",
}


def main():
    bad = 0
    for rel, want in FILES.items():
        path = DEST / rel
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            try:
                path.write_bytes(urllib.request.urlopen(BASE + rel, timeout=60).read())
            except Exception as e:
                print(f"{rel}: download failed ({e}); fetch it by hand from {BASE}{rel}")
                bad += 1
                continue
        got = hashlib.sha256(path.read_bytes()).hexdigest()
        if got != want:
            print(f"{rel}: SHA-256 {got} is not the recorded {want}: refusing it")
            path.unlink()
            bad += 1
        else:
            print(f"ok  {rel} ({path.stat().st_size} B)")
    if bad:
        return 1
    print(DEST)
    return 0


if __name__ == "__main__":
    sys.exit(main())
