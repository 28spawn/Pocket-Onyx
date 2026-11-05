#!/usr/bin/env python3
"""
custom-fw/emu/nam_loader.py — Universal .nam (Neural Amp Modeler) & .wav IR Loader for Pocket Amp Emulator

Supports:
  1. Standard ToneHunt / Neural Amp Modeler `.nam` JSON files (all versions, flat or nested model/net dictionaries)
  2. M-VAVE binary `.nam` / `.am3` / `.am4` containers (@M2, @M3, @M4, NAM, LMT magics)
  3. Direct Pocket Amp GRU JSON / binary `.am3data` (3,852 B / 4,040 B `pa_nam_model_t` struct)
  4. Standard `.wav` Cabinet Impulse Response files (16-bit, 24-bit, 32-bit int, 32-bit float PCM)
"""

import json
import math
import struct
from typing import Dict, Any, Tuple, List, Optional

Q21_SCALE = 2097152.0  # 1 << 21 (matches PA_Q21_ONE in pa_model.h)
NAM_HIDDEN = 16
PA_NAM_STRUCT_BYTES = 3852


def detect_voicing_profile(name: str, gear_type: str = "") -> Tuple[int, str]:
    """Detects optimal Pocket Amp 5-category adaptive voicing profile (0..4)."""
    text = f"{name} {gear_type}".lower()
    if any(k in text for k in ["bass", "sub", "lowend", "4x10", "8x10"]):
        return 4, "Bass"
    if any(k in text for k in ["fuzz", "octave", "velcro", "gated", "fuzzbox"]):
        return 3, "Fuzz"
    if any(k in text for k in ["lead", "metal", "chug", "high gain", "highgain", "modern", "stack", "savage", "fireball", "invader", "uber", "stealth"]):
        return 2, "High Gain"
    if any(k in text for k in ["crunch", "drive", "british", "vintage", "overdrive", "rock", "breakup", "blues", "warm", "boost", "od"]):
        return 1, "Crunch"
    return 0, "Clean"


def has_baked_cab(gear_type: str, model_name: str, filename: str) -> bool:
    """True when the capture includes the speaker cabinet. "... CAB FREE", "NO CAB", "w/o cab" are amp-only captures
    that merely have the word in their name - they need the CAB slot, so it must not be bypassed for them."""
    import re
    names = (model_name.lower(), filename.lower())
    if any(re.search(r"cab[ _\-]*free|no[ _\-]*cab|without[ _\-]*cab|w/o[ _\-]*cab|cabless|cab[ _\-]*less|amp[ _\-]*only", n) for n in names):
        return False
    return ("cab" in gear_type) or any("cab" in n for n in names)


def recommend_amp_settings(meta: Dict[str, Any], voicing_id: int, is_amp_cab: bool) -> Dict[str, Any]:
    """IMP-01: derive the AMP-block settings that are stored WITH the capture.

    The firmware used to keep whatever GAIN/BASS/.../MASTER the previous amp had. Captures already
    contain the amp's own gain and EQ, so the neutral starting point is 50 % everywhere (0 dB input
    gain, flat tone stack, unity master). Two things from the .nam metadata are worth using:
      * "loudness" (dB, measured by the NAM trainer on its standard input) -> output makeup so the
        capture lands near the stock-amp target of -18 dBFS RMS;
      * "gain" (0..1 trainer estimate) / voicing -> gentle presence/treble trim for high-gain
        captures, which tend to be fizzy without a cab.
    """
    knobs = [50, 50, 50, 50, 50, 50]          # GAIN, BASS, MID, TREBLE, PRES, MASTER
    loud = None
    for k in ("loudness", "output_loudness", "loudness_db"):
        v = meta.get(k)
        if isinstance(v, (int, float)):
            loud = float(v)
            break
    makeup_db = 0.0 if loud is None else max(-12.0, min(12.0, -18.0 - loud))
    gain = meta.get("gain")
    hi_gain = voicing_id in (2, 3) or (isinstance(gain, (int, float)) and float(gain) > 0.6)
    if hi_gain and not is_amp_cab:
        knobs[3] = 45                          # TREBLE -1 step
        knobs[4] = 42                          # PRESENCE -1.3 dB: tames fizz on amp-only captures
    if voicing_id == 4:                        # bass: a touch of low end back
        knobs[1] = 55
    return {"amp_knobs": knobs, "makeup_db": round(makeup_db, 2)}


