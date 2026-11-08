#!/usr/bin/env python3
"""tools/sources.py - the source manifest reader.

custom-fw/sources.txt is the single place that says which modules each build
target compiles. This module parses it, so the Python build scripts and the
drift guard read it the same way.

Syntax (see sources.txt):
    [name]                 a target, or a group if the name starts with "g_"
    include = a, b, @grp   compile exactly these (globs and @groups allowed)
    exclude = a, b         compile everything under src/ EXCEPT these
"""
import fnmatch
import os
import re

FW = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(FW, "sources.txt")

# A HOST target (the emulator, the browser) may only exclude the board layer
# (the files it replaces itself) and the dead CPU1 offload: excluding anything
# else would silently drop firmware from a real build. The host TEST suites
# are deliberately minimal, so they may exclude what they like - but the
# excluded path must exist.
ALLOWED_EXCLUDE = ("src/board/", "src/sys/smp_cpu1.c")
HOST_TARGETS = ("emu", "wasm")


def _parse(path=MANIFEST):
    sections, section = {}, None
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            m = re.match(r"^\[(.+)\]$", line)
            if m:
                section = m.group(1).strip().lower()
                sections.setdefault(section, {"include": [], "exclude": []})
                continue
            m = re.match(r"^(include|exclude)\s*=\s*(.*)$", line, re.I)
            if m and section:
                key = m.group(1).lower()
                items = [p.strip().replace("\\", "/")
                         for p in m.group(2).split(",") if p.strip()]
                sections[section][key] = items
    return sections


def all_sources(fw=FW):
    """Every .c under custom-fw/src, relative to custom-fw/."""
    out = []
    for dp, _dn, fn in os.walk(os.path.join(fw, "src")):
        for f in fn:
            if f.endswith(".c"):
                out.append(os.path.relpath(os.path.join(dp, f), fw).replace(os.sep, "/"))
    return sorted(out)


def _expand(patterns, srcs, groups, seen=None):
    """Turn the manifest's include patterns into a list of real files."""
    seen = seen or set()
    out = []
    for pat in patterns:
        if pat.startswith("@"):
            g = pat[1:].strip().lower()
            if g in seen:
                continue
            seen.add(g)
            spec = groups.get(g)
            if not spec:
                raise KeyError("sources.txt: unknown group '@%s'" % g)
            out += _expand(spec["include"], srcs, groups, seen)
            continue
        if any(ch in pat for ch in "*?"):
            hits = [s for s in srcs if fnmatch.fnmatch(s, pat)]
            if not hits:
                raise ValueError("sources.txt: '%s' matches nothing" % pat)
            out += hits
        else:
            out.append(pat)
    return out


def sources_for(target, fw=FW, sections=None):
    """The source files a target compiles, in a stable order."""
    sections = sections or _parse(os.path.join(fw, "sources.txt"))
    spec = sections.get(target.lower())
    if not spec:
        raise KeyError("sources.txt has no target '%s'" % target)
    srcs = all_sources(fw)
    if spec["include"]:
        files = _expand(spec["include"], srcs, sections)
        return sorted(set(files))
    excl = set(spec["exclude"])
    return [s for s in srcs if s not in excl]


def main():
    sections = _parse()
    srcs = all_sources()
    for name in sorted(sections):
        if name.startswith("g_"):
            continue
        files = sources_for(name, sections=sections)
        print("%-14s %2d modules" % (name, len(files)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 The Onyx Authors (@28spawn)
