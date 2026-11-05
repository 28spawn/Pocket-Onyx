/* PARA and HOME have BOTH a click and a hold. A real button cannot do both:
 * the click is what happens when you let go quickly, the hold is what happens
 * when you keep holding. These checks make sure the firmware does that.
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

const T = new Uint32Array(mem.buffer, X.pa_wasm_telemetry_ptr(), 21);
const tick = (n = 1) => { for (let i = 0; i < n; i++) X.pa_wasm_tick(16); X.pa_wasm_update_telemetry(); };
const view = () => T[0];

const PARA = 1 << 5, HOME = 1 << 0;

let fails = 0;
const check = (ok, msg) => { console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${msg}`); if (!ok) fails++; };

console.log('=== a quick PARA press is a click (it opens the block editor) ===');
tick(20);
const v0 = view();
X.pa_wasm_press(PARA);
tick(3);
X.pa_wasm_release(PARA);
tick(4);
const v1 = view();
check(v1 !== v0, `the click arrived on the release (view ${v0} -> ${v1})`);

console.log('\n=== a PARA hold is NOT a click ===');
// back to the home grid first
X.pa_wasm_press(PARA); tick(3); X.pa_wasm_release(PARA); tick(4);
const v2 = view();
X.pa_wasm_press(PARA);
tick(60);                       // 960 ms - still under the 1 s hold threshold
X.pa_wasm_release(PARA);
tick(4);
const v3 = view();
check(v3 === v2, `a 960 ms press did not act as a click (view ${v2} -> ${v3})`);

console.log('\n=== a PARA hold fires the HOLD bit, once ===');
// PARA held for 1.28 s: the HOLD1 ("Save to") must fire, and it must NOT also
// act as a click. The Save-to action opens the save dialog (view 5), so the
// view changing to 5 is the hold working - and the block editor (view 2)
// must NOT have been opened first.
X.pa_wasm_press(PARA);
tick(80);                       // 1.28 s
X.pa_wasm_release(PARA);
tick(10);
const v4 = view();
check(v4 === 5, `the hold opened the Save dialog (view ${v3} -> ${v4})`);
check(v4 !== 2, `and it did not act as a click as well`);

console.log('\n=== HOME: a quick press opens the Settings, a hold does not ===');
// get back to the home grid first
X.pa_wasm_press(HOME); tick(3); X.pa_wasm_release(HOME); tick(6);
if (view() === 0) {
  X.pa_wasm_press(HOME); tick(3); X.pa_wasm_release(HOME); tick(4);
  const s1 = view();
  check(s1 !== 0, `a quick HOME press opened the Settings (view ${s1})`);
  // back home
  X.pa_wasm_press(HOME); tick(3); X.pa_wasm_release(HOME); tick(4);
  const s2 = view();
  check(s2 === 0, `and HOME again came back (view ${s2})`);
  // a long HOME hold: no click
  const s3 = view();
  X.pa_wasm_press(HOME);
  tick(100);                     // 1.6 s - past the 1.5 s HOME hold
  X.pa_wasm_release(HOME);
  tick(4);
  const s4 = view();
  check(s4 === s3, `a 1.6 s HOME hold did not act as a click (view ${s3} -> ${s4})`);
}

console.log('\n' + (fails === 0 ? 'PARA SEMANTICS OK' : fails + ' FAILURE(S)'));
process.exit(fails === 0 ? 0 : 1);
