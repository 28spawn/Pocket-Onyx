/* Preset management over the wire.
 *
 * The firmware's upload session (SysEx cmd 25) has a target: 0 is a PRESET.
 * The body is the preset record - pa_preset_t, 164 bytes (include/pa_preset.h)
 * - and the firmware stores it in the flash with the torn-write-safe A/B
 * commit, so a preset can be pushed to a device (or to the browser's flash
 * image) exactly like a NAM capture.
 *
 * This drives the firmware's OWN parser (the same pa_sysex_input_byte() the
 * device runs) and then reads the preset back with the firmware's own
 * pa_preset_load(), so the round trip is measured, not assumed.
 *
 *   node custom-fw/wasm/test_preset.js [pocket_amp.wasm]
 */
const fs = require('fs');
const path = require('path');

const wasmPath = process.argv[2] || path.join(__dirname, 'out', 'pocket_amp.wasm');
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
  /* the length is a plain 24-bit little-endian (len:u24le) */
  const l0 = payload.length & 0xFF;
  const l1 = (payload.length >> 8) & 0xFF;
  const l2 = (payload.length >> 16) & 0xFF;
  let sum = (cmd & 0xFF) + l0 + l1 + l2;
  for (const b of payload) sum += b;
  const inner = [HDR0, HDR1, cmd & 0xFF, l0, l1, l2, ...payload, (~sum) & 0xFF];
  return [0xF0, ...pack8to7(inner), 0xF7];
}
const SCRATCH = 22 * 1024 * 1024;
function feed(bytes) {
  const memU8 = new Uint8Array(mem.buffer);
  memU8.set(bytes, SCRATCH);
  X.pa_wasm_feed_sysex(SCRATCH, bytes.length);
}

/* --- the preset record, pa_preset_t (164 bytes) --------------------------- */
const PRESET_RECORD_SIZE = 164;
const SLOT_COUNT = 8;
const NPARAMS = 6;

/* The firmware's pa_crc32(): the standard reflected CRC-32 (0xEDB88320), the
 * same as zlib's crc32. The preset path checks it before it saves the slot,
 * so a pushed preset has to carry the right one. */
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    t[n] = c >>> 0;
  }
  return t;
})();
function crc32(bytes, len) {
  let crc = 0xFFFFFFFF;
  for (let i = 0; i < len; i++) crc = CRC_TABLE[(crc ^ bytes[i]) & 0xFF] ^ (crc >>> 8);
  return (~crc) >>> 0;
}

function buildPresetRecord(name, presetVol, slotOrder, ampIdx, cabIdx, knobs) {
  const rec = new Uint8Array(PRESET_RECORD_SIZE);
  const dv = new DataView(rec.buffer);
  dv.setUint32(0, 0x32504D50, true);          // the magic, "PMP2"
  dv.setUint32(4, 0, true);                    // the sequence
  for (let i = 0; i < name.length && i < 23; i++) rec[8 + i] = name.charCodeAt(i);
  dv.setUint16(32, 120, true);                 // the bpm
  rec[39] = 0xFF;                              // every slot enabled
  for (let i = 0; i < SLOT_COUNT; i++) rec[40 + i] = slotOrder[i];
  rec[48] = ampIdx;
  rec[49] = cabIdx;
  rec[50] = presetVol;
  rec[51] = 0;                                 // not EMPTY
  for (let s = 0; s < SLOT_COUNT; s++) {
    for (let k = 0; k < NPARAMS; k++) {
      dv.setUint16(64 + (s * NPARAMS + k) * 2, knobs[s][k], true);
    }
  }
  /* the CRC32 is over the record minus the CRC field itself */
  dv.setUint32(160, crc32(rec, 160), true);
  return rec;
}

function presetName(idx) {
  const ptr = X.pa_wasm_scratch_in();          /* any scratch in the wasm memory */
  const namePtr = ptr;
  const vol = X.pa_wasm_preset_name(idx, namePtr, 64);
  const memU8 = new Uint8Array(mem.buffer);
  let end = namePtr;
  while (memU8[end]) end++;
  return { name: Buffer.from(memU8.slice(namePtr, end)).toString('ascii'), vol };
}

console.log('=== a preset record pushed into the flash image ===');
const order = [0, 1, 2, 3, 4, 5, 6, 7];
const knobs = [];
for (let s = 0; s < SLOT_COUNT; s++) knobs.push([1000, 2000, 3000, 4000, 5000, 6000]);
const rec = buildPresetRecord('WORKBENCH P41', 77, order, 5, 3, knobs);

/* the session: cmd 25 arms it (the length as u24le, the target, the slot),
 * the body follows in cmd 34 chunks, and the firmware commits by itself when
 * the declared length has arrived. */
const header = [rec.length & 0xFF, (rec.length >> 8) & 0xFF, (rec.length >> 16) & 0xFF, 0, 41];
feed(buildFrame(25, header));
for (let off = 0; off < rec.length; off += 128) {
  feed(buildFrame(34, Array.from(rec.subarray(off, off + 128))));
}
console.log(`  pushed ${rec.length} bytes into slot 41`);

