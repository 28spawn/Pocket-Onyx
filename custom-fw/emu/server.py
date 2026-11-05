#!/usr/bin/env python3
"""
custom-fw/emu/server.py — M-VAVE Pocket Amp Live Firmware Emulator & ASIO Audio Host

Features:
  - Enables PortAudio ASIO (`SD_ENABLE_ASIO=1`) to connect directly to USB ASIO audio interfaces
    (with automatic fallback to WASAPI / WDM-KS / MME audio endpoints).
  - Loads `pocket_amp_emu.dll` (compiled from all 24 C firmware modules) via ctypes.
  - Streams real-time 44.1 kHz audio (128-sample blocks) through the C firmware's 8-slot DSP chain,
    0x01EE0400 hardware accelerator model, 16-unit Q21 GRU NAM engine, and Q18 FIR Cab convolver.
  - Hosts the interactive HTML5 Front Panel UI at http://localhost:8080 with live .nam and .wav IR hot-loading.
"""

import os
# Enable ASIO in sounddevice BEFORE importing sounddevice
os.environ["SD_ENABLE_ASIO"] = "1"

import base64
import ctypes
import json
import math
import sys
import threading
import time
import wave
import socket
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import numpy as np
import sounddevice as sd

from nam_loader import parse_nam_file, parse_wav_ir

EMU_DIR = Path(__file__).resolve().parent
DLL_PATH = EMU_DIR / "pocket_amp_emu.dll"
UI_HTML_PATH = EMU_DIR / "ui.html"

SAMPLE_RATE = 44100
BLOCK_SIZE = 256
# ASIO buffer the server asks for (frames). 0 = the driver's own size. See start_audio_stream.
ASIO_BLOCK = int(os.environ.get("PA_EMU_ASIO_BLOCK", "128"))


class EmuTelemetry(ctypes.Structure):
    _fields_ = [
        ("view", ctypes.c_uint32),
        ("preset_index", ctypes.c_uint32),
        ("preset_volume", ctypes.c_uint32),
        ("master_volume", ctypes.c_uint32),
        ("selected_slot", ctypes.c_uint32),
        ("selected_param", ctypes.c_uint32),
        ("slot_active_mask", ctypes.c_uint32),
        ("sram_used_bytes", ctypes.c_uint32),
        ("sram_peak_bytes", ctypes.c_uint32),
        ("cpu_total_permille", ctypes.c_uint32),
        ("cpu_stage_permille", ctypes.c_uint32 * 8),
        ("tuner_active", ctypes.c_uint32),
        ("tuner_locked", ctypes.c_uint32),
        ("tuner_note", ctypes.c_uint32),
        ("tuner_octave", ctypes.c_int32),
        ("tuner_cents", ctypes.c_int32),
        ("tuner_freq_tenths", ctypes.c_uint32),
        ("tuner_dots", ctypes.c_uint32),
        ("drum_playing", ctypes.c_uint32),
        ("drum_bpm", ctypes.c_uint32),
        ("drum_pattern", ctypes.c_uint32),
        ("in_peak", ctypes.c_float),
        ("out_peak", ctypes.c_float),
        ("bt_state", ctypes.c_uint32),
        ("cursor_slot", ctypes.c_uint32),
        ("editor_cursor", ctypes.c_uint32),
    ]


# Load native C firmware DLL
dll = ctypes.CDLL(str(DLL_PATH))

dll.emu_init.argtypes = []
dll.emu_init.restype = None

dll.emu_set_bt_state.argtypes = [ctypes.c_int]
dll.emu_set_bt_state.restype = None

dll.emu_get_bt_state.argtypes = []
dll.emu_get_bt_state.restype = ctypes.c_int

dll.emu_process_audio_f32.argtypes = [
    ctypes.POINTER(ctypes.c_float),
    ctypes.POINTER(ctypes.c_float),
    ctypes.POINTER(ctypes.c_float),
    ctypes.c_int,
]
dll.emu_process_audio_f32.restype = None

dll.emu_ui_input.argtypes = [ctypes.c_uint32, ctypes.c_int32, ctypes.c_uint16]
dll.emu_ui_input.restype = None

# The press / release api. The firmware decides a PARA or HOME click from the
# RELEASE edge, so a button that is never released never clicks and never
# holds - that is what left half of the emulator's front panel dead.
dll.emu_ui_press.argtypes = [ctypes.c_uint32]
dll.emu_ui_press.restype = None
dll.emu_ui_release.argtypes = [ctypes.c_uint32]
dll.emu_ui_release.restype = None
dll.emu_ui_tap.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
dll.emu_ui_tap.restype = None
dll.emu_ui_enc.argtypes = [ctypes.c_int32]
dll.emu_ui_enc.restype = None
dll.emu_ui_pot.argtypes = [ctypes.c_uint16]
dll.emu_ui_pot.restype = None

dll.emu_get_lcd_rgba.argtypes = [ctypes.POINTER(ctypes.c_uint8)]
dll.emu_get_lcd_rgba.restype = None

dll.emu_load_nam_struct.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32]
dll.emu_load_nam_struct.restype = ctypes.c_int

dll.emu_register_user_nam.argtypes = [ctypes.c_char_p]
dll.emu_set_pending_nam_meta.argtypes = [ctypes.c_float, ctypes.c_float, ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint8]
dll.emu_set_pending_nam_meta.restype = None


def push_nam_meta(meta):
    """IMP-01: hand the importer's recommended AMP settings to the firmware registry."""
    knobs = (ctypes.c_uint8 * 6)(*[int(k) for k in meta.get("amp_knobs", [50] * 6)])
    dll.emu_set_pending_nam_meta(float(meta.get("input_trim", 1.0) or 1.0),
                                 float(meta.get("makeup_db", 0.0) or 0.0), knobs,
                                 1 if meta.get("approximate") else 0)
dll.emu_register_user_nam.restype = ctypes.c_int

dll.emu_select_amp_profile.argtypes = [ctypes.c_int]
dll.emu_select_amp_profile.restype = None

dll.emu_load_cab_ir_floats.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.c_uint32]
dll.emu_load_cab_ir_floats.restype = ctypes.c_int

dll.emu_set_slot_enabled.argtypes = [ctypes.c_int, ctypes.c_int]
dll.emu_set_slot_enabled.restype = None

dll.emu_set_master_volume.argtypes = [ctypes.c_int]
dll.emu_set_master_volume.restype = None

dll.emu_select_preset.argtypes = [ctypes.c_int]
dll.emu_select_preset.restype = None

dll.emu_set_slot_param.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
dll.emu_set_slot_param.restype = None

dll.emu_set_drums.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
dll.emu_set_drums.restype = None

