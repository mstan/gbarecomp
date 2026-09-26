// Run with node tests/web/host_input_test.js. Keyboard bindings, modifier
// chords, save-state slots and hotkey ownership; no browser required.
'use strict';
const assert = require('node:assert/strict');
const {Host, parseBinding} = require('../../packaging/web/host_web.js');

// A host attached to a fake control block, recording commands and fullscreen.
function host(keybinds = '', keymap = '') {
  const reports = [], h = new Host({focus() {}, style: {}}, x => reports.push(x));
  h.configure(keybinds, keymap);
  const fields = {keys: 0, turbo: 1, inputUpdates: 2, commandWrite: 3, commandRead: 4, commandOverflow: 5};
  h.control = new Int32Array(new SharedArrayBuffer(1024));
  h.buffer = h.control.buffer; h.ptr = 0; h.active = true;
  h.d = {fields, commands: 64, commandSlots: 16};
  h.commands = [];
  h.command = (kind, arg = 0) => { h.commands.push(arg ? `${kind}${arg}` : kind); return true; };
  h.fullscreens = [];
  h.fullscreenRequest = mode => h.fullscreens.push(mode);
  h.reports = reports;
  return h;
}
function key(code, mods = {}, extra = {}) {
  return {code, ctrlKey: !!mods.ctrl, altKey: !!mods.alt, shiftKey: !!mods.shift, metaKey: !!mods.meta,
    repeat: false, prevented: false, preventDefault() { this.prevented = true; }, ...extra};
}
const pressed = h => (~Atomics.load(h.control, 0)) & 1023;
const BIT = {a: 1, b: 2, select: 4, start: 8, right: 16, left: 32, up: 64, down: 128, r: 256, l: 512};

function test_parse_and_configure() {
  assert.deepEqual(parseBinding('Ctrl+Shift+KeyX'), {code: 'KeyX', count: 2, Ctrl: true, Alt: false, Shift: true});
  assert.equal(parseBinding(''), null);
  const h = host('[player1]\na=Shift+X\nb=ctrl+z', '[KeyMap]\nTurbo=Ctrl+Tab\nPause=Shift+Alt+P');
  assert.equal(h.keys[0], 'Shift+KeyX');
  assert.equal(h.keys[1], 'Ctrl+KeyZ', 'modifier names are canonical');
  assert.equal(h.hotkeys.Turbo, 'Ctrl+Tab');
  assert.equal(h.hotkeys.Pause, 'Alt+Shift+KeyP', 'modifier order is canonical');
  const bad = host('[player1]\na=Hyper+X');
  assert(bad.reports.some(r => /Unsupported web binding: Hyper\+X/.test(r)));
  assert.equal(bad.keys[0], 'KeyX', 'rejected binding keeps the default');
}

function test_modifier_bound_buttons_engage() {
  // Regression: held buttons used to compare bindings against bare e.code.
  const h = host('[player1]\na=Shift+X', '[KeyMap]\nTurbo=Ctrl+Tab');
  h.keyDown(key('ShiftLeft', {shift: true}));
  h.keyDown(key('KeyX', {shift: true}));
  assert.equal(pressed(h) & BIT.a, BIT.a, 'Shift+X holds A');
  h.keyUp(key('ShiftLeft'));
  assert.equal(pressed(h) & BIT.a, 0, 'releasing the modifier releases a modifier binding');
  h.keyUp(key('KeyX'));
  h.keyDown(key('KeyX'));
  assert.equal(pressed(h) & BIT.a, 0, 'bare X does not satisfy Shift+X');
  h.keyUp(key('KeyX'));
  // Turbo with a modifier is level-triggered and exact, like the native backend.
  h.keyDown(key('ControlLeft', {ctrl: true}));
  h.keyDown(key('Tab', {ctrl: true}));
  assert.equal(Atomics.load(h.control, 1), 1, 'Ctrl+Tab holds turbo');
  h.keyUp(key('ControlLeft'));
  assert.equal(Atomics.load(h.control, 1), 0, 'turbo needs its exact chord');
  h.keyUp(key('Tab'));
  h.keyDown(key('Tab'));
  assert.equal(Atomics.load(h.control, 1), 0, 'bare Tab is not Ctrl+Tab');
  assert.deepEqual(h.commands, [], 'turbo never queues a command');
}

function test_most_specific_binding_wins() {
  const h = host('[player1]\na=Shift+X\nb=X');
  h.keyDown(key('KeyX'));
  assert.equal(pressed(h) & (BIT.a | BIT.b), BIT.b);
  h.keyDown(key('ShiftLeft', {shift: true}));
  assert.equal(pressed(h) & (BIT.a | BIT.b), BIT.a, 'Shift+X is A only, not also B');
}

