// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 The Onyx Authors (@28spawn)
/* Verify the JACK (the firmware's 24-bit audio I/O) in the WASM build.
 *
 * The browser's audio (on Windows: Windows Audio / WASAPI) is converted to
 * 24-bit at the edge and handed to the firmware as the jack input; the
 * firmware's jack output is converted back. So these checks are the ones that
 * matter for "the browser hears what the device would play":
 *
 *   - a signal in the jack comes out of the jack
 *   - the master volume knob scales the jack output (the firmware's own
 *     volume law, not the page's)
 *   - the jack is stereo (L and R are separate slots)
 *   - the jack output is 24-bit (the low 24 bits, sign extended)
 */
const fs = require('fs');
const wasmPath = process.argv[2];
const buf = fs.readFileSync(wasmPath);
const mem = new WebAssembly.Memory({ initial: 512, maximum: 4096 });
const imp = {
  env: { memory: mem,
    __indirect_function_table: new WebAssembly.Table({ initial: 1024, element: 'anyfunc' }),
    __stack_pointer: new WebAssembly.Global({ value: 'i32', mutable: true }, 16 * 1024 * 1024),
    __memory_base: new WebAssembly.Global({ value: 'i32', mutable: false }, 0),
    __table_base:  new WebAssembly.Global({ value: 'i32', mutable: false }, 0),
    __main_argc_argv: () => 0 },
  wasi_snapshot_preview1: { args_get: () => 0, args_sizes_get: () => 0, proc_exit: () => 0,
    fd_write: () => 0, environ_get: () => 0, environ_sizes_get: () => 0,
    clock_time_get: () => 0, random_get: () => 0 },
};
const X = new WebAssembly.Instance(new WebAssembly.Module(buf), imp).exports;
X.pa_wasm_init();

const J = X.pa_wasm_jack_frames();
const jackIn  = new Int32Array(mem.buffer, X.pa_wasm_jack_in_ptr(),  J * 2);
const jackOut = new Int32Array(mem.buffer, X.pa_wasm_jack_out_ptr(), J * 2);

let fails = 0;
const check = (ok, msg) => { console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${msg}`); if (!ok) fails++; };

/* Feed one jack quantum: a sine on the Left slot (where the guitar is on the
 * device), silence on the Right. */
function pushJack(amp) {
  for (let i = 0; i < J; i++) {
    const s24 = Math.round(amp * Math.sin(2 * Math.PI * 220 * i / J) * 8388607);
    jackIn[2 * i]     = s24 & 0x00FFFFFF;
    jackIn[2 * i + 1] = 0;
  }
  X.pa_wasm_audio_process();
}
function peakL() {
  let p = 0;
  for (let i = 0; i < J; i++) {
    const v = (jackOut[2 * i] << 8) >> 8;   // sign extend the 24-bit slot
    const a = v < 0 ? -v : v;
    if (a > p) p = a;
  }
  return p;
}
/* The cabinet convolution rings across the block boundary, so the peak of ONE
 * block drifts. Settle first, then take the largest peak over a run of
 * blocks - that is the signal's real peak. */
/* The RMS over a long run: what the MASTER law is checked on. (A peak wanders with a preset's
 * modulated delay and reverb - factory preset 1 has both.) */
function settledRms(settle, measure) {
  for (let k = 0; k < settle; k++) pushJack(0.5);
  let acc = 0, n = 0;
  for (let k = 0; k < measure; k++) {
    pushJack(0.5);
    for (let i = 0; i < J; i++) { const v = (jackOut[2 * i] << 8) >> 8; acc += v * v; n++; }
  }
  return Math.sqrt(acc / n);
}
function settledPeak(settle, measure) {
  for (let k = 0; k < settle; k++) pushJack(0.5);
  let p = 0;
  for (let k = 0; k < measure; k++) { pushJack(0.5); const q = peakL(); if (q > p) p = q; }
  return p;
}

console.log('=== the jack is 64 stereo frames (like the device\'s half-buffer) ===');
check(J === 64, `64 frames per jack quantum (got ${J})`);

console.log('\n=== a signal in the jack comes out of the jack ===');
const pk = settledPeak(200, 50);
/* no factory partition in the test build: the amp is the synthetic placeholder, so the level is low */
check(pk > 100, `the left slot carries signal (peak ${pk} of 8388607)`);

console.log('\n=== the right slot is a separate channel ===');
let same = 0;
for (let i = 0; i < J; i++) if (jackOut[2 * i] !== jackOut[2 * i + 1]) same++;
check(same > 0, `L and R are independent (${same}/${J} frames differ)`);

console.log('\n=== the jack output really is 24-bit ===');
let inRange = true;
for (let i = 0; i < J * 2; i++) {
  const v = jackOut[i];
  if (v < -0x800000 || v > 0xFFFFFF) { inRange = false; break; }
}
check(inRange, 'every jack sample fits in 24 bits');

console.log('\n=== the MASTER knob scales the jack output (the firmware\'s own law) ===');
X.pa_wasm_set_pot(0, 4095);              // 100 %
for (let k = 0; k < 40; k++) X.pa_wasm_tick(16);
const loud = settledPeak(250, 60);
const loudRms = settledRms(2100, 700);   // 3 s settle, 1 s measured

X.pa_wasm_set_pot(0, 0);                 // 0 %
for (let k = 0; k < 40; k++) X.pa_wasm_tick(16);
for (let k = 0; k < 250; k++) pushJack(0.5);
let quiet = 0;
for (let k = 0; k < 60; k++) { pushJack(0.5); const q = peakL(); if (q > quiet) quiet = q; }
check(quiet < loud, `the master knob lowers the jack output (${loud} -> ${quiet})`);
// at 0 % the only thing left is the 1-LSB TPDF dither the DAC stage adds
check(quiet <= 1, `at 0 % only the 1-LSB dither is left (peak ${quiet})`);

X.pa_wasm_set_pot(0, 4095);
for (let k = 0; k < 40; k++) X.pa_wasm_tick(16);
const backRms = settledRms(2100, 700);
check(Math.abs(backRms - loudRms) / loudRms < 0.05, `and back up again (rms ${loudRms.toFixed(0)} -> ${backRms.toFixed(0)})`);

console.log('\n' + (fails === 0 ? 'JACK OK' : fails + ' FAILURE(S)'));
process.exit(fails === 0 ? 0 : 1);
