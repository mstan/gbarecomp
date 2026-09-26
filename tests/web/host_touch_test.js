// Run with node tests/web/host_touch_test.js. Touch ring, pointer ownership,
// virtual pad, host overlay replay, presentation fields and browser services.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../../packaging/web/host_web.js'), 'utf8');

function context(extra = {}) {
  const storage = new Map(), events = [];
  const ctx = vm.createContext({module: {exports: {}}, performance, Atomics, SharedArrayBuffer,
    Int32Array, Uint32Array, Float32Array, Uint8Array, Math, Object, Array, Number, String, Map, Set, Error,
    navigator: {maxTouchPoints: 5, vibrate: ms => { events.push(['vibrate', ms]); return true; }},
    matchMedia: query => ({matches: /coarse/.test(query)}),
    localStorage: {getItem: k => (storage.has(k) ? storage.get(k) : null), setItem: (k, v) => storage.set(k, String(v))},
    CustomEvent: class { constructor(type) { this.type = type; } },
    dispatchEvent: e => { events.push(['event', e.type]); return true; },
    ...extra});
  vm.runInContext(source, ctx);
  return {ctx, storage, events, exports: ctx.module.exports};
}
// Host attached to a control block laid out like the descriptor (ABI 2).
function attached(exports) {
  const fields = {};
  ['keys', 'turbo', 'inputUpdates', 'touchWrite', 'touchRead', 'touchOverflow', 'touchControls', 'padVisible',
   'anchorTop', 'drawable', 'dprMilli', 'insetsLT', 'insetsRB', 'viewXY', 'viewWH'].forEach((k, i) => { fields[k] = i; });
  const canvas = {width: 480, height: 320, style: {}, focus() {}, setPointerCapture() {},
    getBoundingClientRect: () => ({left: 10, top: 20, width: 240, height: 160, right: 250, bottom: 180})};
  const h = new exports.Host(canvas, () => {});
  h.control = new Int32Array(new SharedArrayBuffer(4096));
  h.buffer = h.control.buffer; h.ptr = 0; h.active = true;
  h.d = {fields, touches: 256, touchSlots: 4, touchStride: 16, commands: 1024, commandSlots: 4};
  return h;
}
const field = (h, k) => Atomics.load(h.control, h.d.fields[k]) >>> 0;
function ring(h) {
  const out = [];
  for (let i = field(h, 'touchRead'); i !== field(h, 'touchWrite'); i = (i + 1) >>> 0) {
    const p = h.d.touches + (i & 3) * 16, head = new Int32Array(h.buffer, p, 2), xy = new Float32Array(h.buffer, p + 8, 2);
    out.push([head[0], head[1], xy[0], xy[1]]);
  }
  return out;
}
const pointer = (type, id, x, y, extra = {}) => ({pointerType: type, pointerId: id, clientX: x, clientY: y, button: 0,
  preventDefault() { this.prevented = true; }, ...extra});

function test_ring_and_ownership() {
  const {exports} = context();
  const {TOUCH} = exports;
  const h = attached(exports);
  h.configureTouch(true, -1, false);
  assert.equal(field(h, 'touchControls'), 1, 'a touch screen offers the pad');
  assert.equal(field(h, 'padVisible'), 1, 'touch-primary default shows the pad');
  assert.equal(h.canvas.style.touchAction, 'none', 'the browser must not scroll/zoom game touches');
  // Finger: client -> drawable pixels (canvas 480x320 over a 240x160 CSS box at 10,20).
  assert(h.pointerDown(pointer('touch', 7, 70, 60)));
  h.pointerMove(pointer('touch', 7, 90, 80));
  h.pointerMove(pointer('touch', 7, 90, 80), TOUCH.Up);
  assert.deepEqual(ring(h), [[TOUCH.Down, 7, 120, 80], [TOUCH.Move, 7, 160, 120], [TOUCH.Up, 7, 160, 120]]);
  assert.equal(h.pointerMove(pointer('touch', 7, 1, 1)), false, 'a finished pointer is forgotten');
  // Mouse is not touch unless emulation is on.
  assert.equal(h.pointerDown(pointer('mouse', 1, 50, 50)), false);
  Atomics.store(h.control, h.d.fields.touchRead, field(h, 'touchWrite'));
  // Full ring: counted overflow, never an overwrite of unread events.
  for (let i = 0; i < 4; ++i) assert(h.pushTouch(TOUCH.Move, i, i, i));
  assert.equal(h.pushTouch(TOUCH.Move, 9, 0, 0), false);
  assert.equal(field(h, 'touchOverflow'), 1);
  assert.equal(h.stats.touchOverflow, 1);
  assert.deepEqual(ring(h).map(e => e[1]), [0, 1, 2, 3]);
  // Blur/visibility cancel live pointers so the recognizer never keeps a ghost finger.
  Atomics.store(h.control, h.d.fields.touchRead, field(h, 'touchWrite'));
  h.pointerDown(pointer('pen', 3, 30, 30));
  h.clearInput();
  assert.deepEqual(ring(h).map(e => [e[0], e[1]]), [[TOUCH.Down, 3], [TOUCH.Cancel, 3]]);
}

