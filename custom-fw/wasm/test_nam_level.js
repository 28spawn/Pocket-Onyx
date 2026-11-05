/* The NAM volume: a capture loaded from the page must come up at the SAME
 * level as the same capture picked from the effect menu.
 *
 * The effect-menu path goes through the model's apply path, which resets the
 * per-model trim (a capture carries its own makeup gain). The page's load path
 * did not, so the stock amp's trim was still applied on top.
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

/* Silence in, so the time effects of the preset that is loaded (factory preset 1 has a delay
 * and a reverb) ring out between measurements: a tail of the loud stock-amp run must not be
 * read as the capture's level. */
function silence(blocks) {
  for (let i = 0; i < J * 2; i++) jackIn[i] = 0;
  for (let b = 0; b < blocks; b++) X.pa_wasm_audio_process();
}
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

// build a NAM (the WAVE container)
const n = 1870;
const blob = new Uint8Array(20 + n * 4);
const dv = new DataView(blob.buffer);
blob[0] = 87; blob[1] = 65; blob[2] = 86; blob[3] = 69;
dv.setUint32(4, n, true);
dv.setFloat32(8, 1.0, true);
dv.setUint32(12, 0, true);
dv.setFloat32(16, 1.0, true);
for (let i = 0; i < n; i++) dv.setFloat32(20 + i * 4, Math.sin(i * 0.11) * 0.05, true);

const name = new TextEncoder().encode('VOLUME TEST NAM');
const memU8 = new Uint8Array(mem.buffer);
const blobPtr = 22 * 1024 * 1024;
const namePtr = blobPtr + blob.length;
memU8.set(blob, blobPtr);
memU8.set(name, namePtr);
memU8[namePtr + name.length] = 0;

const stock = play(0.3, 200);
console.log(`  the stock amp           rms ${stock.rms.toFixed(4)}`);

const idx = X.pa_wasm_add_nam(blobPtr, blob.length, namePtr);
check(idx >= 0, `the capture was registered (slot ${idx})`);

// let the deferred model swap land (the audio thread applies it at a block
// boundary once the gain has reached zero), and the tails ring out (4 s)
silence(3000);

const loaded = play(0.3, 200);
console.log(`  loaded from the page    rms ${loaded.rms.toFixed(4)}`);

// the same capture, picked from the effect menu: the core's model path
// (the AMP slot's type = PA_STOCK_AMP_COUNT + idx)
X.pa_wasm_select_amp(20 + idx);
silence(3000);
const viaMenu = play(0.3, 200);
console.log(`  picked from the menu    rms ${viaMenu.rms.toFixed(4)}`);

const d = Math.abs(loaded.rms - viaMenu.rms) / Math.max(viaMenu.rms, 1e-9);
check(d < 0.15, `the two paths agree within 1.5 dB (${(20*Math.log10(loaded.rms/viaMenu.rms)).toFixed(2)} dB apart)`);

console.log('\n' + (fails === 0 ? 'NAM LEVEL OK' : fails + ' FAILURE(S)'));
process.exit(fails === 0 ? 0 : 1);

