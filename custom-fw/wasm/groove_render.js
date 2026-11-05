/* Render the Groove Station and dump the LCD, so it can be compared with the
 * design kit's reference render (GROOVE_SPEC.md, groove_px_3_transport.png).
 *
 * The screen is drawn 1:1, so the comparison is a pixel diff: the design's
 * rule is "if your screen and the PNG differ by even one pixel, the PNG is
 * right". The header (y 0-29) is the device's own, so the comparison starts
 * below it.
 *
 * The dump is the RAW RGB565 the panel holds (byte-swapped, as the driver
 * stores it), because the reference PNG is the exact colour the design uses
 * and an 8-bit expansion of the same colour can round differently.
 *
 *   node custom-fw/wasm/groove_render.js [pocket_amp.wasm] [out.rgb565] [stopped|playing] [step:N]
 *
 * `step:N` waits for the drum engine's absolute step to reach N before dumping, which is
 * how the "the bar goes away after the 4th" regression compares bar 1 with bar 2.
 */
const fs = require('fs');
const path = require('path');

const wasmPath = process.argv[2] || path.join(__dirname, 'out', 'pocket_amp.wasm');
const outPath  = process.argv[3] || path.join(__dirname, 'out', 'groove.rgb565');
const playing  = process.argv[4] === 'playing';
const argStep  = (process.argv[5] || '').startsWith('step:')
  ? parseInt(process.argv[5].slice(5), 10) : null;

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
    fd_write: () => 0, environ_get: () => 0, environ_sizes_get: () => 0, clock_time_get: () => 0, random_get: () => 0 },
};
const X = new WebAssembly.Instance(new WebAssembly.Module(buf), imp).exports;
X.pa_wasm_init();

/* the telemetry's fields, in the order pa_wasm.c writes them */
const T = new Uint32Array(mem.buffer, X.pa_wasm_telemetry_ptr(), 21);
const TEL = { view: 0, drum_playing: 12, drum_bpm: 13, drum_pattern: 14 };
const tick = (n = 1) => { for (let i = 0; i < n; i++) X.pa_wasm_tick(16); X.pa_wasm_update_telemetry(); };
/* The drum engine's clock only advances when the audio is mixed, so the
 * playhead needs the jack pumped - the UI just reads the engine's step. */
const advance = (n = 1) => {
  for (let i = 0; i < n; i++) { X.pa_wasm_tick(16); X.pa_wasm_audio_process(); }
  X.pa_wasm_update_telemetry();
};

const HOME = 1 << 0, VOL_MINUS = 1 << 1, VOL_PLUS = 1 << 2, PARA = 1 << 5;

tick(20);

/* The Groove Station is the HOME + VOL- chord on the home grid. */
if (T[TEL.view] === 0) {
  X.pa_wasm_press(HOME | VOL_MINUS); tick(4); X.pa_wasm_release(HOME | VOL_MINUS); tick(8);
}
if (T[TEL.view] !== 7) {
  console.error('the groove station did not open (view ' + T[TEL.view] + ')');
  process.exit(2);
}

/* The reference render is the "8 BEAT 1" style at 116 BPM and 70 % volume.
 * The volume is the firmware's default; the tempo starts at 120 and the
 * style at 0. */
/* a negative encoder delta lowers the tempo, the way the knob does */
const enc = (d, n) => { for (let i = 0; i < n; i++) { X.pa_wasm_add_enc(d); tick(3); } };
if (T[TEL.drum_bpm] > 116) enc(-1, T[TEL.drum_bpm] - 116);
else if (T[TEL.drum_bpm] < 116) enc(1, 116 - T[TEL.drum_bpm]);

/* back to the first style */
while (T[TEL.drum_pattern] !== 0) {
  X.pa_wasm_press(1 << 4); tick(3); X.pa_wasm_release(1 << 4); tick(3);
}

/* Play or stop: a PARA press is play / stop. */
if (playing && !T[TEL.drum_playing]) {
  X.pa_wasm_press(PARA); tick(3); X.pa_wasm_release(PARA); tick(6);
}
tick(10);

const w = X.pa_wasm_lcd_w(), h = X.pa_wasm_lcd_h();
const lcd = new Uint16Array(mem.buffer, X.pa_wasm_lcd_ptr(), w * h);

/* The reference render has the playhead on step 5, so the comparison needs
 * the same step. The step comes from the drum engine's own clock, so the
 * render waits for it: the strip's current step is the SELECT-coloured
 * block. */
if (playing && argStep === null) {
  const SELECT = 0xF628;
  const SWAPPED = ((SELECT >> 8) | (SELECT << 8)) & 0xFFFF;   /* the panel's byte order */
  const stepX = (i) => 12 + i * 13 + Math.floor(i / 4) * 4;
  let found = false;
  for (let i = 0; i < 800 && !found; i++) {
    advance(1);
    const c = lcd[133 * w + stepX(5)];
    if (c === SELECT || c === SWAPPED) found = true;
  }
  if (!found) console.log('the playhead never reached step 5');
}

/* Optional: stop on one exact engine step instead (step:N), for the test that
 * compares the same bar position in the first and a later bar of the loop. */
if (argStep !== null) {
  let hit = false;
  for (let i = 0; i < 20000 && !hit; i++) {
    advance(1);
    if (X.pa_wasm_drum_step() === argStep) hit = true;
  }
  if (!hit) {
    console.error('the engine never reached step ' + argStep);
    process.exit(3);
  }
  /* The screen is drawn from the UI's copy of the step, which the tick takes at the
   * start of its pass - so the panel still shows the previous step. One more tick
   * lets it catch up (the drum clock cannot run a whole step in one block). */
  advance(1);
}

console.log('the view', T[TEL.view], 'bpm', T[TEL.drum_bpm],
            'style', T[TEL.drum_pattern], 'playing', T[TEL.drum_playing]);

/* the view's own offset - the LCD is not at the start of the memory */
fs.writeFileSync(outPath, Buffer.from(lcd.buffer, lcd.byteOffset, w * h * 2));
console.log('wrote', outPath, w + 'x' + h);