const got = presetName(41);
console.log(`  the preset reads back: "${got.name}" (volume ${got.vol})`);
check(got.name === 'WORKBENCH P41', 'the preset landed in the flash, with its name');
check(got.vol === 77, 'and with its volume');

console.log('\n=== the factory presets are still intact ===');
const p1 = presetName(0);
const p40 = presetName(39);
console.log(`  P01 "${p1.name}", P40 "${p40.name}"`);
check(p1.name.length > 0 && p40.name.length > 0, 'the factory presets are untouched');

console.log('\n=== a preset with a name that needs trimming ===');
const rec2 = buildPresetRecord('A VERY LONG PRESET NAME INDEED', 50, order, 0, 0, knobs);
const header2 = [rec2.length & 0xFF, (rec2.length >> 8) & 0xFF, (rec2.length >> 16) & 0xFF, 0, 42];
feed(buildFrame(25, header2));
for (let off = 0; off < rec2.length; off += 128) {
  feed(buildFrame(34, Array.from(rec2.subarray(off, off + 128))));
}
const got2 = presetName(42);
console.log(`  the preset reads back: "${got2.name}" (${got2.name.length} characters)`);
check(got2.name.length === 23, 'the name is cut to the 23 characters the record holds');
check(got2.name === 'A VERY LONG PRESET NAME', 'and it is the name that was sent');

console.log('\n=== the record the HOST TOOL builds (custom-fw/tools/mvave_device.py) ===');
/* The host tool builds the record with Python's zlib.crc32 and this test
 * builds it with a table-driven CRC-32 - the same function, two
 * implementations. If the two records agree byte for byte, the tool's
 * builder and the firmware agree with each other. */
const toolRecordPath = process.argv[3];
if (toolRecordPath && fs.existsSync(toolRecordPath)) {
  const hex = fs.readFileSync(toolRecordPath, 'ascii').replace(/\s+/g, '');
  const toolRec = new Uint8Array(Buffer.from(hex, 'hex'));
  console.log(`  the tool's record: ${toolRec.length} bytes`);
  const h3 = [toolRec.length & 0xFF, (toolRec.length >> 8) & 0xFF, (toolRec.length >> 16) & 0xFF, 0, 43];
  feed(buildFrame(25, h3));
  for (let off = 0; off < toolRec.length; off += 128) {
    feed(buildFrame(34, Array.from(toolRec.subarray(off, off + 128))));
  }
  const got3 = presetName(43);
  console.log(`  it reads back: "${got3.name}" (volume ${got3.vol})`);
  check(got3.name === 'WORKBENCH P41', "the tool's record is accepted by the firmware");
  check(got3.vol === 77, "with its volume");
} else {
  console.log('  (no tool record given - run py_preset.py to make one)');
}

console.log('\n=== the workbench\'s own preset API (the wasm exports) ===');
/* The LIBRARY tab manages the store through these: read a slot's record,
 * write one back, store the sound that is playing into a slot. */
const scratch = X.pa_wasm_scratch_in();
console.log(`  the record is ${X.pa_wasm_preset_size()} bytes`);
check(X.pa_wasm_preset_size() === PRESET_RECORD_SIZE, 'the record is the 164 bytes pa_preset_t is');

check(X.pa_wasm_preset_get(41, scratch) === 0, 'a slot reads back');
const memU8 = new Uint8Array(mem.buffer);
let readName = '';
for (let i = 0; i < 24 && memU8[scratch + 8 + i]; i++) readName += String.fromCharCode(memU8[scratch + 8 + i]);
check(readName === 'WORKBENCH P41', `the record carries the name (${readName})`);

/* write a slot back: the name, the volume and the amp model */
const rec3 = new Uint8Array(PRESET_RECORD_SIZE);
rec3.set(rec);
for (let i = 0; i < 11; i++) rec3[8 + i] = 'EDITED P42'.charCodeAt(i);
new DataView(rec3.buffer).setUint8(50, 44);
new DataView(rec3.buffer).setUint8(48, 12);
memU8.set(rec3, scratch);
check(X.pa_wasm_preset_put(42, scratch) === 0, 'a slot is written back');
const back42 = presetName(42);
console.log(`  P42 reads back: "${back42.name}" (volume ${back42.vol})`);
check(back42.name === 'EDITED P42', 'the written name is what the store holds');
check(back42.vol === 44, 'and the written volume');

/* store the sound that is playing into a slot */
const namePtr = scratch + 4096;          /* a string has to live in the wasm memory */
for (let i = 0; i < 'LIVE CAPTURE'.length; i++) memU8[namePtr + i] = 'LIVE CAPTURE'.charCodeAt(i);
memU8[namePtr + 'LIVE CAPTURE'.length] = 0;
check(X.pa_wasm_preset_capture(43, namePtr) === 0, 'the live sound is stored');
const back43 = presetName(43);
console.log(`  P43 reads back: "${back43.name}"`);
check(back43.name === 'LIVE CAPTURE', 'the captured preset carries its name');

console.log('\n' + (fails === 0 ? 'PRESET MANAGEMENT OVER THE WIRE WORKS' : fails + ' FAILURE(S)'));
process.exit(fails === 0 ? 0 : 1);
