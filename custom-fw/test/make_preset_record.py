"""The preset record the host tool builds, as hex, for the wasm test.

`custom-fw/tools/mvave_device.py` builds the preset record - pa_preset_t, the
164 bytes the firmware stores - and the firmware checks its CRC32 before it
saves the slot. This writes that record out so `test_preset.js` can push it
through the firmware's own parser and read it back, which proves the tool's
builder and the firmware agree.

    python custom-fw/test/make_preset_record.py [out.hex]
    node custom-fw/wasm/test_preset.js custom-fw/wasm/out/pocket_amp.wasm out.hex
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import mvave_device as M          # noqa: E402

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "preset_record.hex")

rec = M.build_preset_record(
    "WORKBENCH P41", 77,
    slot_order=[0, 1, 2, 3, 4, 5, 6, 7],
    amp_model_idx=5, cab_model_idx=3,
    params=[[1000, 2000, 3000, 4000, 5000, 6000]] * 8)

with open(out, "w", encoding="ascii") as f:
    f.write(rec.hex())
print("wrote %s (%d bytes)" % (out, len(rec)))
print("the fields:", M.unpack_preset_record(rec))