dll.emu_feed_sysex_bytes.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32]
dll.emu_feed_sysex_bytes.restype = None

dll.emu_feed_ble_midi.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32]
dll.emu_feed_ble_midi.restype = None

dll.emu_get_telemetry.argtypes = [ctypes.POINTER(EmuTelemetry)]
dll.emu_get_telemetry.restype = None

dll.emu_get_scope.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float)]
dll.emu_get_scope.restype = None

dll.emu_select_stock_amp.argtypes = [ctypes.c_int]
dll.emu_select_stock_amp.restype = ctypes.c_int

dll.emu_select_stock_cab.argtypes = [ctypes.c_int]
dll.emu_select_stock_cab.restype = ctypes.c_int

dll.emu_get_stock_amp_name.argtypes = [ctypes.c_int]
dll.emu_get_stock_amp_name.restype = ctypes.c_char_p

dll.emu_get_stock_cab_name.argtypes = [ctypes.c_int]
dll.emu_get_stock_cab_name.restype = ctypes.c_char_p

dll.emu_get_user_nam_count.argtypes = []
dll.emu_get_user_nam_count.restype = ctypes.c_int

dll.emu_get_user_nam_name.argtypes = [ctypes.c_int]
dll.emu_get_user_nam_name.restype = ctypes.c_char_p

dll.emu_get_total_amp_count.argtypes = []
dll.emu_get_total_amp_count.restype = ctypes.c_int

dll.emu_select_user_nam.argtypes = [ctypes.c_int]
dll.emu_select_user_nam.restype = ctypes.c_int

dll.emu_set_wavenet_voicing.argtypes = [ctypes.c_int]
dll.emu_set_wavenet_voicing.restype = None

dll.emu_get_wavenet_voicing.argtypes = []
dll.emu_get_wavenet_voicing.restype = ctypes.c_int

dll.emu_get_slot_type.argtypes = [ctypes.c_int]
dll.emu_get_slot_type.restype = ctypes.c_int

dll.emu_get_slot_param.argtypes = [ctypes.c_int, ctypes.c_int]
dll.emu_get_slot_param.restype = ctypes.c_int


# Initialize firmware state
emu_lock = threading.Lock()
with emu_lock:
    dll.emu_init()

# Sample Player & Guitar DI Loops.
# The test audio and the IR bank are the project's own, synthesised by
# custom-fw/tools/gen_test_audio.py on first start (emu/samples/ and emu/irs/ are
plucked-string DI phrases, modelled cabinets, plate and spring reverbs.
sys.path.insert(0, str(EMU_DIR.parent / "tools"))
try:
    import gen_test_audio
    gen_test_audio.ensure_emu_assets(str(EMU_DIR), log=lambda s: print(s, flush=True))
except Exception as e:  # noqa: BLE001 - the emulator still runs without them
    print(f"[test audio] could not generate the test files: {e}", flush=True)
SAMPLE_DIR = EMU_DIR / "samples"
g_loaded_samples = {}
g_test_mode = "live"  # Default to live instrument input from audio interface
g_last_sample_mode = "di_clean"
g_sample_playing = False
g_sample_loop = True
g_sample_play_pos = 0
g_input_trim_lin = float(10.0 ** (6.0 / 20.0))  # +6 dB nominal instrument level (~2.0x)
SAMPLE_DI_TRIM_LIN = float(10.0 ** (-20.0 / 20.0))  # -20 dB trim for normalized DI samples to match live ASIO guitar level (~0.10x)
g_sample_clock = 0


def load_audio_samples():
    global g_loaded_samples
    samples = {
        # GuitarSet studio session multi-tracks (CC-BY 4.0)
        "gs_funk_comp": ("gs_funk_comp.wav", "GuitarSet Funk 16th Rhythm Chops (112 BPM, C#)"),
        "gs_funk_solo": ("gs_funk_solo.wav", "GuitarSet Funk Melodic Lead & Fills (114 BPM, Ab)"),
        "gs_jazz_comp": ("gs_jazz_comp.wav", "GuitarSet Jazz Swing 7th/9th Comping (130 BPM, D)"),
        "gs_jazz_solo": ("gs_jazz_solo.wav", "GuitarSet Jazz Bebop Melodic Runs (137 BPM, Eb)"),
        "gs_rock_comp": ("gs_rock_comp.wav", "GuitarSet Rock Arpeggiated Rhythm (85 BPM, F)"),
        "gs_rock_solo": ("gs_rock_solo.wav", "GuitarSet Rock Expressive Lead Solo (148 BPM, C)"),
        "gs_bossa_comp": ("gs_bossa_comp.wav", "GuitarSet Bossa Nova Fingerstyle (154 BPM, E)"),
        "gs_bossa_solo": ("gs_bossa_solo.wav", "GuitarSet Bossa Nova Melodic Lead (166 BPM, Ab)"),
        "gs_acoustic_strum": ("gs_acoustic_strum.wav", "GuitarSet Acoustic Open Strumming (107 BPM, Ab)"),
        "gs_acoustic_solo": ("gs_acoustic_solo.wav", "GuitarSet Acoustic Melodic Solo (100 BPM, C#)"),
        # synthesised DI phrases (tools/gen_test_audio.py)
        "di_clean": ("di_clean_chords.wav", "Clean strummed chords (G C D Em)"),
        "di_arp": ("di_arpeggio.wav", "Clean arpeggios (Am F C G)"),
        "di_rock": ("di_rock_riff.wav", "Rock riff, power chords + palm mutes"),
        "di_metal": ("di_metal_chug.wav", "Metal chugs, low E 16ths"),
        "di_blues": ("di_blues_lead.wav", "Blues lead line (A minor pentatonic)"),
        "di_bass": ("di_bass_line.wav", "Bass line (E G A, root-octave-fifth)"),
    }
    for key, (fname, label) in samples.items():
        fpath = SAMPLE_DIR / fname
        if fpath.exists():
            try:
                with wave.open(str(fpath), "rb") as wf:
                    sr = wf.getframerate()
                    raw = wf.readframes(wf.getnframes())
                    arr = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
                    if wf.getnchannels() > 1:
                        arr = arr.reshape(-1, wf.getnchannels())[:, 0]
                    g_loaded_samples[key] = {
                        "name": label,
                        "data": np.ascontiguousarray(arr, dtype=np.float32),
                        "len": len(arr),
                    }
                    print(f"[SamplePlayer] Loaded {label} ({len(arr)/sr:.2f}s, {len(arr)} samples)")
            except Exception as e:
                print(f"[SamplePlayer] Failed loading {fname}: {e}")


load_audio_samples()

RIFF_NOTES_HZ = [82.41, 98.00, 110.00, 123.47, 146.83, 123.47, 110.00, 98.00]


