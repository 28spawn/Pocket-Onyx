"""The firmware's half of the vendor MIDI update (src/sys/pa_ota_session.c) on host, against real packages and a simulated flash (test/test_ota_session.c): the loader of the package is staged byte for byte, the
record has the SPL's markers, nothing outside the five staging sectors is written, a host without "success" commits nothing, damaged packages are refused, the boot clean-up touches only a staging area.

    python custom-fw/test/test_ota_session.py [package.fwsc ...]      (default: the original M-VAVE file if it is in ~/Downloads, and the pack files in qa/backups)
"""
import glob, os, shutil, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
clang = shutil.which("clang") or next(iter(glob.glob(os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW.UCRT*\**\clang.exe"), recursive=True)), None)
if not clang:
    print("FAIL  no clang"); sys.exit(1)
pk = sys.argv[1:]
if not pk:
    orig = os.path.expanduser("~/Downloads/Pocket AMP.fwsc")
    pk = ([orig] if os.path.exists(orig) else []) + sorted(glob.glob(os.path.join(ROOT, "release", "*", "*.fwsc")))[-1:] + sorted(glob.glob(os.path.join(ROOT, "qa", "backups", "*", "Pocket_AMP_Custom_pack*.fwsc")))[-1:]
if not pk:
    print("FAIL  no package to serve"); sys.exit(1)
exe = os.path.join(tempfile.mkdtemp(prefix="pa_ota_"), "t.exe")
r = subprocess.run([clang, "-O1", "-Wall", "-Wextra", "-DPA_OTA_NO_STUBS=1", "-I" + os.path.join(ROOT, "include"), os.path.join(ROOT, "test", "test_ota_session.c"), os.path.join(ROOT, "src", "sys", "pa_ota_session.c"), "-o", exe], capture_output=True, text=True)
if r.returncode:
    print(r.stdout + r.stderr); sys.exit(1)
r = subprocess.run([exe] + pk, capture_output=True, text=True); print(r.stdout)
sys.exit(r.returncode)
