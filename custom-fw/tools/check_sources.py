#!/usr/bin/env python3
"""tools/check_sources.py - the source-list drift guard.

Every build target compiles the tree in custom-fw/src minus the board files it
replaces itself, and that difference is written down ONCE, in
custom-fw/sources.txt. Before this, every build script carried its own
hand-written list, and a new module was silently missing from one of them.

This check makes that class of bug impossible to miss:

  - an excluded path that does not exist        (a stale entry)
  - a module excluded from every target         (nobody builds it)
  - a target excluding something that is not a board file (a silent omission)
  - a build script that names modules the manifest does not agree with

Usage:  python custom-fw/tools/check_sources.py
Exit code 0 = the manifest and the build scripts agree.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sources as S   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FW = S.FW


def script_named_modules(path):
    """The modules a build script names on its command line."""
    try:
        txt = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return None
    found = set()
    for m in re.finditer(r"custom-fw/(src/[\w/]+\.c)", txt):
        found.add(m.group(1))
    return found


def script_globs(path):
    try:
        txt = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return False
    return ("sources.txt" in txt) or ("Get-ChildItem" in txt and "src" in txt)


def main():
    sections = S._parse()
    srcs = S.all_sources()
    srcset = set(srcs)
    fails = []

    print("the source manifest: custom-fw/sources.txt")
    print("  %d modules under custom-fw/src\n" % len(srcs))

    compiled_by = {}
    for name in sorted(sections):
        if name.startswith("g_"):
            continue
        try:
            files = S.sources_for(name, sections=sections)
        except Exception as e:
            fails.append("%s: %s" % (name, e))
            continue
        compiled_by[name] = set(files)
        print("%-14s compiles %2d of %d modules" % (name, len(files), len(srcs)))
        for bad in sections[name]["exclude"]:
            if bad not in srcset:
                fails.append("%s: excludes '%s', which does not exist" % (name, bad))
            elif name in S.HOST_TARGETS and not any(bad.startswith(a) for a in S.ALLOWED_EXCLUDE):
                fails.append("%s: excludes '%s', which is not a board file - that "
                             "looks like a silent omission" % (name, bad))

    # a module that NO target builds
    union = set()
    for f in compiled_by.values():
        union |= f
    for o in sorted(srcset - union):
        fails.append("no target compiles '%s'" % o)

    # the build scripts must agree with the manifest
    checks = [
        ("emu/build_dll.ps1",  "emu"),
        ("emu/run_emu.ps1",    "emu"),
        ("wasm/build_wasm.py", "wasm"),
    ]
    for rel, target in checks:
        p = os.path.join(FW, rel)
        if not os.path.exists(p):
            continue
        named = script_named_modules(p)
        if named:
            want = compiled_by.get(target, set())
            stale = named - want
            missing = want - named
            if stale:
                fails.append("%s names modules the manifest does not compile for "
                             "%s: %s" % (rel, target, ", ".join(sorted(stale))))
            if missing:
                fails.append("%s does not name modules the manifest compiles for "
                             "%s: %s" % (rel, target, ", ".join(sorted(missing))))
        elif not script_globs(p):
            fails.append("%s neither globs the sources nor names them" % rel)

    print()
    if fails:
        print("SOURCE LIST DRIFT:")
        for f in fails:
            print("  - " + f)
        print("\nRESULT: FAILED")
        return 1
    print("RESULT: the manifest and the build scripts agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