def clamp_i32(val: float) -> int:
    v = int(round(val))
    return max(-2147483648, min(2147483647, v))


def f_to_q21(val: float) -> int:
    return clamp_i32(val * Q21_SCALE)


def pack_pa_nam_model_struct(
    w_r: List[List[float]],      # [16][17]
    w_z: List[List[float]],      # [16][17]
    w_n: List[List[float]],      # [16][16]
    w_in: List[float],           # [16]
    w_out: List[float],          # [16]
    b_r: List[float],            # [16]
    b_z: List[float],            # [16]
    b_ni: List[float],           # [16]
    b_nh: List[float],           # [16]
    b_out: float = 0.0,
) -> bytes:
    buf = bytearray(PA_NAM_STRUCT_BYTES)
    off = 0

    # w_r_hw[9][17] + w_r_cpu[7][17]
    for r in range(16):
        for c in range(17):
            struct.pack_into("<i", buf, off, f_to_q21(w_r[r][c]))
            off += 4

    # w_z_hw[9][17] + w_z_cpu[7][17]
    for r in range(16):
        for c in range(17):
            struct.pack_into("<i", buf, off, f_to_q21(w_z[r][c]))
            off += 4

    # w_n_hw[9][16] + w_n_cpu[7][16]
    for r in range(16):
        for c in range(16):
            struct.pack_into("<i", buf, off, f_to_q21(w_n[r][c]))
            off += 4

    # w_in[16] (+3200)
    for i in range(16):
        struct.pack_into("<i", buf, off, f_to_q21(w_in[i]))
        off += 4

    # w_out[16] (+3264)
    for i in range(16):
        struct.pack_into("<i", buf, off, f_to_q21(w_out[i]))
        off += 4

    # Skip runtime state (+3328 .. +3591 = 264 bytes)
    off = 3592

    # Biases: b_r[16], b_z[16], b_ni[16], b_nh[16], b_out
    for vec in (b_r, b_z, b_ni, b_nh):
        for i in range(16):
            struct.pack_into("<i", buf, off, f_to_q21(vec[i]))
            off += 4

    struct.pack_into("<i", buf, off, f_to_q21(b_out))
    return bytes(buf)


def pack_wavenet_blob(weights: List[float], head_scale: float = 1.0, is_amp_cab: bool = False, input_trim: float = 1.0) -> bytes:
    """Packs a 23-layer dilated WaveNet model (3-ch Lite or 8-ch Full) into the binary container parsed by Pocket Amp."""
    flags = 1 if is_amp_cab else 0
    header = struct.pack("<4sIfIf", b"WAVE", len(weights), float(head_scale), flags, float(input_trim))
    weights_bytes = struct.pack(f"<{len(weights)}f", *weights)
    return header + weights_bytes


# The Pocket Amp's ADC full scale in dBu. 4.0 is an ASSUMPTION (not measured): the input trim of a capture is 10 ** ((ADC_REF_DBU - capture input level) / 20), so a capture
# made at 15 dBu gets -11 dB. If a high-gain capture sounds too clean, raise this (pa_import.py --ref-dbu 15 gives a trim of 1.0).
ADC_REF_DBU = 4.0


def extract_input_level_dbu(meta: Dict[str, Any], data: Dict[str, Any]) -> Optional[float]:
    """Extracts optional input_level_dbu calibration metadata from ToneHunt / NAM exports."""
    for obj in (meta, data.get("metadata", {}), data.get("config", {}), data):
        if isinstance(obj, dict):
            for k in ("input_level_dbu", "input_level", "input_dBu", "calibration_dBu"):
                if k in obj and obj[k] is not None:
                    try:
                        return float(obj[k])
                    except (ValueError, TypeError):
                        pass
    return None