def synthesize_test_guitar(nframes: int) -> np.ndarray:
    global g_sample_clock
    t = (np.arange(nframes, dtype=np.float32) + g_sample_clock) / float(SAMPLE_RATE)
    g_sample_clock += nframes

    if g_test_mode == "a440":
        return (0.28 * np.sin(2.0 * np.pi * 440.0 * t)).astype(np.float32)
    if g_test_mode == "e2":
        f0 = 82.41
        sig = (
            0.25 * np.sin(2.0 * np.pi * f0 * t)
            + 0.12 * np.sin(2.0 * np.pi * (f0 * 2.0) * t)
            + 0.06 * np.sin(2.0 * np.pi * (f0 * 3.0) * t)
        )
        return sig.astype(np.float32)
    if g_test_mode == "riff":
        note_dur = int(SAMPLE_RATE * 0.28)
        idx = ((np.arange(nframes) + (g_sample_clock - nframes)) // note_dur) % len(RIFF_NOTES_HZ)
        pos_in_note = ((np.arange(nframes) + (g_sample_clock - nframes)) % note_dur) / float(SAMPLE_RATE)
        freqs = np.array([RIFF_NOTES_HZ[i] for i in idx], dtype=np.float32)
        env = np.exp(-5.5 * pos_in_note) * np.minimum(1.0, pos_in_note / 0.004)
        phase = 2.0 * np.pi * freqs * pos_in_note
        sig = env * (
            0.32 * np.sin(phase)
            + 0.18 * np.sin(2.0 * phase)
            + 0.10 * np.sin(3.0 * phase)
            + 0.05 * np.sin(4.0 * phase)
        )
        return sig.astype(np.float32)
    return np.zeros(nframes, dtype=np.float32)


# Live Input Routing & Calibration
g_input_channel = "auto"   # "auto", "ch1", "ch2", "mix"
g_sticky_channel = 0       # 0 = Ch 1 (L / Guitar), 1 = Ch 2 (R / Aux)
g_ch_hold_blocks = 0       # Hysteresis counter to prevent chatter


def get_next_audio_input(nframes: int, indata) -> np.ndarray:
    global g_sample_play_pos, g_sample_playing, g_sample_clock, g_sticky_channel, g_ch_hold_blocks

    if g_test_mode == "live":
        if indata is not None and indata.shape[1] >= 1:
            if g_input_channel == "ch1" or indata.shape[1] < 2:
                raw_in = indata[:, 0]
            elif g_input_channel == "ch2":
                raw_in = indata[:, 1]
            elif g_input_channel == "mix":
                raw_in = 0.5 * (indata[:, 0] + indata[:, 1])
            else:  # "auto": sticky lock to Input 1 (Instrument) with hysteresis (ZERO block-by-block chatter)
                pk0 = float(np.max(np.abs(indata[:, 0])))
                pk1 = float(np.max(np.abs(indata[:, 1])))
                # Only latch to Ch 2 if Ch 2 has sustained strong signal AND Ch 1 is quiet for >30 blocks
                if pk1 > 0.005 and pk1 > (pk0 * 3.0 + 0.001):
                    g_ch_hold_blocks += 1
                    if g_ch_hold_blocks > 30:
                        g_sticky_channel = 1
                elif pk0 > 0.002:
                    g_ch_hold_blocks = 0
                    g_sticky_channel = 0
                raw_in = indata[:, g_sticky_channel]

            mono_in = raw_in * g_input_trim_lin
            return np.ascontiguousarray(mono_in, dtype=np.float32)
        return np.zeros(nframes, dtype=np.float32)

    if g_test_mode in g_loaded_samples:
        if not g_sample_playing:
            return np.zeros(nframes, dtype=np.float32)
        sample = g_loaded_samples[g_test_mode]["data"]
        total_len = len(sample)
        if total_len == 0:
            return np.zeros(nframes, dtype=np.float32)

        start_pos = g_sample_play_pos
        if not g_sample_loop:
            end_pos = min(total_len, start_pos + nframes)
            chunk = np.zeros(nframes, dtype=np.float32)
            valid = end_pos - start_pos
            if valid > 0:
                chunk[:valid] = sample[start_pos:end_pos]
            g_sample_play_pos = end_pos
            if end_pos >= total_len:
                g_sample_playing = False
        else:
            indices = (np.arange(start_pos, start_pos + nframes)) % total_len
            chunk = sample[indices].copy()
            g_sample_play_pos = (start_pos + nframes) % total_len
            # Smooth 128-sample crossfade across loop boundary
            seam_dist = total_len - start_pos
            if 0 < seam_dist < nframes:
                xf = min(128, seam_dist, nframes - seam_dist)
                if xf > 0:
                    alpha = 0.5 * (1.0 - np.cos(np.linspace(0, np.pi, xf, dtype=np.float32)))
                    chunk[seam_dist - xf : seam_dist] = (
                        chunk[seam_dist - xf : seam_dist] * (1.0 - alpha)
                        + sample[:xf] * alpha
                    )
        return (chunk * g_input_trim_lin * SAMPLE_DI_TRIM_LIN).astype(np.float32)

    sig = synthesize_test_guitar(nframes) * g_input_trim_lin
    return np.ascontiguousarray(sig, dtype=np.float32)


# Audio Device Manager
g_stream = None
g_active_device_id = ""
g_audio_status = "Initializing..."
g_device_options = []


def discover_audio_options():
    global g_device_options
    apis = sd.query_hostapis()
    devs = sd.query_devices()
    opts = []

    # 1. ASIO Devices (Direct low-latency hardware streaming)
    for idx, d in enumerate(devs):
        api_name = apis[d["hostapi"]]["name"]
        if "ASIO" in api_name and d["max_input_channels"] > 0 and d["max_output_channels"] > 0:
            is_asio = "ASIO" in d["name"].upper()
            opts.append({
                "id": f"asio:{idx}:{idx}",
                "in_dev": idx,
                "out_dev": idx,
                "is_asio": True,
                "label": f"[*] {d['name']} [{api_name}] ({d['max_input_channels']} in / {d['max_output_channels']} out)",
                "priority": 0 if is_asio else 2,
            })

    # 2. Match Audio Input + Output pairs across DirectSound, WASAPI, WDM-KS, MME
    for api_idx, api in enumerate(apis):
        api_name = api["name"]
        if "ASIO" in api_name:
            continue
        ge_ins = [i for i in api["devices"] if any(k in devs[i]["name"].upper() for k in ["AUDIO", "USB", "CODEC"]) and devs[i]["max_input_channels"] > 0]
        ge_outs = [i for i in api["devices"] if any(k in devs[i]["name"].upper() for k in ["AUDIO", "USB", "CODEC"]) and devs[i]["max_output_channels"] > 0]
        for in_i in ge_ins:
            for out_i in ge_outs:
                if "DIRECTSOUND" in api_name.upper():
                    prio = 1
                elif "WASAPI" in api_name.upper():
                    prio = 3
                elif "WDM-KS" in api_name.upper():
                    prio = 4
                else:  # MME
                    prio = 5
                opts.append({
                    "id": f"pair:{in_i}:{out_i}",
                    "in_dev": in_i,
                    "out_dev": out_i,
                    "is_asio": False,
                    "label": f"{devs[in_i]['name']} -> {devs[out_i]['name']} [{api_name}]",
                    "priority": prio,
                })

    # 3. System Default Shared Audio (PC Output)
    opts.append({
        "id": "default",
        "in_dev": None,
        "out_dev": None,
        "is_asio": False,
        "label": "[PC Audio] Windows Default Output (Shared)",
        "priority": 10,
    })

    # 4. Safe Offline Simulation (Sound Card Free / PC Audio Safe)
    opts.append({
        "id": "offline",
        "in_dev": None,
        "out_dev": None,
        "is_asio": False,
        "label": "[Safe Offline] Simulation Only (No Audio Device)",
        "priority": 99,
    })

    opts.sort(key=lambda x: x["priority"])
    g_device_options = opts
    return opts


# Real-Time Zero-Allocation Audio Buffers (eliminates GC jitter and audio crackles)
MAX_AUDIO_FRAMES = 4096
_static_mono_in = np.zeros(MAX_AUDIO_FRAMES, dtype=np.float32)
_static_out_L = np.zeros(MAX_AUDIO_FRAMES, dtype=np.float32)
_static_out_R = np.zeros(MAX_AUDIO_FRAMES, dtype=np.float32)
_static_in_ptr = _static_mono_in.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
_static_out_l_ptr = _static_out_L.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
_static_out_r_ptr = _static_out_R.ctypes.data_as(ctypes.POINTER(ctypes.c_float))

_static_rgba = (ctypes.c_uint8 * (240 * 240 * 4))()
_static_telem = EmuTelemetry()
_static_scope_in = (ctypes.c_float * 256)()
_static_scope_out = (ctypes.c_float * 256)()

try:
    kernel32 = ctypes.windll.kernel32
    kernel32.GetCurrentProcess.restype = ctypes.c_void_p
    kernel32.SetPriorityClass.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), 0x00000080)  # HIGH_PRIORITY_CLASS
    print("[Audio Engine] Process priority elevated to HIGH_PRIORITY_CLASS (Real-time DMA safety)", flush=True)
