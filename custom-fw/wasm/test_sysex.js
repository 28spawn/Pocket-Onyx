/* The SysEx upload path, in the browser build.
 *
 * The firmware's SysEx carries a binary payload: the inner frame is 8-bit
 * and travels packed 8-to-7-bit between F0 and F7, because USB MIDI carries
 * 7-bit data bytes. It used to mask every byte with 0x7F, which made it
 * impossible to send a NAM capture - every byte above 0x7F arrived with its
 * top bit stripped, so the capture that reached the flash was not the capture
 * that was sent.
 *
 * This drives the firmware's OWN parser (the same pa_sysex_input_byte() the
 * device runs) with a cmd-25 upload session, and then checks that the
 * capture in the flash image is byte for byte the one that was sent.
 *
 *   node custom-fw/wasm/test_sysex.js [pocket_amp.wasm]
 */
const fs = require('fs');
const path = require('path');

const wasmPath = process.argv[2] || path.join(__dirname, 'pocket_amp.wasm');
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

let fails = 0;
const check = (ok, msg) => { console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${msg}`); if (!ok) fails++; };

/* --- the same framing the firmware uses (custom-fw/tools/mvave_device.py) --- */
const HDR0 = 0x00, HDR1 = 0x59;
function pack8to7(data) {
  let bits = 0, nbits = 0;
  const out = [];
  for (const b of data) {
    bits += (b & 0xFF) * Math.pow(2, nbits);
    nbits += 8;
    while (nbits >= 7) { out.push(bits % 128); bits = Math.floor(bits / 128); nbits -= 7; }
  }
  if (nbits) out.push(bits % 128);
  return out;
}
function buildFrame(cmd, payload) {
  /* The length is a plain 24-bit little-endian (len:u24le) - it
   * used to be three 7-bit groups, which only agreed with the firmware for
   * lengths up to 256. */
  const l0 = payload.length & 0xFF;
  const l1 = (payload.length >> 8) & 0xFF;
  const l2 = (payload.length >> 16) & 0xFF;
  let sum = (cmd & 0xFF) + l0 + l1 + l2;
  for (const b of payload) sum += b;
  const inner = [HDR0, HDR1, cmd & 0xFF, l0, l1, l2, ...payload, (~sum) & 0xFF];
  return [0xF0, ...pack8to7(inner), 0xF7];
}
const SCRATCH = 22 * 1024 * 1024;      // the same scratch the workbench uses
function feed(bytes) {
  const memU8 = new Uint8Array(mem.buffer);
  memU8.set(bytes, SCRATCH);
  X.pa_wasm_feed_sysex(SCRATCH, bytes.length);
}

/* --- a capture with plenty of bytes above 0x7F ---------------------------- */
const N = 1871;                       // the Lite model's weight count
const blob = new Uint8Array(20 + N * 4);
const dv = new DataView(blob.buffer);
blob[0] = 87; blob[1] = 65; blob[2] = 86; blob[3] = 69;      // "WAVE"
dv.setUint32(4, N, true);
dv.setFloat32(8, 0.0113756, true);
dv.setUint32(12, 1, true);            // it has a cabinet in it
dv.setFloat32(16, 0.2818, true);
for (let i = 0; i < N; i++) {
  /* a value whose float32 bytes are all over the place, top bits included */
  dv.setFloat32(20 + i * 4, Math.sin(i * 0.37) * 3.7, true);
}
const high = blob.filter(b => b >= 0x80).length;
console.log(`=== a ${blob.length} byte capture, ${high} of its bytes above 0x7F ===`);

console.log('\n=== the upload session ===');
const header = [blob.length & 0xFF, (blob.length >> 8) & 0xFF, (blob.length >> 16) & 0xFF, 1, 0];
feed(buildFrame(25, header));
let sent = 0, frames = 1;
while (sent < blob.length) {
  const part = Array.from(blob.subarray(sent, sent + 256));
  feed(buildFrame(34, part));
  sent += part.length; frames++;
}
console.log(`  ${frames} frames, ${sent} body bytes`);

check(X.pa_wasm_nam_count() === 1, `the firmware registered the capture (${X.pa_wasm_nam_count()})`);

console.log('\n=== the capture in the flash image ===');
const flash = new Uint8Array(mem.buffer, X.pa_wasm_flash_ptr(), X.pa_wasm_flash_size());
let at = -1;
for (let i = 0; i + blob.length <= flash.length; i++) {
  let hit = true;
  for (let k = 0; k < blob.length; k++) if (flash[i + k] !== blob[k]) { hit = false; break; }
  if (hit) { at = i; break; }
}
check(at >= 0, at >= 0 ? `the capture is in the flash, byte for byte, at offset 0x${at.toString(16)}`
                       : 'the capture is NOT in the flash');
if (at >= 0) {
  let same = true;
  for (let k = 0; k < blob.length; k++) if (flash[at + k] !== blob[k]) { same = false; break; }
  check(same, 'every byte of it is the byte that was sent');
}

console.log('\n=== a frame that is not packed must be refused ===');
const before = X.pa_wasm_nam_count();
/* the old 7-bit framing: F0 00 59 <cmd> ... F7, unmasked and unpacked */
feed([0xF0, 0x00, 0x59, 0x19, 0x00, 0x00, 0x05, 0x20, 0x00, 0x00, 0x01, 0x00, 0x40, 0xF7]);
check(X.pa_wasm_nam_count() === before,
  'an unpacked frame is not accepted (the framing is the packed one)');

console.log('\n' + (fails === 0 ? 'THE SYSEX UPLOAD WORKS' : fails + ' FAILURE(S)'));
process.exit(fails === 0 ? 0 : 1);