function test_default_turbo_and_game_buttons() {
  const h = host();
  h.keyDown(key('Tab'));
  assert.equal(Atomics.load(h.control, 1), 1, 'default bare Tab turbo');
  // Select (right Shift) held: Tab still means turbo, not Shift+Tab.
  h.keyDown(key('ShiftRight', {shift: true}));
  assert.equal(pressed(h) & BIT.select, BIT.select);
  assert.equal(Atomics.load(h.control, 1), 1, 'a held game button is not a hotkey modifier');
  h.keyDown(key('KeyX', {shift: true}));
  assert.equal(pressed(h) & BIT.a, BIT.a);
}

function test_slots() {
  const h = host();
  h.keyDown(key('F1'));
  h.keyUp(key('F1'));
  h.keyDown(key('ShiftLeft', {shift: true}));
  h.keyDown(key('F2', {shift: true}));
  h.keyUp(key('F2', {shift: true}));
  h.keyUp(key('ShiftLeft'));
  // Regression: Select is right Shift, and Select+F3 used to SAVE slot 3.
  h.keyDown(key('ShiftRight', {shift: true}));
  h.keyDown(key('F3', {shift: true}));
  h.keyUp(key('F3', {shift: true}));
  assert.equal(pressed(h) & BIT.select, BIT.select, 'Select stays held for the game');
  // With Select held a free left Shift still saves.
  h.keyDown(key('ShiftLeft', {shift: true}));
  h.keyDown(key('F4', {shift: true}));
  h.keyUp(key('F4', {shift: true}));
  h.keyUp(key('ShiftLeft', {shift: true}));
  h.keyUp(key('ShiftRight'));
  // Other chords are not slot bindings; repeats do not re-fire.
  h.keyDown(key('F5', {ctrl: true}));
  h.keyDown(key('F6'));
  h.keyDown(key('F6', {}, {repeat: true}));
  // Shift held before the canvas had focus (no keydown seen) still saves.
  h.keyDown(key('F7', {shift: true}));
  assert.deepEqual(h.commands, ['Load1', 'Save2', 'Load3', 'Save4', 'Load6', 'Save7']);
}

function test_chords_own_their_key() {
  // Regression: Alt+Enter toggled fullscreen AND pressed Start.
  const h = host();
  h.keyDown(key('AltLeft', {alt: true}));
  const enter = key('Enter', {alt: true});
  h.keyDown(enter);
  assert(enter.prevented);
  assert.deepEqual(h.fullscreens, [1]);
  assert.equal(pressed(h) & BIT.start, 0, 'Alt+Enter never presses Start');
  h.keyUp(key('AltLeft'));
  assert.equal(pressed(h) & BIT.start, 0, 'the chord keeps Enter until it is released');
  h.keyUp(key('Enter'));
  h.keyDown(key('Enter'));
  assert.equal(pressed(h) & BIT.start, BIT.start, 'plain Enter is Start');
  h.keyUp(key('Enter'));
  // Shift+P pauses; Select+P does not (Select is a game button).
  h.keyDown(key('ShiftLeft', {shift: true}));
  h.keyDown(key('KeyP', {shift: true}));
  h.keyUp(key('KeyP'));
  h.keyUp(key('ShiftLeft'));
  h.keyDown(key('ShiftRight', {shift: true}));
  h.keyDown(key('KeyP', {shift: true}));
  assert.deepEqual(h.commands, ['Pause']);
  // A bare hotkey that is also a game button drives both (native parity).
  const both = host('[player1]\nl=F', '');
  both.keyDown(key('KeyF'));
  assert.deepEqual(both.commands, ['DisplayPerf']);
  assert.equal(pressed(both) & BIT.l, BIT.l);
  // Meta chords belong to the browser/OS.
  const meta = host();
  const cmd = key('KeyP', {meta: true, shift: true});
  meta.keyDown(cmd);
  assert.deepEqual(meta.commands, []);
}

function test_unbound_keys_pass_through() {
  const h = host();
  const e = key('KeyQ');
  assert.equal(h.keyDown(e), false);
  assert.equal(e.prevented, false, 'unbound keys keep their browser behaviour');
  const mod = key('ControlLeft', {ctrl: true});
  h.keyDown(mod);
  assert.equal(mod.prevented, false, 'a free modifier is tracked, not swallowed');
  h.clearInput();
  assert.equal(h.keyboard.size, 0);
  assert.equal(h.consumed.size, 0);
}

test_parse_and_configure();
test_modifier_bound_buttons_engage();
test_most_specific_binding_wins();
test_default_turbo_and_game_buttons();
test_slots();
test_chords_own_their_key();
test_unbound_keys_pass_through();
console.log('web host input PASS (modifier bindings, turbo chords, slots vs held Select, chord ownership)');