except Exception:
    pass


# IMP-03: the firmware DSP (filters, tuner, delay times) assumes 44.1 kHz. At 48 kHz the old
# emulator ran it 8.8 % fast (tuner off by ~1.5 semitones, all filter corners shifted).
g_dev_rate = SAMPLE_RATE
_rs_in = _rs_out = None
def set_resampler(dev_rate):
    global g_dev_rate, _rs_in, _rs_out
    g_dev_rate = dev_rate
    _rs_in = _rs_out = None
    if dev_rate != SAMPLE_RATE:
        try:
            import soxr                                   # pip install soxr (streaming, low latency)
            _rs_in = soxr.ResampleStream(dev_rate, SAMPLE_RATE, 1, dtype="float32", quality="LQ")
            _rs_out = soxr.ResampleStream(SAMPLE_RATE, dev_rate, 2, dtype="float32", quality="LQ")
            _rs_prime()
        except ImportError:
            print("[Audio] install 'soxr' for correct-pitch operation at", dev_rate, "Hz", flush=True)


def audio_callback(indata, outdata, frames, time_info, status):
    if status:
        print(f"[Audio Stream Warning] {status}", flush=True)

    n = min(frames, MAX_AUDIO_FRAMES)
    sig = get_next_audio_input(n, indata)
    _static_mono_in[:n] = sig[:n]

    if _rs_in is not None:                                  # IMP-03: device rate != 44.1 kHz
        _process_resampled(_static_mono_in[:n].copy(), outdata, n)
        return

    with emu_lock:
        dll.emu_process_audio_f32(_static_in_ptr, _static_out_l_ptr, _static_out_r_ptr, n)

    outdata[:n, 0] = _static_out_L[:n]
    if outdata.shape[1] >= 2:
        outdata[:n, 1] = _static_out_R[:n]


_rs_fifo = np.zeros((0, 2), dtype=np.float32)
_rs_underruns = 0
RS_CUSHION = 1024    # device-rate frames of silence queued ahead of the resampled output


def _rs_prime():
    """The two resamplers return a varying number of frames per call (and hold some back
    at the start), so an output FIFO with no reserve runs dry now and then, and every time
    it does the callback outputs zeros - a click. Start it with a fixed cushion instead:
    ~23 ms at 44.1/48 kHz, constant, which absorbs the variation."""
    global _rs_fifo, _rs_underruns
    _rs_fifo = np.zeros((RS_CUSHION, 2), dtype=np.float32)
    _rs_underruns = 0


def _process_resampled(mono_dev, outdata, n):
    """Device-rate in -> 44.1 kHz firmware DSP -> device-rate out, via a small output FIFO."""
    global _rs_fifo
    x = _rs_in.resample_chunk(mono_dev.astype(np.float32))
    m = len(x)
    if m:
        xin = np.ascontiguousarray(x, dtype=np.float32)
        yl = np.zeros(m, dtype=np.float32); yr = np.zeros(m, dtype=np.float32)
        with emu_lock:
            dll.emu_process_audio_f32(xin.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                                      yl.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                                      yr.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), m)
        y = _rs_out.resample_chunk(np.stack([yl, yr], axis=1))
        _rs_fifo = np.concatenate([_rs_fifo, y.astype(np.float32)])
    take = min(n, len(_rs_fifo))
    if take < n:
        global _rs_underruns
        _rs_underruns += 1
        if _rs_underruns in (1, 10, 100) or _rs_underruns % 1000 == 0:
            print(f"[Audio] resampler output ran dry {_rs_underruns}x - raise RS_CUSHION", flush=True)
    outdata[:n, :] = 0.0
    outdata[:take, 0] = _rs_fifo[:take, 0]
    if outdata.shape[1] >= 2:
        outdata[:take, 1] = _rs_fifo[:take, 1]
    _rs_fifo = _rs_fifo[take:]


