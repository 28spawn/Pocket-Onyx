import tempfile
"""
Build the Pocket Amp firmware core to WebAssembly.

The firmware's portable core (DSP, NAM, FX, SYS, the UI) is compiled to
wasm32 so the browser runs the ACTUAL firmware - the same pa_ui_render() and
the same DSP - rather than a re-implementation.

Requires: zig (its `zig cc` is a clang with the wasm32 target).
Usage:   python wasm/build_wasm.py [--zig <path to zig.exe>]
"""
import os, subprocess, sys, glob, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
FW   = os.path.dirname(HERE)
OUT  = os.path.join(HERE, "out")

# ---- the source list -------------------------------------------------------
# Globbed from custom-fw/src, minus the board files the WebAssembly shim
# replaces itself (the panel, the ALNK driver, the NOR flash, the GPIO
# scanner and the device's main()). The difference is written down ONCE, in
# custom-fw/sources.txt, and tools/check_sources.py keeps the manifest and
# the build scripts honest. Before this, each build script carried its own
# hand-written list, and a new module was silently missing from one of them.
def _manifest():
    man, section = {}, None
    path = os.path.join(FW, "sources.txt")
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1].strip().lower()
                man.setdefault(section, [])
            elif line.lower().startswith("exclude") and "=" in line and section:
                man[section] = [p.strip().replace("\\", "/")
                                for p in line.split("=", 1)[1].split(",") if p.strip()]
    return man

def _core_sources():
    excl = set(_manifest().get("wasm", []))
    out = []
    for dp, _dn, fn in os.walk(os.path.join(FW, "src")):
        for f in sorted(fn):
            if not f.endswith(".c"):
                continue
            rel = os.path.relpath(os.path.join(dp, f), FW).replace(os.sep, "/")
            if rel not in excl:
                out.append(rel)
    return sorted(out)

CORE = _core_sources() + ["wasm/pa_wasm.c"]

def _zig_works(path):
    """A broken shim (e.g. the npm @ziglang/cli wrapper whose postinstall failed)
    exits non-zero, so verify the candidate before using it. `zig version` prints
    just the version number, so only the exit code is checked."""
    if not path or not os.path.exists(path):
        return False
    try:
        r = subprocess.run([path, "version"], capture_output=True, text=True, timeout=30)
        return r.returncode == 0 and bool((r.stdout or r.stderr).strip())
    except Exception:
        return False

def find_zig(arg=None):
    cands = []
    if arg:                       cands.append(arg)
    env = os.environ.get("ZIG")
    if env:                       cands.append(env)
    # a real zig.exe on the PATH
    w = shutil.which("zig.exe") or shutil.which("zig")
    if w:                         cands.append(w)
    # the standard install locations
    for base in (os.path.join(os.path.expanduser("~"), "tools", "zig"),
                 os.path.join(os.path.expanduser("~"), ".zig"),
                 r"C:\zig", r"C:\Program Files\zig",
                 os.path.join(tempfile.gettempdir(), "opencode", "zig")):
        if not os.path.isdir(base): continue
        for dp, dn, fn in os.walk(base):
            if "zig.exe" in fn: cands.append(os.path.join(dp, "zig.exe"))
    for c in cands:
        if _zig_works(c):
            return c
    sys.exit("zig not found (or not working).\n"
             "  install it with:  npm i -g @ziglang/cli\n"
             "  or download from: https://ziglang.org/download\n"
             "  then re-run, or pass it explicitly:  --zig <path to zig.exe>")

def inline(wasm_path, html_path, out_path):
    """Bake the wasm into the page so it works straight from the disk.

    A browser blocks fetch() of a local file (CORS), so opening
    workbench.html with file:// cannot load pocket_amp.wasm. This writes a
    self-contained workbench-standalone.html with the module embedded as
    base64 (~35 % bigger, but it opens with a double-click).
    """
    import base64
    html = open(html_path, encoding="utf-8").read()
    b64 = base64.b64encode(open(wasm_path, "rb").read()).decode("ascii")
    tag = '<script>window.PA_WASM_B64="";</script>'
    if tag not in html:
        print("  [warn] could not find the injection point in workbench.html")
        return 1
    html = html.replace(tag, '<script>window.PA_WASM_B64="' + b64 + '";</script>', 1)
    open(out_path, "w", encoding="utf-8", newline="\n").write(html)
    print(f"  -> {out_path}  ({os.path.getsize(out_path)/1048576:.1f} MB, self-contained)")
    print("     open it with a double-click - no server needed")
    return 0


def main():
    zig = find_zig(sys.argv[2] if len(sys.argv) > 2 and sys.argv[1] == "--zig" else None)
    print(f"zig: {zig}")
    os.makedirs(OUT, exist_ok=True)
    target = "wasm32-wasi-musl"
    objs = []
    for rel in CORE:
        src = os.path.join(FW, rel.replace("/", os.sep))
        obj = os.path.join(OUT, rel.replace("/", "_").replace(".c", ".o"))
        cmd = [zig, "cc", "--target=" + target, "-Oz", "-Wall", "-fPIC",
               "-ffreestanding", "-fno-builtin",
               "-DPA_HOST_SIM=1", "-DPA_EMU_DLL=1", "-DPA_POOL_TEST=1",
               "-I", os.path.join(FW, "include"),
               "-c", src, "-o", obj]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"[FAIL] {rel}\n{r.stderr[:1500]}")
            return 1
        if r.stderr.strip():
            for line in r.stderr.strip().splitlines():
                if "warning" in line: print(f"  [warn] {rel}: {line.strip()}")
        objs.append(obj)
    wasm = os.path.join(OUT, "pocket_amp.wasm")
    cmd = [zig, "cc", "--target=" + target, "-Oz", "-shared", "-fPIC",
           "-Wl,--no-entry", "-Wl,--export-dynamic",
           "-o", wasm, *objs]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("LINK FAILED\n" + (r.stdout + r.stderr)[:3000])
        return 1
    print(f"  -> {wasm}  ({os.path.getsize(wasm)} bytes)")

    # the page loads pocket_amp.wasm next to itself
    import shutil
    page_dir = os.path.join(FW, "wasm")
    try:
        shutil.copy2(wasm, os.path.join(page_dir, "pocket_amp.wasm"))
    except Exception as e:
        print(f"  [warn] could not copy the wasm next to workbench.html: {e}")

    if "--inline" in sys.argv:
        html = os.path.join(page_dir, "workbench.html")
        out  = os.path.join(page_dir, "workbench-standalone.html")
        if inline(wasm, html, out):
            return 1
    else:
        print("  run:  python custom-fw/wasm/serve.py     (then open the printed URL)")
        print("  or:   python custom-fw/wasm/build_wasm.py --inline   (self-contained page)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