function test_mouse_emulation() {
  const {exports} = context({navigator: {maxTouchPoints: 0}, matchMedia: () => ({matches: false})});
  const {TOUCH} = exports;
  const h = attached(exports);
  h.configureTouch(false, -1, false);
  assert.equal(field(h, 'touchControls'), 0, 'desktop without touch: no pad');
  assert.equal(h.touchEnabled(), false);
  assert.equal(h.pointerDown(pointer('touch', 1, 20, 30)), false, 'no touch path without capability or policy');
  h.configureTouch(false, -1, true);
  assert.equal(field(h, 'touchControls'), 1, 'emulation provides the pad capability (native parity)');
  assert.equal(field(h, 'padVisible'), 0, 'fine pointer: pad hidden by default');
  h.pointerDown(pointer('mouse', 1, 20, 30));
  h.pointerDown(pointer('mouse', 1, 20, 30, {button: 2}));
  h.pointerDown(pointer('mouse', 1, 20, 30, {button: 1}));
  assert.deepEqual(ring(h).map(e => e[0]), [TOUCH.Down | TOUCH.FromMouse,
    TOUCH.TwoFingerTap | TOUCH.FromMouse, TOUCH.ThreeFingerTap | TOUCH.FromMouse]);
}

function test_pad() {
  const {exports, storage} = context();
  const h = attached(exports);
  h.gameId = 'a'.repeat(40);
  h.configureTouch(true, 0, false);
  assert.equal(field(h, 'padVisible'), 0, 'explicit pad_default=0 hides the pad');
  h.setPadVisible(true);
  assert.equal(field(h, 'padVisible'), 1);
  assert.equal(storage.get('gbarecomp.touchPad.' + 'a'.repeat(40)), '1', 'choice persists per game');
  h.configureTouch(true, 0, false);
  assert.equal(field(h, 'padVisible'), 1, 'saved choice beats the default');
  // Pad pointers feed the touch mask (one owner per pointer, slide between buttons).
  h.padMaskAt = (x) => x;  // hit test stubbed: x encodes the mask
  h.padPointer(pointer('touch', 1, 1, 0, {type: 'pointerdown'}));
  h.padPointer(pointer('touch', 2, 16 | 64, 0, {type: 'pointerdown'}));
  assert.equal((~field(h, 'keys')) & 1023, 1 | 16 | 64);
  h.padPointer(pointer('touch', 1, 2, 0, {type: 'pointermove'}));
  assert.equal((~field(h, 'keys')) & 1023, 2 | 16 | 64, 'sliding follows the finger');
  h.padPointer(pointer('touch', 2, 0, 0, {type: 'pointerup'}));
  assert.equal((~field(h, 'keys')) & 1023, 2);
  h.padPointer(pointer('touch', 9, 8, 0, {type: 'pointermove'}));
  assert.equal((~field(h, 'keys')) & 1023, 2, 'a move without a pad down is not a press');
  h.setPadVisible(false);
  assert.equal((~field(h, 'keys')) & 1023, 0, 'hiding the pad releases its buttons');
  assert.equal(storage.get('gbarecomp.touchPad.' + 'a'.repeat(40)), '0');
  // No capability: visibility requests are refused.
  const {exports: noTouch} = context({navigator: {maxTouchPoints: 0}, matchMedia: () => ({matches: false})});
  const d = attached(noTouch);
  d.configureTouch(true, 1, false);
  d.setPadVisible(true);
  assert.equal(field(d, 'padVisible'), 0);
  // 8-way d-pad with a dead zone.
  const {dpadMask} = exports;
  assert.equal(dpadMask(0, 0, 100), 0);
  assert.equal(dpadMask(80, 0, 100), 16);
  assert.equal(dpadMask(-80, 0, 100), 32);
  assert.equal(dpadMask(0, -80, 100), 64);
  assert.equal(dpadMask(0, 80, 100), 128);
  assert.equal(dpadMask(60, -60, 100), 16 | 64);
  assert.equal(dpadMask(-60, 60, 100), 32 | 128);
}