def find_weights_recursive(obj: Any) -> Optional[List[float]]:
    """Recursively traverses any JSON tree to locate weights array or tensor dictionaries."""
    if isinstance(obj, dict):
        if "weights" in obj and isinstance(obj["weights"], list) and len(obj["weights"]) > 0:
            if isinstance(obj["weights"][0], (int, float)):
                return [float(x) for x in obj["weights"]]
        for k in ("model", "net", "network", "layers", "state_dict"):
            if k in obj:
                found = find_weights_recursive(obj[k])
                if found:
                    return found
        # Check if dictionary of named float tensors (e.g. PyTorch state_dict)
        flat = []
        for v in obj.values():
            if isinstance(v, list) and v and isinstance(v[0], (int, float)):
                flat.extend(float(x) for x in v)
        if len(flat) >= 16:
            return flat
        for v in obj.values():
            found = find_weights_recursive(v)
            if found:
                return found
    elif isinstance(obj, list):
        if obj and isinstance(obj[0], (int, float)):
            return [float(x) for x in obj]
        for item in obj:
            found = find_weights_recursive(item)
            if found:
                return found
    return None


def find_meta_recursive(obj: Any) -> Dict[str, Any]:
    """Extracts metadata name/architecture from any level in JSON."""
    meta: Dict[str, Any] = {}
    if isinstance(obj, dict):
        for k in ("name", "model_name", "gear_make", "gear_model", "architecture", "tone_type", "gear_type"):
            if k in obj and isinstance(obj[k], str):
                meta[k] = obj[k]
        if "metadata" in obj and isinstance(obj["metadata"], dict):
            meta.update(obj["metadata"])
        if "model" in obj and isinstance(obj["model"], dict):
            for k in ("architecture", "name", "gear_type"):
                if k in obj["model"] and isinstance(obj["model"][k], str):
                    meta[k] = obj["model"][k]
    return meta


