// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 The Onyx Authors (@28spawn)
/* Does a loaded NAM actually RUN in the firmware (not just sit in the
 * registry)? Load a capture, then compare the output with and without it. */
const fs = require('fs');
const buf = fs.readFileSync(process.argv[2]);
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

function play(amp, blocks) {
  let pk = 0, acc = 0, n = 0, ph = 0;
  for (let b = 0; b < blocks; b++) {
    for (let i = 0; i < J; i++) {
      const s = Math.round(amp * Math.sin(2 * Math.PI * 220 * ph / 44100) * 8388607);
      jackIn[2 * i] = s & 0xFFFFFF; jackIn[2 * i + 1] = 0;
      ph++;
    }
    X.pa_wasm_audio_process();
    for (let i = 0; i < J; i++) {
      const v = (jackOut[2 * i] << 8) >> 8;
      const a = Math.abs(v);
      if (a > pk) pk = a;
      acc += v * v; n++;
    }
  }
  return { pk: pk / 8388608, rms: Math.sqrt(acc / n) / 8388608 };
}

let fails = 0;
const check = (ok, msg) => { console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${msg}`); if (!ok) fails++; };

console.log('=== the stock amp ===');
const before = play(0.3, 200);
console.log(`  out peak ${before.pk.toFixed(4)}  rms ${before.rms.toFixed(4)}`);

console.log('\n=== build a NAM and load it ===');
// the "WAVE" container: 20 bytes of header + the float32 weights
const n = 1870;
const blob = new Uint8Array(20 + n * 4);
const dv = new DataView(blob.buffer);
blob[0] = 87; blob[1] = 65; blob[2] = 86; blob[3] = 69;   // "WAVE"
dv.setUint32(4, n, true);
dv.setFloat32(8, 1.0, true);
dv.setUint32(12, 0, true);
dv.setFloat32(16, 1.0, true);
for (let i = 0; i < n; i++) dv.setFloat32(20 + i * 4, Math.sin(i * 0.11) * 0.05, true);

const name = new TextEncoder().encode('NODE TEST NAM');
const memU8 = new Uint8Array(mem.buffer);
const blobPtr = 22 * 1024 * 1024;
const namePtr = blobPtr + blob.length;
memU8.set(blob, blobPtr);
memU8.set(name, namePtr);
memU8[namePtr + name.length] = 0;
const idx = X.pa_wasm_add_nam(blobPtr, blob.length, namePtr);
check(idx >= 0, `the firmware registered the NAM (slot ${idx})`);
check(X.pa_wasm_nam_count() === 1, `the registry has ${X.pa_wasm_nam_count()} capture(s)`);

console.log('\n=== the NAM is now the engine ===');
const after = play(0.3, 200);
console.log(`  out peak ${after.pk.toFixed(4)}  rms ${after.rms.toFixed(4)}`);
const changed = Math.abs(after.rms - before.rms) > 1e-4;
check(changed, `the output changed (rms ${before.rms.toFixed(4)} -> ${after.rms.toFixed(4)}) - the NAM is running`);

console.log('\n' + (fails === 0 ? 'NAM RUNS IN THE FIRMWARE' : fails + ' FAILURE(S)'));
process.exit(fails === 0 ? 0 : 1);