def start_audio_stream(preferred_id: str = ""):
    global g_stream, g_active_device_id, g_audio_status
    if g_stream is not None:
        try:
            g_stream.stop()
            g_stream.close()
        except Exception:
            pass
        g_stream = None

    opts = discover_audio_options()
    if not preferred_id:
        preferred_id = opts[0]["id"] if opts else "offline"

    if preferred_id == "offline":
        g_active_device_id = "offline"
        g_audio_status = "Safe Simulation (Sound Card Free — PC Audio Untouched)"
        print(f"[Audio] {g_audio_status}", flush=True)
        return True

    target = next((o for o in opts if o["id"] == preferred_id), None)
    if not target:
        print(f"[Audio] Device ID '{preferred_id}' not found, falling back to top candidate", flush=True)
        target = opts[0] if opts else None
        if not target or target["id"] == "offline":
            g_active_device_id = "offline"
            g_audio_status = "Safe Simulation (Sound Card Free)"
            return False

    candidates = [target] + [o for o in opts if o["id"] != target["id"] and o["id"] != "offline"]

    for cand in candidates:
        try:
            is_asio = cand.get("is_asio", False)
            in_dev_info = sd.query_devices(cand["in_dev"]) if cand["in_dev"] is not None else None

            # IMP-03: never inherit the driver's current rate. default_samplerate of an ASIO device
            # is whatever its control panel last used (the ASIO driver may report 96 kHz after any app
            # opened it at 96k), and opening the stream there re-programs the hardware. The DSP is
            # 44.1 kHz native, so ask for 44.1 kHz explicitly, then 48 kHz, and check the result.
            target_sr = None
            for sr in (44100, 48000):
                try:
                    sd.check_input_settings(device=cand["in_dev"], samplerate=sr, channels=2, dtype="float32")
                    sd.check_output_settings(device=cand["out_dev"] if cand.get("out_dev") is not None else cand["in_dev"],
                                             samplerate=sr, channels=2, dtype="float32")
                    target_sr = sr
                    break
                except Exception:
                    continue
            if target_sr is None:
                raise RuntimeError("device accepts neither 44.1 kHz nor 48 kHz")
            if is_asio:
                dev_arg = cand["in_dev"]
                lat_arg = "low"
                # The audio callback is Python, so it competes with the web server for the
                # GIL. The driver's minimum buffer (often 32-64 frames, 0.7-1.5 ms) leaves no
                # margin and crackles; ask for PA_EMU_ASIO_BLOCK frames (default 128 =
                # 2.9 ms), and fall back to the driver's own size if it refuses.
                bs_try = [ASIO_BLOCK, 0] if ASIO_BLOCK else [0]
            else:
                dev_arg = (cand["in_dev"], cand["out_dev"]) if cand["in_dev"] is not None else None
                lat_arg = None
                bs_try = [512 if cand["id"] == "default" else 0]

            stream, last_err = None, None
            for bs in bs_try:
                try:
                    stream = sd.Stream(
                        samplerate=target_sr,
                        blocksize=bs,
                        device=dev_arg,
                        channels=(2, 2),
                        dtype="float32",
                        latency=lat_arg,
                        callback=audio_callback,
                    )
                    break
                except Exception as e:
                    last_err = e
                    print(f"[Audio] {cand['label']}: blocksize {bs or 'native'} refused ({e})", flush=True)
            if stream is None:
                raise last_err
            stream.start()
            if int(round(stream.samplerate)) != target_sr:      # IMP-03: driver overrode us
                print(f"[Audio] WARNING driver runs at {stream.samplerate:.0f} Hz, requested {target_sr}", flush=True)
            set_resampler(int(round(stream.samplerate)))
            g_stream = stream
            g_active_device_id = cand["id"]
            actual_bs = stream.blocksize if stream.blocksize > 0 else "Native"
            g_audio_status = f"Streaming @ {target_sr} Hz ({actual_bs}): {cand['label']}"
            print(f"[Audio] {g_audio_status}", flush=True)
            return True
        except Exception as e:
            print(f"[Audio] Candidate {cand['label']} failed: {e}", flush=True)

    g_active_device_id = "offline"
    g_audio_status = "Offline Simulation Loop (Hardware Stream Failed)"
    return False


# Background thread to pump DSP when no hardware stream is active
def fallback_dsp_worker():
    while True:
        if g_stream is None or not g_stream.active:
            mono_in = get_next_audio_input(BLOCK_SIZE, None)
            out_L = np.zeros(BLOCK_SIZE, dtype=np.float32)
            out_R = np.zeros(BLOCK_SIZE, dtype=np.float32)
            with emu_lock:
                dll.emu_process_audio_f32(
                    mono_in.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                    out_L.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                    out_R.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                    BLOCK_SIZE,
                )
        time.sleep(BLOCK_SIZE / float(SAMPLE_RATE))


AMP_PROFILE_NAMES = {
    0: "Fender Twin Clean (Q21 GRU-16)",
    1: "Vox AC30 Top Boost (Q21 GRU-16)",
    2: "Marshall JCM800 Crunch (Q21 GRU-16)",
    3: "Mesa Dual Rectifier (Q21 GRU-16)",
    4: "Peavey 5150 High-Gain (Q21 GRU-16)",
    5: "Ampeg SVT Bass (Q21 GRU-16)",
    6: "Bogner Ecstasy Lead (Q21 GRU-16)",
    7: "Soldano SLO-100 (Q21 GRU-16)",
    8: "Roland JC-120 Clean (Q21 GRU-16)",
    9: "Fender '59 Bassman (Q21 GRU-16)",
}


class EmuRequestHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # Keep console clean

    def _send_json(self, obj, code=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/" or self.path.startswith("/index"):
            html = UI_HTML_PATH.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(html)))
            self.end_headers()
            self.wfile.write(html)
            return

        if self.path.startswith("/onyx/"):
            screen_name = self.path[6:].split("?")[0]
            base_screens = (EMU_DIR.parent / "ui_design" / "screens").resolve()
            try:
                png_file = (base_screens / screen_name).resolve()
                if png_file.is_relative_to(base_screens) and png_file.is_file():
                    data = png_file.read_bytes()
                    self.send_response(200)
                    self.send_header("Content-Type", "image/png")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                    return
            except Exception:
                pass
            self.send_error(404, f"Screen {screen_name} not found")
            return

        if self.path == "/api/onyx_screens":
            screens_dir = (EMU_DIR.parent / "ui_design" / "screens").resolve()
            names = sorted([f.name for f in screens_dir.glob("*.png")]) if screens_dir.exists() else []
            self._send_json({"screens": names})
            return

        if self.path == "/api/devices":
            self._send_json({
                "devices": g_device_options,
                "active_id": g_active_device_id,
                "status": g_audio_status,
            })
            return

        if self.path == "/api/stock_assets":
            total_amps = dll.emu_get_total_amp_count()
            amps = [dll.emu_get_stock_amp_name(i).decode('latin1', 'ignore') for i in range(total_amps)]
            cabs = [dll.emu_get_stock_cab_name(i).decode('latin1', 'ignore') for i in range(20)]
            self._send_json({"amps": amps, "cabs": cabs, "total_amps": total_amps, "user_count": dll.emu_get_user_nam_count()})
            return

        if self.path == "/api/lcd":
            # The screen as raw RGBA (230,400 B), for the browser to copy straight into
            # its canvas. It used to travel inside /api/state as 307 KB of base64 in JSON,
            # 20 times a second: ~1.4 ms of Python per poll holding the GIL - and the audio
            # callback is Python too, so it waited (crackles at small buffer sizes).
            rgba = (ctypes.c_uint8 * (240 * 240 * 4))()
            dll.emu_get_lcd_rgba(rgba)
            data = bytes(rgba)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)
            return

        if self.path.startswith("/api/state"):
            # Lock-free display telemetry reads to prevent audio callback jitter.
            # emu_get_lcd_rgba() also runs the idle tick when no audio is streaming, so it
            # is still called here; the pixels themselves go out on /api/lcd. Old clients
            # can still ask for them inline with /api/state?lcd=1.
            telem = EmuTelemetry()
            scope_in = (ctypes.c_float * 256)()
            scope_out = (ctypes.c_float * 256)()
            lcd_b64 = ""
            if "lcd=1" in self.path:
                req_rgba = (ctypes.c_uint8 * (240 * 240 * 4))()
                dll.emu_get_lcd_rgba(req_rgba)
                lcd_b64 = base64.b64encode(bytes(req_rgba)).decode("ascii")
            dll.emu_get_telemetry(ctypes.byref(telem))
            dll.emu_get_scope(scope_in, scope_out)

            cur_sample = g_loaded_samples.get(g_test_mode)
            sample_pct = 0.0
            if cur_sample and cur_sample["len"] > 0:
                sample_pct = round((g_sample_play_pos / cur_sample["len"]) * 100.0, 1)

            self._send_json({
                "audio_status": g_audio_status,
                "active_device_id": g_active_device_id,
                "test_mode": g_test_mode,
                "is_live_input": (g_test_mode == "live"),
                "last_sample_mode": g_last_sample_mode,
                "sample_playing": g_sample_playing,
                "sample_loop": g_sample_loop,
                "sample_pos_pct": sample_pct,
                "input_channel": g_input_channel,
                "sticky_channel": g_sticky_channel,
                "input_trim_db": round(20.0 * math.log10(max(1e-6, g_input_trim_lin)), 1),
                "wavenet_voicing": dll.emu_get_wavenet_voicing(),
                "lcd_b64": lcd_b64,
                "telemetry": {
                    "view": telem.view,
                    "preset_index": telem.preset_index,
                    "preset_volume": telem.preset_volume,
                    "master_volume": telem.master_volume,
                    "selected_slot": telem.selected_slot,
                    "selected_param": telem.selected_param,
                    "cursor_slot": telem.cursor_slot,
                    "editor_cursor": telem.editor_cursor,
                    "slot_active_mask": telem.slot_active_mask,
                    "sram_used_bytes": telem.sram_used_bytes,
                    "sram_peak_bytes": telem.sram_peak_bytes,
                    "cpu_total_permille": telem.cpu_total_permille,
                    "cpu_stage_permille": [telem.cpu_stage_permille[i] for i in range(8)],
                    "tuner_active": telem.tuner_active,
                    "tuner_locked": telem.tuner_locked,
                    "tuner_note": telem.tuner_note,
                    "tuner_octave": telem.tuner_octave,
                    "tuner_cents": telem.tuner_cents,
                    "tuner_freq_tenths": telem.tuner_freq_tenths,
                    "tuner_dots": telem.tuner_dots,
                    "drum_playing": telem.drum_playing,
                    "drum_bpm": telem.drum_bpm,
                    "drum_pattern": telem.drum_pattern,
                    "in_peak": float(telem.in_peak),
                    "out_peak": float(telem.out_peak),
                    "bt_state": int(telem.bt_state),
                },
                "slot_types": [dll.emu_get_slot_type(s) for s in range(8)],
                "slot_params": [[dll.emu_get_slot_param(s, p) for p in range(6)] for s in range(8)],
                "scope_in": [round(float(scope_in[i]), 4) for i in range(0, 256, 2)],
                "scope_out": [round(float(scope_out[i]), 4) for i in range(0, 256, 2)],
            })
            return

        if self.path == "/api/telemetry":
            telem = EmuTelemetry()
            dll.emu_get_telemetry(ctypes.byref(telem))
            cpu_pct = round(telem.cpu_total_permille / 10.0, 1)
            self._send_json({
                "cpu_pct": cpu_pct,
                "preset_idx": telem.preset_index + 1,
                "sram_used": telem.sram_used_bytes,
                "drum_playing": bool(telem.drum_playing),
                "drum_bpm": telem.drum_bpm,
                "drum_pattern": telem.drum_pattern,
            })
            return

        self.send_error(404)

    MAX_PAYLOAD_BYTES = 16 * 1024 * 1024  # 16 MB limit

    def do_POST(self):
        global g_test_mode, g_input_trim_lin, g_sample_playing, g_sample_loop, g_sample_play_pos, g_last_sample_mode, g_input_channel

        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._send_json({"ok": False, "error": "Invalid Content-Length"}, code=400)
            return

        if length < 0 or length > self.MAX_PAYLOAD_BYTES:
            self._send_json({"ok": False, "error": "Payload too large"}, code=413)
            return

        if self.path in ("/api/load_nam", "/api/load_ir"):
            raw_bytes = self.rfile.read(length) if length > 0 else b""
            fname = self.headers.get("X-Filename", "uploaded")

            try:
                if self.path == "/api/load_nam":
                    blob, meta = parse_nam_file(raw_bytes, fname)
                    c_buf = (ctypes.c_uint8 * len(blob)).from_buffer_copy(blob)
                    with emu_lock:
                        dll.emu_load_nam_struct(c_buf, len(blob))
                        if "voicing_id" in meta:
                            dll.emu_set_wavenet_voicing(int(meta["voicing_id"]))
                        clean_name = meta.get("name") or fname
                        push_nam_meta(meta)
                        # The amp id of the capture. It is NOT necessarily the
                        # last one in the list: loading a capture a second time
                        # overwrites the entry with the same name in place.
                        amp_id = dll.emu_register_user_nam(clean_name.encode("utf-8"))
                        total_amps = dll.emu_get_total_amp_count()
                        user_count = dll.emu_get_user_nam_count()
                    self._send_json({"ok": True, "meta": meta, "amp_id": amp_id,
                                     "total_amps": total_amps, "user_count": user_count})
                else:
                    taps, meta = parse_wav_ir(raw_bytes, max_taps=1024)
                    c_taps = (ctypes.c_float * len(taps))(*taps)
                    with emu_lock:
                        dll.emu_load_cab_ir_floats(c_taps, len(taps))
                    self._send_json({"ok": True, "meta": meta})
            except Exception as e:
                self._send_json({"ok": False, "error": str(e)}, code=400)
            return

        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length).decode("utf-8")) if length > 0 else {}

        if self.path == "/api/load_sample_nam":
            sample_file = body.get("filename", "test_capture.nam")   # generated, gen_test_audio.py
            base_emu = EMU_DIR.resolve()
            try:
                p = (base_emu / sample_file).resolve()
                valid = p.is_relative_to(base_emu) and p.is_file()
            except Exception:
                valid = False
            if not valid:
                self._send_json({"ok": False, "error": f"Sample file {sample_file} not found"}, code=404)
                return
            try:
                raw_bytes = p.read_bytes()
                blob, meta = parse_nam_file(raw_bytes, sample_file)
                c_buf = (ctypes.c_uint8 * len(blob)).from_buffer_copy(blob)
                with emu_lock:
                    dll.emu_load_nam_struct(c_buf, len(blob))
                    if "voicing_id" in meta:
                        dll.emu_set_wavenet_voicing(int(meta["voicing_id"]))
                    clean_name = meta.get("name") or sample_file
                    push_nam_meta(meta)
                    # The amp id of the capture. It is NOT necessarily the
                    # last one in the list: loading a capture a second time
                    # overwrites the entry with the same name in place.
                    amp_id = dll.emu_register_user_nam(clean_name.encode("utf-8"))
                    total_amps = dll.emu_get_total_amp_count()
                    user_count = dll.emu_get_user_nam_count()
                self._send_json({"ok": True, "meta": meta, "amp_id": amp_id,
                                 "total_amps": total_amps, "user_count": user_count})
            except Exception as e:
                self._send_json({"ok": False, "error": str(e)}, code=400)
            return

        if self.path == "/api/input":
            btns = int(body.get("buttons", 0))
            enc = int(body.get("enc_delta", 0))
            pot = int(body.get("master_pot", 0xFFFF))
            with emu_lock:
                if "preset_select" in body:
                    p_sel = int(body["preset_select"]) - 1
                    dll.emu_select_preset(p_sel)
                if "drums_playing" in body or "drum_style" in body or "drum_bpm" in body:
                    dll.emu_get_telemetry(ctypes.byref(_static_telem))
                    cur_play = _static_telem.drum_playing
                    cur_bpm = _static_telem.drum_bpm
                    cur_pat = _static_telem.drum_pattern
                    play_val = 1 if body.get("drums_playing", cur_play) else 0
                    bpm_val = int(body.get("drum_bpm", cur_bpm if cur_bpm > 0 else 120))
                    pat_val = int(body.get("drum_style", cur_pat))
                    dll.emu_set_drums(play_val, bpm_val, pat_val)
                # The press / release api: each field changes ONE thing, so the
                # encoder and the master pot no longer release the button that
                # is down. "buttons" is the old LEVEL api and is kept for the
                # host tests, which drive a press and a release themselves.
                legacy = "buttons" in body and not ("press" in body or "release" in body)
                if legacy:
                    dll.emu_ui_input(btns, enc, pot)
                else:
                    # A complete gesture: one request is one press-and-release,
                    # so the two halves can never be reordered or lost.
                    if "tap" in body:
                        dll.emu_ui_tap(int(body["tap"]), 0)
                    elif "hold" in body:
                        dll.emu_ui_tap(int(body["hold"]), int(body.get("ms", 0)))
                    else:
                        if "press" in body:
                            dll.emu_ui_press(int(body["press"]))
                        if "release" in body:
                            dll.emu_ui_release(int(body["release"]))
                    if enc:
                        dll.emu_ui_enc(enc)
                    if "master_pot" in body:
                        dll.emu_ui_pot(pot)
                if "master_vol" in body:
                    dll.emu_set_master_volume(int(body["master_vol"]))
            self._send_json({"ok": True})
            return

        if self.path == "/api/sysex":
            hex_str = body.get("hex", "")
            raw = bytes.fromhex(hex_str)
            if raw and dll:
                c_bytes = (ctypes.c_uint8 * len(raw))(*raw)
                with emu_lock:
                    dll.emu_feed_sysex_bytes(c_bytes, len(raw))
            self._send_json({"ok": True, "bytes_fed": len(raw)})
            return

        if self.path == "/api/ble_midi":
            # A BLE-MIDI 1.0 packet, hex encoded. The framing is stripped
            # inside the firmware, so this is what a real BLE peer sends.
            hex_str = body.get("hex", "")
            raw = bytes.fromhex(hex_str)
            if raw and dll:
                c_bytes = (ctypes.c_uint8 * len(raw))(*raw)
                with emu_lock:
                    dll.emu_feed_ble_midi(c_bytes, len(raw))
            self._send_json({"ok": True, "bytes_fed": len(raw)})
            return

        if self.path == "/api/preset":
            p = int(body.get("preset", 0))
            with emu_lock:
                dll.emu_select_preset(p)
            self._send_json({"ok": True, "preset": p})
            return

        if self.path == "/api/master_volume":
            vol = int(body.get("volume", 80))
            with emu_lock:
                dll.emu_set_master_volume(vol)
            self._send_json({"ok": True, "volume": vol})
            return

        if self.path == "/api/slot":
            slot = int(body.get("slot", 0))
            en = int(body.get("enabled", 1))
            with emu_lock:
                dll.emu_set_slot_enabled(slot, en)
            self._send_json({"ok": True})
            return

        if self.path == "/api/param":
            slot = int(body.get("slot", 0))
            pid = int(body.get("param_id", 0))
            val = int(body.get("val", 50))
            with emu_lock:
                dll.emu_set_slot_param(slot, pid, val)
            self._send_json({"ok": True})
            return

        if self.path == "/api/drums":
            playing = int(body.get("playing", 0))
            bpm = int(body.get("bpm", 120))
            pat = int(body.get("pattern", 0))
            with emu_lock:
                dll.emu_set_drums(playing, bpm, pat)
            self._send_json({"ok": True})
            return

        if self.path == "/api/amp_profile":
            pid = int(body.get("profile_id", 2))
            with emu_lock:
                dll.emu_select_amp_profile(pid)
            self._send_json({"ok": True, "profile_id": pid})
            return

        if self.path == "/api/bluetooth":
            state = int(body.get("state", 1))
            with emu_lock:
                dll.emu_set_bt_state(state)
            self._send_json({"ok": True, "state": state})
            return

        if self.path == "/api/stock_amp":
            aid = int(body.get("id", 0))
            with emu_lock:
                dll.emu_select_stock_amp(aid)
            nm = dll.emu_get_stock_amp_name(aid).decode('latin1')
            self._send_json({"ok": True, "id": aid, "name": nm})
            return

        if self.path == "/api/voicing":
            profile = int(body.get("profile", 0))
            with emu_lock:
                dll.emu_set_wavenet_voicing(profile)
            self._send_json({"ok": True, "voicing": profile})
            return

        if self.path == "/api/stock_cab":
            cid = int(body.get("id", 0))
            with emu_lock:
                dll.emu_select_stock_cab(cid)
            nm = dll.emu_get_stock_cab_name(cid).decode('latin1')
            self._send_json({"ok": True, "id": cid, "name": nm})
            return

        if self.path == "/api/set_device":
            dev_id = body.get("id", "")
            start_audio_stream(dev_id)
            self._send_json({"ok": True, "status": g_audio_status, "active_id": g_active_device_id})
            return

        if self.path in ("/api/test_signal", "/api/sample_player"):
            if "action" in body and body["action"] == "toggle_input":
                if g_test_mode == "live":
                    g_test_mode = g_last_sample_mode if g_last_sample_mode in g_loaded_samples else "di_clean"
                    g_sample_playing = True
                else:
                    g_last_sample_mode = g_test_mode
                    g_test_mode = "live"
                    g_sample_playing = False
            elif "mode" in body:
                mode = body["mode"]
                if mode == "live":
                    if g_test_mode != "live":
                        g_last_sample_mode = g_test_mode
                    g_test_mode = "live"
                    g_sample_playing = False
                elif mode == "sample":
                    g_test_mode = g_last_sample_mode if g_last_sample_mode in g_loaded_samples else "di_clean"
                    g_sample_playing = True
                else:
                    g_test_mode = mode
                    if g_test_mode in g_loaded_samples:
                        g_last_sample_mode = g_test_mode
                        g_sample_playing = True
            if "playing" in body and g_test_mode != "live":
                g_sample_playing = bool(body["playing"])
            if "restart" in body and body["restart"]:
                g_sample_play_pos = 0
                g_sample_playing = True
            if "loop" in body:
                g_sample_loop = bool(body["loop"])
            if "input_channel" in body:
                g_input_channel = str(body["input_channel"]).lower()
            if "trim_db" in body:
                try:
                    trim_db = float(body["trim_db"])
                    if math.isfinite(trim_db):
                        trim_db = max(-60.0, min(24.0, trim_db))
                        g_input_trim_lin = float(10.0 ** (trim_db / 20.0))
                except (ValueError, OverflowError):
                    pass
            self._send_json({
                "ok": True,
                "mode": g_test_mode,
                "is_live_input": (g_test_mode == "live"),
                "last_sample_mode": g_last_sample_mode,
                "playing": g_sample_playing,
                "loop": g_sample_loop,
                "input_channel": g_input_channel,
                "sticky_channel": g_sticky_channel,
                "trim_db": round(20.0 * math.log10(max(1e-6, g_input_trim_lin)), 1),
            })
            return

        if self.path == "/api/ir_bank":
            # the IR bank: the synthesised cabinets and reverbs in emu/irs/
            ir_name = "".join(ch for ch in str(body.get("name", "cab_4x12_closed"))
                              if ch.isalnum() or ch == "_")
            ir_file = EMU_DIR / "irs" / f"{ir_name}.wav"
            if ir_file.exists():
                try:
                    taps, meta = parse_wav_ir(ir_file.read_bytes(), max_taps=1024)
                    c_taps = (ctypes.c_float * len(taps))(*taps)
                    with emu_lock:
                        dll.emu_load_cab_ir_floats(c_taps, len(taps))
                    self._send_json({"ok": True, "name": ir_name, "taps": len(taps), "meta": meta})
                    return
                except Exception as e:
                    self._send_json({"ok": False, "error": str(e)}, code=400)
                    return
            self._send_json({"ok": False, "error": f"IR {ir_name} not found"}, code=404)
            return

        if self.path == "/api/slot_type":
            slot = int(body.get("slot", 0))
            type_id = int(body.get("type", 0))
            with emu_lock:
                dll.emu_set_slot_param(slot, 0, type_id)
            self._send_json({"ok": True, "slot": slot, "type": type_id})
            return

        self.send_error(404)