function test_overlay_replay() {
  const {exports} = context();
  const {drawOverlay, OVERLAY} = exports;
  const calls = [];
  const ctx = new Proxy({}, {get: (t, k) => (k in t ? t[k] : (...a) => calls.push([k, ...a])),
    set: (t, k, v) => { t[k] = v; calls.push(['set', k, v]); return true; }});
  const cmds = [[OVERLAY.Line, 0xff0000ff, 1, 2, 3, 4, 5, 0], [OVERLAY.FillCircle, 0x80ffffff, 10, 20, 6, 0, 0, 0],
    [OVERLAY.Arc, 0xc8ffd250, 5, 6, 7, 2, 0, 0.25], [99, 0, 0, 0, 0, 0, 0, 0]];
  const bytes = new ArrayBuffer(cmds.length * 32), u32 = new Uint32Array(bytes), f32 = new Float32Array(bytes);
  cmds.forEach((c, i) => { u32[i * 8] = c[0]; u32[i * 8 + 1] = c[1]; f32.set(c.slice(2), i * 8 + 2); });
  drawOverlay(ctx, u32, f32, cmds.length);
  assert(calls.some(c => c[0] === 'set' && c[1] === 'strokeStyle' && c[2] === 'rgba(255,0,0,1)'), 'rgba unpacks r|g<<8|b<<16|a<<24');
  assert(calls.some(c => c[0] === 'lineTo' && c[1] === 3 && c[2] === 4));
  assert(calls.some(c => c[0] === 'arc' && c[1] === 10 && c[2] === 20 && c[3] === 6));
  const arc = calls.filter(c => c[0] === 'arc').pop();
  assert.ok(Math.abs(arc[4] + Math.PI / 2) < 1e-6 && Math.abs(arc[5]) < 1e-6, 'arc turns start at 12 o\'clock');
  assert.equal(calls.filter(c => c[0] === 'stroke').length, 2, 'unknown primitive skipped');
}

function test_overlay_acquire() {
  const {exports} = context();
  const h = new exports.Host({}, () => {});
  h.buffer = new SharedArrayBuffer(4096);
  h.control = new Int32Array(h.buffer);
  h.ptr = 0;
  h.d = {fields: {middle: 0}, video: 64, videoStride: 1024, pixels: 16, maxWidth: 2, maxHeight: 2, pixelBytes: 12,
    overlay: 28, overlayCmds: 4, overlayStride: 32};
  h.staging = new Uint8Array(12);
  h.stats.overlayDropped = 0;
  const slot = 64 + 1024;
  new Uint32Array(h.buffer, slot, 4).set([2, 2, 6, 1]);
  new Uint32Array(h.buffer, slot + 28, 2).set([1, 3]);
  new Uint32Array(h.buffer, slot + 36, 1)[0] = 4;
  Atomics.store(h.control, 0, 1 | 4);
  assert(h.acquire());
  assert.equal(h.overlayCount, 1);
  assert.equal(h.stats.overlayDropped, 3, 'dropped primitives are counted');
  assert.equal(new Uint32Array(h.overlayBytes.buffer)[0], 4);
  new Uint32Array(h.buffer, slot + 36, 1)[0] = 7;
  assert.equal(new Uint32Array(h.overlayBytes.buffer)[0], 4, 'the overlay is a private copy');
  new Uint32Array(h.buffer, 64, 4).set([2, 2, 6, 2]);
  new Uint32Array(h.buffer, 64 + 28, 2).set([5, 0]);
  Atomics.store(h.control, 0, 0 | 4);
  assert.throws(() => h.acquire(), /Invalid host overlay length/);
}

function test_presentation_and_services() {
  const {exports, events} = context();
  const h = attached(exports);
  // Portrait anchoring follows the runtime request, with the top safe inset.
  h.insets = [0, 30, 0, 0];
  assert.deepEqual({...h.placement(480, 900, 240, 160)}, {x: 0, y: 290, w: 480, h: 320, integer: true});
  Atomics.store(h.control, h.d.fields.anchorTop, 1);
  assert.equal(h.placement(480, 900, 240, 160).y, 30);
  assert.equal(h.placement(900, 480, 240, 160).y, 0, 'landscape is never anchored');
  assert.equal(h.haptic(40), true);
  assert.deepEqual(events.shift(), ['vibrate', 40]);
  h.haptic(99999);
  assert.deepEqual(events.shift(), ['vibrate', 5000], 'duration clamped');
  h.requestSettings();
  assert.deepEqual(events.shift(), ['event', 'gbrsettings']);
  const reports = [];
  const {exports: bare} = context({navigator: {}});
  const quiet = new bare.Host({}, r => reports.push(r));
  assert.equal(quiet.haptic(10), false);
  quiet.haptic(10);
  assert.equal(reports.filter(r => /Haptics unavailable/.test(r)).length, 1, 'reported once');
}

test_ring_and_ownership();
test_mouse_emulation();
test_pad();
test_overlay_replay();
test_overlay_acquire();
test_presentation_and_services();
console.log('web host touch PASS (touch ring, ownership, emulation, pad, overlay replay/copy, anchoring, haptics, settings)');
