/* The chord combos in the headless harness: press BOTH buttons at once, as
 * the workbench does, and check that the combo fires exactly once. */
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

const T = new Uint32Array(mem.buffer, X.pa_wasm_telemetry_ptr(), 21);
const tick = (n = 1) => { for (let i = 0; i < n; i++) X.pa_wasm_tick(16); X.pa_wasm_update_telemetry(); };
const view = () => T[0];

const HOME = 1, VOLM = 2, VOLP = 4, LEFT = 8, RIGHT = 16;

const COMBOS = [
  ['tuner  (VOL- & VOL+)', VOLM | VOLP, 4],
  ['bank   (< & >)',      LEFT | RIGHT, 3],
  ['groove (HOME & VOL-)', HOME | VOLM, 7],
];

let fails = 0;
const check = (ok, msg) => { console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${msg}`); if (!ok) fails++; };

console.log('=== a chord fires exactly once, every time ===');
tick(20);
console.log(`  start view ${view()}`);

for (const [name, mask, want] of COMBOS) {
  let opened = 0, closed = 0;
  for (let round = 0; round < 8; round++) {
    const before = view();
    X.pa_wasm_press(mask);
    tick(4);                      // 64 ms of contact
    X.pa_wasm_release(mask);
    tick(6);
    const after = view();
    if (after !== before) opened++; else closed++;
  }
  check(opened === 8, `${name}: the chord changed the view 8 times out of 8 (got ${opened})`);
}

console.log('\n=== a chord does NOT leave the button stuck down ===');
// press and release, then wait: the button must be up, so a second press
// must be a fresh edge and not a repeat
X.pa_wasm_press(VOLM | VOLP);
tick(4);
X.pa_wasm_release(VOLM | VOLP);
tick(30);
const v1 = view();
X.pa_wasm_press(VOLM | VOLP);
tick(4);
X.pa_wasm_release(VOLM | VOLP);
tick(6);
const v2 = view();
check(v1 !== v2, `the chord toggled again after a full release (${v1} -> ${v2})`);

console.log('\n' + (fails === 0 ? 'CHORDS OK' : fails + ' FAILURE(S)'));
process.exit(fails === 0 ? 0 : 1);