class DualStackServer(ThreadingHTTPServer):
    address_family = socket.AF_INET6

    def server_bind(self):
        try:
            self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        except (AttributeError, OSError):
            pass
        super().server_bind()


def main():
    # --port N and --host ADDR let the launchers customize binding.
    # Default is loopback-only (127.0.0.1) for local security.
    port = 8080
    host = "127.0.0.1"
    argv = sys.argv[1:]
    if "--help" in argv or "-h" in argv:
        print("usage: server.py [--port N] [--host ADDR]   (default: 127.0.0.1:8080)")
        return
    if "--port" in argv:
        i = argv.index("--port")
        if i + 1 >= len(argv):
            print("server.py: --port needs a number"); sys.exit(2)
        port = int(argv[i + 1])
    if "--host" in argv:
        i = argv.index("--host")
        if i + 1 >= len(argv):
            print("server.py: --host needs an IP address"); sys.exit(2)
        host = argv[i + 1]

    # Pre-load the default cabinet (the synthesised closed 4x12, tools/gen_test_audio.py)
    sample_ir = EMU_DIR / "default_cab_ir.wav"
    if sample_ir.exists():
        try:
            with open(sample_ir, "rb") as f:
                taps, meta = parse_wav_ir(f.read(), max_taps=1024)
            c_taps = (ctypes.c_float * len(taps))(*taps)
            with emu_lock:
                dll.emu_load_cab_ir_floats(c_taps, len(taps))
            print(f"[CAB] Preloaded default IR: {sample_ir.name} ({len(taps)} taps)")
        except Exception as e:
            print(f"[CAB] Warning loading default IR: {e}")

    start_audio_stream()
    threading.Thread(target=fallback_dsp_worker, daemon=True).start()

    if host == "::":
        try:
            srv = DualStackServer(("::", port), EmuRequestHandler)
        except Exception:
            srv = ThreadingHTTPServer(("0.0.0.0", port), EmuRequestHandler)
    else:
        srv = ThreadingHTTPServer((host, port), EmuRequestHandler)

    print(f"==================================================================")
    print(f" M-VAVE Pocket Amp Live Firmware Emulator Running!")
    print(f" Audio Status : {g_audio_status}")
    print(f" Open Web UI  : http://{host}:{port}")
    print(f"==================================================================")
    srv.serve_forever()


if __name__ == "__main__":
    main()
