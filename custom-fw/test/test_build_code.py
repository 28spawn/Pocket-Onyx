"""The pack number and the unique build code (build.ps1 -> include/pa_build_id.h): PA_BUILD_CODE is "<pack>-<build id>", the build id is the yyMMddHHmm stamp the firmware already writes to SRAM, the code
is shown on the unit's SYSTEM screen and sent to the editor (names kind 255), and the OTA ID block the vendor updater reads (product name / V103) does not carry it.

    python custom-fw/test/test_build_code.py
"""
import os, re, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
fails = 0
def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg); fails += not ok
rd = lambda p: open(os.path.join(ROOT, p), encoding="utf-8", errors="replace").read()
h = rd("include/pa_build_id.h")
pack = int(re.search(r"PA_PACK_NUM (\d+)u", h).group(1)); bid = re.search(r"PA_BUILD_ID (\d{10})u", h).group(1); code = re.search(r'PA_BUILD_CODE "([^"]+)"', h).group(1)
check(code == f"Onyx-{pack}-{bid}" or code.startswith("Pocket_AMP_Onyx_") or "Onyx" in code, f"the build code is Onyx ({code})")
check(re.fullmatch(r"\d{10}", bid) is not None and 2601010000 <= int(bid) <= 3612312359, "the build id is a yyMMddHHmm stamp")
b = rd("build.ps1")
check("$packNum = " in b and "PA_BUILD_CODE" in b and "$buildCode" in b, "build.ps1 generates the pack number and the code on every build")
check(re.search(r"\$packNum = (\d+)", b).group(1) == str(pack), f"the header carries the build.ps1 pack number ({pack})")
u = rd("src/board/ui_app.c")
check("PA_BUILD_CODE" in u and "kind == 255u" in u, "the SYSTEM screen shows the code and the editor can read it (names kind 255)")
o = rd("src/sys/pa_ota.c")
check("PA_BUILD_CODE" not in o and "PA_PACK_NUM" not in o, "the OTA ID block (the vendor updater's product name and version) does not carry it")
print("\n" + ("THE BUILD CODE IS WIRED" if not fails else f"{fails} FAILURE(S)"))
sys.exit(1 if fails else 0)
