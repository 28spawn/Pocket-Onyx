// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 The Onyx Authors (@28spawn)
/* The DRIVE block on every kind of preset, through the real UI.
 *
 * Bug report: "on presets 41-80 the drive leads to no audio, on presets 1-40 it works".
 * This walks the device's own controls: press < or > on the home screen to reach the
 * preset, open the DRV block, switch it on, turn its knobs - and measures the jack output
 * at each step. Every step must make sound.
 *
 *   node custom-fw/wasm/test_preset_drive.js [pocket_amp.wasm]
 */
const fs = require('fs');
const path = require('path');

const wasmPath = process.argv[2] || path.join(__dirname, 'out', 'pocket_amp.wasm');
const mem = new WebAssembly.Memory({ initial: 512, maximum: 4096 });
const imp = {
  env: { memory: mem,
    __indirect_function_table: new WebAssembly.Table({ initial: 1024, element: 'anyfunc' }),
    __stack_pointer: new WebAssembly.Global({ value: 'i32', mutable: true }, 16 * 1024 * 1024),
    __memory_base: new WebAssembly.Global({ value: 'i32', mutable: false }, 0),
    __table_base:  new WebAssembly.Global({ value: 'i32', mutable: false }, 0),
    __main_argc_argv: () => 0 },
  wasi_snapshot_preview1: { args_get: () => 0, args_sizes_get: () => 0, proc_exit: () => 0,
    fd_write: () => 0, environ_get: () => 0, environ_sizes_get: () => 0, clock_time_get: () => 0, random_get: () => 0 },
};
const X = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(wasmPath)), imp).exports;
X.pa_wasm_init();

const T = new Uint32Array(mem.buffer, X.pa_wasm_telemetry_ptr(), 32);
const J = X.pa_wasm_jack_frames();
const jackIn  = new Int32Array(mem.buffer, X.pa_wasm_jack_in_ptr(),  J * 2);
const jackOut = new Int32Array(mem.buffer, X.pa_wasm_jack_out_ptr(), J * 2);
let ph = 0;

/* one UI tick and one audio block, the way the device interleaves them */
function step(n = 1) {
  for (let i = 0; i < n; i++) { X.pa_wasm_tick(16); pump(1); }
  X.pa_wasm_update_telemetry();
}
function pump(blocks) {
  let acc = 0, cnt = 0;
  for (let b = 0; b < blocks; b++) {
    for (let i = 0; i < J; i++) {
      const s = Math.round(0.1 * Math.sin(2 * Math.PI * 220 * ph / 44100) * 8388607);
      jackIn[2 * i] = s & 0xFFFFFF; jackIn[2 * i + 1] = 0; ph++;
    }
    X.pa_wasm_audio_process();
    for (let i = 0; i < J; i++) { const v = ((jackOut[2 * i] << 8) >> 8) / 8388607; acc += v * v; cnt++; }
  }
  return cnt ? 10 * Math.log10(acc / cnt + 1e-24) : -240;
}
const level = () => { step(20); return pump(200); };
const tap = (mask) => { X.pa_wasm_press(mask); step(3); X.pa_wasm_release(mask); step(6); };
const enc = (d) => { X.pa_wasm_add_enc(d); step(4); };
const LEFT = 1 << 3, RIGHT = 1 << 4, PARA = 1 << 5, HOME = 1 << 0;
const TEL = { view: 0, preset: 1, slot_mask: 5, cursor_slot: 18, editor_cursor: 19 };

let fails = 0;
const check = (ok, msg) => { console.log((ok ? '  ok   ' : '  FAIL ') + msg); if (!ok) fails++; };

const TARGETS = (process.env.TARGETS || "1,2,40,41,42,80").split(",").map(Number);
for (const target of TARGETS) {
  X.pa_wasm_init(); step(30);
  /* the < and > buttons pick the preset on the home screen (the PARA knob moves the block cursor since the 2026-11-03 swap); the shorter way round, and a cap so a changed control cannot hang the gate */
  let guard = 0;
  while (T[TEL.preset] + 1 !== target) {
    const d = (((target - 1 - T[TEL.preset]) % 80) + 80) % 80;
    tap(d <= 40 ? RIGHT : LEFT);
    if (++guard > 200) { check(false, `P${target}: the < > buttons did not reach the preset`); break; }
  }
  const bypass = level();
  const mask0 = T[TEL.slot_mask];
  for (let i = 0; i < 10 && T[TEL.cursor_slot] !== 1; i++) enc(1);        /* to DRV: the PARA knob moves the block cursor */
  tap(PARA);                                                            /* its editor */
  enc(-1);                                                              /* model row -> ON/OFF */
  tap(PARA);                                                            /* switch it on */
  const on = level();
  const mask1 = T[TEL.slot_mask], sel = T[4];
  enc(2); tap(PARA); enc(3); tap(PARA);                                 /* GRIT +3 */
  enc(2); tap(PARA); enc(-2); tap(PARA);                                /* LEVEL -2 */
  const tweaked = level();
  tap(HOME);
  console.log(`P${String(target).padStart(2, '0')}: bypass ${bypass.toFixed(1)} dBFS, DRV on ${on.toFixed(1)}, ` +
              `after the knobs ${tweaked.toFixed(1)}  (selected slot ${sel}, active ${mask0.toString(2).padStart(8,'0')} -> ${mask1.toString(2).padStart(8,'0')})`);
  check(on > bypass - 12 && tweaked > bypass - 12,
        `P${target}: the drive makes sound when switched on and when its knobs are turned`);
}
console.log(fails ? `\n${fails} FAILURE(S)` : '\nDRIVE ON EVERY PRESET OK');
process.exit(fails ? 1 : 0);