def parse_nam_file(raw_bytes: bytes, filename: str = "model.nam") -> Tuple[bytes, Dict[str, Any]]:
    """
    Universal parser for all .nam files:
      - Binary M-VAVE (@M2, @M3, @M4, NAM, LMT, raw structs)
      - Standard ToneHunt JSON (top-level or nested model.weights / net.weights)
      - Prioritizes 8-Channel A2-Full studio resolution from SlimmableContainer
    """
    # 1. Binary checks
    if len(raw_bytes) >= 4 and not raw_bytes.lstrip().startswith(b"{"):
        magic4 = raw_bytes[:4]
        # Direct 3852 or 4040 runtime struct
        if len(raw_bytes) in (3852, 4040):
            return raw_bytes[:3852], {
                "name": filename,
                "architecture": "PocketAmp-Q21-GRU (Binary 0xFC8)",
                "weights_count": len(raw_bytes) // 4,
                "drive_est": 1.0,
            }
        # M-VAVE @M2 / @M3 / @M4 / NAM container
        # am3_region is at +6204 (+0x183C), am4_region at +7868 (+0x1EBC)
        if magic4 in (b"@M2\0", b"@M3\0", b"@M4\0", b"NAM\0") or magic4.startswith(b"@M"):
            if len(raw_bytes) >= 6204 + 3852:
                am3_slice = raw_bytes[6204 : 6204 + 3852]
                return am3_slice, {
                    "name": filename,
                    "architecture": f"M-VAVE Container ({magic4[:3].decode('latin1', 'ignore')})",
                    "weights_count": 961,
                    "drive_est": 1.15,
                }
            if len(raw_bytes) >= 3852:
                return raw_bytes[:3852], {
                    "name": filename,
                    "architecture": f"M-VAVE Binary ({magic4[:3].decode('latin1', 'ignore')})",
                    "weights_count": 961,
                    "drive_est": 1.0,
                }
        # Raw float32 array
        if len(raw_bytes) >= 64 and (len(raw_bytes) % 4 == 0):
            try:
                floats = list(struct.unpack(f"<{len(raw_bytes)//4}f", raw_bytes))
                if all(not math.isnan(x) and not math.isinf(x) and abs(x) < 1000.0 for x in floats[:32]):
                    return build_gru_from_weight_vector(floats, filename, "Raw Float32 Binary")
            except Exception:
                pass

    # 2. JSON parsing
    try:
        data = json.loads(raw_bytes.decode("utf-8", errors="replace"))
    except Exception as e:
        raise ValueError(f"File is neither valid binary nor JSON: {e}")

    # NAM "A2" SlimmableContainer (a Lite and a Full WaveNet, e.g. emu/test_capture.nam)
    if isinstance(data, dict) and data.get("architecture") == "SlimmableContainer":
        submodels = data.get("config", {}).get("submodels", [])
        # Prioritize Submodel 0 (Channels=3, 1,871 weights, A2-Lite) for the embedded hardware
        # target (AC791N: PA_CPU_HZ is 360 MHz, the data-sheet rate is 320 - see pa_config.h)
        # to preserve CPU headroom (~37-42% vs ~174-196% for Full).
        # Fall back to Submodel 1 (Channels=8, 12,146 weights, A2-Full) only if Lite is not present.
        chosen_sm = None
        for sm in submodels:
            m = sm.get("model", {})
            sm_w = m.get("weights", [])
            if len(sm_w) in (1870, 1871):
                chosen_sm = sm
                break
        if chosen_sm is None:
            for sm in submodels:
                m = sm.get("model", {})
                sm_w = m.get("weights", [])
                if len(sm_w) in (12145, 12146):
                    chosen_sm = sm
                    break

        if chosen_sm is not None:
            m = chosen_sm.get("model", {})
            sm_w = m.get("weights", [])
            head_scale = float(m.get("config", {}).get("head_scale", 1.0))
            weights_list = [float(x) for x in sm_w]
            if len(weights_list) in (1870, 12145):
                weights_list.append(head_scale)
            meta = m.get("metadata", {}) or data.get("metadata", {})
            model_name = meta.get("name") or meta.get("gear_model") or filename
            gear_type = str(meta.get("gear_type", "")).lower()
            is_amp_cab = has_baked_cab(gear_type, model_name, filename)
            ch = 3 if len(weights_list) == 1871 else 8
            v_id, v_name = detect_voicing_profile(model_name, gear_type)
            # Automatic Input Level Staging Calibration
            PA_ADC_REF_DBU = ADC_REF_DBU
            input_level_dbu = extract_input_level_dbu(meta, data)
            if input_level_dbu is not None:
                input_trim = round(10.0 ** ((PA_ADC_REF_DBU - input_level_dbu) / 20.0), 4)
            else:
                input_trim = 1.0
            blob = pack_wavenet_blob(weights_list, head_scale, is_amp_cab, input_trim)
            arch_label = f"ToneHunt WaveNet A2 ({ch}-Channel {'Lite' if ch == 3 else 'Full'}{' + Cab' if is_amp_cab else ''})"
            rec = recommend_amp_settings(meta, v_id, is_amp_cab)
            return blob, {
                **rec,
                "name": model_name,
                "architecture": arch_label,
                "engine": "WaveNet",
                "channels": ch,
                "weights_count": len(weights_list),
                "is_amp_cab": is_amp_cab,
                "drive_est": 3.20 if "bog" in model_name.lower() or "gain" in model_name.lower() else 1.50,
                "head_scale": head_scale,
                "input_level_dbu": input_level_dbu,
                "input_trim": input_trim,
                "voicing_id": v_id,
                "voicing_name": v_name,
            }

    # Check for direct Pocket Amp GRU JSON
    if "w_ih" in data and "w_hh" in data:
        w_ih = data["w_ih"]
        w_hh = data["w_hh"]
        b_ih = data["b_ih"]
        b_hh = data["b_hh"]
        head_w = data["head_w"]
        head_b = float(data.get("head_b", 0.0))

        w_r = [list(w_hh[0][i]) + [w_ih[0][i]] for i in range(16)]
        w_z = [list(w_hh[1][i]) + [w_ih[1][i]] for i in range(16)]
        w_n = [list(w_hh[2][i]) for i in range(16)]
        w_in = [float(w_ih[2][i]) for i in range(16)]
        w_out = [float(head_w[i]) for i in range(16)]
        b_r = [float(b_ih[0][i] + b_hh[0][i]) for i in range(16)]
        b_z = [float(b_ih[1][i] + b_hh[1][i]) for i in range(16)]
        b_ni = [float(b_ih[2][i]) for i in range(16)]
        b_nh = [float(b_hh[2][i]) for i in range(16)]

        blob = pack_pa_nam_model_struct(w_r, w_z, w_n, w_in, w_out, b_r, b_z, b_ni, b_nh, head_b)
        v_id, v_name = detect_voicing_profile(data.get("name", filename), "")
        return blob, {
            "name": data.get("name", filename),
            "architecture": "GRU-16 (Direct Native)",
            "weights_count": 961,
            "drive_est": round(sum(abs(x) for x in w_in) / 16.0, 2),
            "voicing_id": v_id,
            "voicing_name": v_name,
        }

    # Extract weights from any level in JSON
    weights = find_weights_recursive(data)
    if not weights or len(weights) == 0:
        raise ValueError("Could not find weights array in .nam JSON (checked root, model, net, layers, state_dict)")

    meta = find_meta_recursive(data)
    model_name = meta.get("name") or meta.get("gear_model") or filename
    arch = meta.get("architecture") or data.get("architecture") or "WaveNet"

    # If standard 23-layer WaveNet weights array (1870, 1871, 12145, or 12146 weights)
    if arch == "WaveNet" and len(weights) in (1870, 1871, 12145, 12146):
        head_scale = float(data.get("config", {}).get("head_scale", 1.0))
        weights_list = [float(x) for x in weights]
        if len(weights_list) in (1870, 12145):
            weights_list.append(head_scale)
        gear_type = str(meta.get("gear_type", "")).lower()
        is_amp_cab = has_baked_cab(gear_type, model_name, filename)
        ch = 8 if len(weights_list) == 12146 else 3
        v_id, v_name = detect_voicing_profile(model_name, gear_type)
        # Automatic Input Level Staging Calibration
        PA_ADC_REF_DBU = ADC_REF_DBU
        input_level_dbu = extract_input_level_dbu(meta, data)
        if input_level_dbu is not None:
            input_trim = round(10.0 ** ((PA_ADC_REF_DBU - input_level_dbu) / 20.0), 4)
        else:
            input_trim = 1.0
        blob = pack_wavenet_blob(weights_list, head_scale, is_amp_cab, input_trim)
        rec = recommend_amp_settings(meta, v_id, is_amp_cab)
        return blob, {
            **rec,
            "name": model_name,
            "architecture": f"ToneHunt WaveNet A2 ({ch}-Channel {'Lite' if ch == 3 else 'Full'}{' + Cab' if is_amp_cab else ''})",
            "engine": "WaveNet",
            "channels": ch,
            "weights_count": len(weights_list),
            "is_amp_cab": is_amp_cab,
            "drive_est": 3.20 if "bog" in model_name.lower() or "gain" in model_name.lower() else 1.50,
            "head_scale": head_scale,
            "input_level_dbu": input_level_dbu,
            "input_trim": input_trim,
            "voicing_id": v_id,
            "voicing_name": v_name,
        }

    return build_gru_from_weight_vector(weights, model_name, arch, meta)



