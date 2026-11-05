#!/usr/bin/env python3
"""
custom-fw/tools/gen_editor_tables.py - the model and knob names of the web editor (web/Onyx_Web_Editor.html), taken from the firmware's own tables in src/board/ui_app.c, so the page can not drift
from the unit's screen. The amp and cabinet names are NOT here: the page reads them from the unit (cmd 81).

    python custom-fw/tools/gen_editor_tables.py            rewrite the block between the TABLES markers in web/Onyx_Web_Editor.html
    python custom-fw/tools/gen_editor_tables.py --check    exit 1 when the page's block differs from the firmware (the gate runs this)
"""
import json, re, sys
from pathlib import Path

FW = Path(__file__).resolve().parent.parent
SRC = FW / "src" / "board" / "ui_app.c"
PAGE = FW / "web" / "Onyx_Web_Editor.html"
BEGIN, END = "/*TABLES-BEGIN (tools/gen_editor_tables.py: do not edit)*/", "/*TABLES-END*/"


def strs(body):
    return re.findall(r'"((?:[^"\\]|\\.)*)"', body)


def array(text, name, dims):
    m = re.search(r"static const char \* const " + name + r"\[[^=]*\]\s*=\s*\{(.*?)\n?\s*\};", text, re.S)
    if not m:
        raise SystemExit(f"gen_editor_tables: {name} not found in ui_app.c")
    flat = strs(re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S))
    if dims == 1:
        return flat
    assert len(flat) % dims == 0, name
    return [flat[i:i + dims] for i in range(0, len(flat), dims)]


def tables():
    t = SRC.read_text(encoding="utf-8")
    f = t[t.index("get_slot_model_name(const pa_ui_ctx_t"):t.index("/* UI Primitives */")]
    cnt = re.search(r"static uint8_t get_slot_param_count.*?\n}\n", t, re.S).group(0)
    counts = dict((k, int(v)) for k, v in re.findall(r"case PA_SLOT_(\w+):\s+return (\d+);", cnt))
    eqn = [int(x) for x in re.search(r"static const uint8_t n\[3\] = \{([^}]*)\}", cnt).group(1).split(",")]
    return {
        "models": {"DYN": array(f, "dyn_models", 1), "DRIVE": array(f, "drv_models", 1), "EQ": array(f, "eq_models", 1), "MOD": array(f, "mod_models", 1),
                   "DLY": array(f, "dly_models", 1), "REV": array(f, "rev_models", 1)},
        "params": {"DYN": array(f, "dyn_params", 4), "DRIVE": array(f, "drv_params", 1), "AMP": array(f, "amp_params", 1), "CAB": array(f, "cab_params", 1),
                   "EQ": [array(f, "eq_bands", 1), array(f, "eq_cut", 1), array(f, "eq_par", 1)], "MOD": array(f, "mod_params", 4),
                   "DLY": [array(f, "dly_params", 1)[:3], array(f, "dly_aux", 1)], "REV": array(f, "rev_params", 1)},
        "counts": {"DYN": counts["DYNAMICS"], "DRIVE": counts["DRIVE"], "AMP": counts["AMP"], "CAB": counts["CAB"], "MOD": counts["MOD"], "DLY": counts["DELAY"],
                   "REV": counts["REVERB"], "EQ": eqn},
    }


def block():
    return BEGIN + "\nconst TABLES = " + json.dumps(tables(), separators=(",", ":")) + ";\n" + END


def main():
    page = PAGE.read_text(encoding="utf-8")
    a, b = page.find(BEGIN), page.find(END)
    if a < 0 or b < 0:
        print("web/Onyx_Web_Editor.html has no TABLES markers"); return 1
    new = page[:a] + block() + page[b + len(END):]
    if "--check" in sys.argv:
        if new != page:
            print("web/Onyx_Web_Editor.html: the model and knob tables differ from src/board/ui_app.c (run tools/gen_editor_tables.py)"); return 1
        print("editor tables: in sync with the firmware"); return 0
    PAGE.write_text(new, encoding="utf-8", newline="\n")
    print("editor tables written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