def build_gru_from_weight_vector(
    weights: List[float],
    model_name: str,
    arch: str,
    meta: Optional[Dict[str, Any]] = None
) -> Tuple[bytes, Dict[str, Any]]:
    """
    Projects any arbitrary weight distribution into the 16-unit Q21 GRU structure with
    dynamic range expansion based on model metadata (gain, loudness, tone_type) and weight statistics.
    
    Distinguishes:
      - Clean / Low-Gain: Input scale 0.40 - 0.75, decay tau = 0.85, linear head
      - Crunch / Medium:  Input scale 1.10 - 1.80, decay tau = 0.70
      - High-Gain / Lead: Input scale 2.50 - 4.80, heavy asymmetrical saturation, high recurrent drive
    """
    if meta is None:
        meta = {}

    n_w = len(weights)
    rms = math.sqrt(sum(w * w for w in weights) / max(1, n_w))
    mean_w = sum(weights) / max(1, n_w)
    max_w = max(abs(w) for w in weights) if weights else 1.0

    # 1. Inspect Tone / Gain Indicators from Metadata
    name_lower = (model_name + " " + str(meta.get("gear_model", "")) + " " + str(meta.get("tone_type", ""))).lower()
    
    # Gain hints from metadata
    meta_gain = meta.get("gain")
    meta_loudness = meta.get("loudness") # typical -10 to -24 dB
    
    is_clean = any(k in name_lower for k in ("clean", "cln", "twin", "chorus", "acoustic", "combo", "jazz"))
    is_high_gain = any(k in name_lower for k in ("lead", "hi-gain", "high-gain", "high gain", "modern", "stack", "uber", "heavy", "metal", "savage", "doom", "fuzz", "fireball", "invader", "solo", "hotrod", "hot rod"))

    # Compute base drive
    base_drive = (rms * 4.2) + (max_w * 0.40)
    
    if meta_gain is not None and isinstance(meta_gain, (int, float)):
        # NAM trainer gain normalized 0.0 .. 1.0+
        base_drive = float(meta_gain) * 3.5

    if is_clean and not is_high_gain:
        # Scale for clean / low-gain: 0.40 .. 0.75
        drive = max(0.40, min(0.75, base_drive * 0.55))
        tau = 0.85
        asym_mult = 0.4
        gain_tier = "Clean / Low-Gain"
    elif is_high_gain:
        # Scale for high-gain / lead: 2.50 .. 4.80
        drive = max(2.50, min(4.80, base_drive * 1.80 + 1.20))
        tau = 0.50
        asym_mult = 2.4
        gain_tier = "High-Gain / Modern Lead"
    else:
        # Mid-gain / Crunch: 1.10 .. 2.20
        drive = max(1.10, min(2.40, base_drive))
        tau = 0.68
        asym_mult = 1.0
        gain_tier = "Crunch / Dynamic Rock"

    asym = max(-0.55, min(0.55, mean_w * 4.5 * asym_mult))

    w_r = [[0.0] * 17 for _ in range(16)]
    w_z = [[0.0] * 17 for _ in range(16)]
    w_n = [[0.0] * 16 for _ in range(16)]
    w_in = [0.0] * 16
    w_out = [0.0] * 16
    b_r = [0.0] * 16
    b_z = [0.0] * 16
    b_ni = [0.0] * 16
    b_nh = [0.0] * 16

    stride = max(1, n_w // 256)
    for i in range(16):
        sign = -1.0 if (i & 1) else 1.0
        w_sample = weights[(i * stride * 7) % n_w]
        w_sample2 = weights[(i * stride * 13 + 5) % n_w]

        decay = 0.20 + 0.70 * (i / 16.0) * tau
        # Input drive scaled with wide dynamic range
        w_in[i] = drive * (1.45 / (1.0 + 0.14 * i)) * sign + 0.20 * w_sample
        w_out[i] = (0.38 / math.sqrt(i + 1)) * sign + 0.08 * w_sample2
        b_r[i] = 1.20 + 0.15 * w_sample
        b_z[i] = (decay * 3.0 - 1.5) + 0.12 * w_sample2
        # Asymmetrical tube clipping bias shift b_ni
        b_ni[i] = asym * 0.16 * sign + 0.06 * w_sample

        for j in range(16):
            idx_w = ((i * 16 + j) * stride) % n_w
            wk = max(-0.7, min(0.7, weights[idx_w]))
            if i == j:
                w_z[i][j] = decay
                w_r[i][j] = 0.18 + 0.12 * wk
                w_n[i][j] = (0.24 + 0.15 * wk) * (1.2 if is_high_gain else 0.8)
            else:
                rec_spread = 0.08 if is_high_gain else 0.03
                w_z[i][j] = rec_spread * wk
                w_r[i][j] = rec_spread * wk
                w_n[i][j] = rec_spread * wk * 1.5

        w_z[i][16] = 0.12 + 0.08 * w_sample
        w_r[i][16] = (0.16 + 0.08 * w_sample2) * sign

    blob = pack_pa_nam_model_struct(w_r, w_z, w_n, w_in, w_out, b_r, b_z, b_ni, b_nh, 0.0)
    return blob, {
        **recommend_amp_settings(meta, 2 if is_high_gain else (0 if is_clean else 1), False),
        "approximate": True,   # IMP-01: synthesised from weight statistics, NOT a faithful conversion
        "name": model_name,
        "architecture": f"{arch} -> Q21-GRU16 ({n_w} weights)",
        "gain_tier": gain_tier,
        "weights_count": n_w,
        "drive_est": round(drive, 2),
    }


def parse_riff_wav(raw_bytes: bytes) -> Dict[str, Any]:
    """Reads a RIFF/WAVE file: the format block and the sample data.

    Python's `wave` module only understands PCM (format tag 1). Half of the
    IR bank is IEEE float (format tag 3) - the Mesa 4x12 among them - and
    `wave` refused those with "unknown format: 3", so the button that loads
    that cabinet did nothing but show an error. The sample data is returned
    as it is; the caller decides what to do with it.
    """
    if len(raw_bytes) < 12 or raw_bytes[0:4] != b"RIFF" or raw_bytes[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")

    pos = 12
    fmt = None
    data = b""
    while pos + 8 <= len(raw_bytes):
        cid = raw_bytes[pos:pos + 4]
        (size,) = struct.unpack_from("<I", raw_bytes, pos + 4)
        body = raw_bytes[pos + 8: pos + 8 + size]
        if cid == b"fmt ":
            if len(body) >= 16:
                tag, ch, sr, byte_rate, block_align, bits = struct.unpack_from("<HHIIHH", body, 0)
                fmt = {
                    "format_tag": tag,
                    "channels": max(1, int(ch)),
                    "sample_rate": int(sr),
                    "byte_rate": int(byte_rate),
                    "block_align": int(block_align),
                    "bits": int(bits),
                }
        elif cid == b"data":
            data = body
        pos += 8 + size + (size & 1)      # chunks are word aligned

    if fmt is None:
        raise ValueError("no fmt chunk in the WAVE file")
    if not data:
        raise ValueError("no data chunk in the WAVE file")
    fmt["data"] = data
    return fmt


def parse_wav_ir(raw_bytes: bytes, max_taps: int = 1024) -> Tuple[List[float], Dict[str, Any]]:
    info = parse_riff_wav(raw_bytes)
    tag = info["format_tag"]
    nch = info["channels"]
    sr = info["sample_rate"]
    bits = info["bits"]

    if tag not in (1, 3):
        raise ValueError(f"unsupported WAVE format tag: {tag}")

    if tag == 3:
        # IEEE float: 32 or 64 bits
        sw = 4 if bits == 32 else 8
        code = "f" if sw == 4 else "d"
    else:
        sw = max(1, bits // 8)
        code = None

    frame_bytes = sw * nch
    pcm = info["data"]
    nframes = min(len(pcm) // frame_bytes if frame_bytes else 0, max_taps)

    taps: List[float] = []
    for i in range(nframes):
        off = i * frame_bytes
        ch0 = pcm[off: off + sw]
        if sw == 1:
            val = (ch0[0] - 128) / 128.0
        elif sw == 2:
            val = struct.unpack("<h", ch0)[0] / 32768.0
        elif sw == 3:
            b0, b1, b2 = ch0[0], ch0[1], ch0[2]
            ival = b0 | (b1 << 8) | (b2 << 16)
            if ival & 0x800000:
                ival -= 0x1000000
            val = ival / 8388608.0
        elif code == "f":
            fval = struct.unpack("<f", ch0)[0]
            if not math.isnan(fval) and abs(fval) <= 10.0:
                val = fval
            else:
                val = struct.unpack("<i", ch0)[0] / 2147483648.0
        elif code == "d":
            dval = struct.unpack("<d", ch0)[0]
            if not math.isnan(dval) and abs(dval) <= 10.0:
                val = dval
            else:
                val = 0.0
        elif sw == 4:
            val = struct.unpack("<i", ch0)[0] / 2147483648.0
        else:
            val = 0.0
        taps.append(float(val))

    peak = max((abs(x) for x in taps), default=1.0)
    if peak > 1e-6:
        scale = 0.85 / peak
        taps = [x * scale for x in taps]

    return taps, {
        "channels": nch,
        "sample_rate": sr,
        "bit_depth": bits,
        "format_tag": tag,
        "taps_loaded": len(taps),
    }
