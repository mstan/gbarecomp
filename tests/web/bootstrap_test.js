// Run with node tests/web/bootstrap_test.js. The real bootstrap, SaveStore and
// AssetStore; only DOM/browser services are substituted. No ROM required.
'use strict';
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {fakeIndexedDB} = require('./fake_indexeddb.js');
const source = name => fs.readFileSync(path.join(__dirname, '../../packaging/web', name), 'utf8');
const turn = () => new Promise(resolve => setImmediate(resolve));
const sha1 = bytes => crypto.createHash('sha1').update(bytes).digest('hex');
const ROM = new Uint8Array(0x400).map((_, i) => (i * 5) & 255);
const BIOS = new Uint8Array(16384).map((_, i) => (i * 3) & 255);

function page({search = '', hostname = 'games.example', build = {}, confirmAnswer = true, indexedDB = fakeIndexedDB(), sha = sha1(ROM)} = {}) {
  const elements = new Map(), logs = [], listeners = {}, requests = [], scripts = [];
  let reloads = 0;
  const element = () => ({children: [], style: {}, disabled: false, hidden: false, textContent: '', className: '',
    appendChild(child) { this.children.push(child); }, addEventListener() {}, replaceChildren() { this.children = []; },
    focus() { this.focused = true; }, scrollIntoView() { this.scrolled = true; }, click() { this.clicked = true; }});
  const document = {getElementById(id) { if (!elements.has(id)) elements.set(id, element()); return elements.get(id); },
    createElement: element, addEventListener() {}, body: {appendChild(s) { scripts.push(s.src); }}};
  class Host {
    constructor() { this.stats = {}; }
    preflight() {}
    async startAudio() {}
    async shutdown() {}
    fail(error) { this.error = String(error); }
    snapshot() { return {}; }
  }
  const confirms = [];
  const context = vm.createContext({document, GbrWebHost: Host,
    GBARECOMP_ROM_SHA1: sha, GBARECOMP_BUILD: {romSha1: sha, biosSha1: sha1(BIOS), dev: false, embedded: null, ...build},
    location: {search, hostname, reload() { ++reloads; }}, URLSearchParams, Uint8Array, performance, crypto: crypto.webcrypto,
    indexedDB, navigator: {}, Blob, URL: {createObjectURL() { return 'blob:'; }, revokeObjectURL() {}},
    console: {log: m => logs.push(String(m)), error: m => logs.push(String(m))},
    addEventListener(type, fn) { (listeners[type] ||= []).push(fn); }, setInterval() {},
    confirm: message => { confirms.push(message); return confirmAnswer; },
    fetch: async name => { requests.push(name); return {ok: true, arrayBuffer: async () => new Uint8Array(4).buffer}; },
  });
  for (const file of ['save_store.js', 'asset_store.js', 'bootstrap.js']) vm.runInContext(source(file), context);
  const get = id => document.getElementById(id);
  const pick = async (id, bytes, name) => {
    get(id).files = [{name, arrayBuffer: async () => bytes.slice().buffer}];
    get(id).onchange();
    for (let i = 0; i < 40; ++i) await turn();
  };
  return {context, get, logs, listeners, requests, scripts, confirms, pick, reloads: () => reloads,
    fire: (type, event = {}) => (listeners[type] || []).forEach(fn => fn(event))};
}
const argv = p => Array.from(p.context.Module.arguments);

async function test_bring_your_own_rom() {
  const idb = fakeIndexedDB();
  const p = page({indexedDB: idb});
  await p.context.GbrAssetsLoaded;
  assert.equal(p.get('start').disabled, true, 'Start needs the player\'s files');
  assert.match(p.get('romstatus').textContent, /not chosen/);
  await p.get('start').onclick();
  assert.equal(p.context.Module, undefined);
  // Wrong ROM: refused with the expected hash, never stored.
  const wrong = ROM.slice(); wrong[9] ^= 1;
  await p.pick('romfile', wrong, 'hack.gba');
  assert(p.logs.some(l => /ROM refused: Wrong ROM: its SHA-1 is [0-9a-f]{40}, but this build needs/.test(l)));
  await p.pick('romfile', BIOS, 'gba_bios.bin');
  assert(p.logs.some(l => /GBA BIOS, not a game ROM/.test(l)));
  await p.pick('romfile', ROM, 'game.gba');
  assert(p.logs.some(l => /ROM accepted: game\.gba \(kept in this browser\)/.test(l)));
  assert.equal(p.get('start').disabled, true, 'still needs the BIOS');
  await p.pick('biosfile', BIOS, 'gba_bios.bin');
  assert.equal(p.get('start').disabled, false);
  // The next visit on this origin needs no picking.
  const again = page({indexedDB: idb});
  await again.context.GbrAssetsLoaded;
  assert.equal(again.get('start').disabled, false);
  assert.match(again.get('romstatus').textContent, /game\.gba/);
  await again.get('start').onclick();
  assert.deepEqual(again.requests, ['runtime.toml'], 'no ROM/BIOS is ever fetched from the server');
  const writes = new Map();
  again.context.Module.ENV = {};
  again.context.Module.FS = {mkdir() {}, writeFile: (name, bytes) => writes.set(name, Uint8Array.from(bytes)), mkdirTree() {},
    filesystems: {}};
  again.context.Module.preRun[0]();
  assert.deepEqual(writes.get('/data/game.gba'), ROM);
  assert.deepEqual(writes.get('/data/gba_bios.bin'), BIOS);
  assert.equal(argv(again)[argv(again).indexOf('--rom-sha1') + 1], sha1(ROM));
  // Forget removes them from this browser.
  const forget = page({indexedDB: idb});
  await forget.context.GbrAssetsLoaded;
  await forget.get('forgetassets').onclick();
  const gone = page({indexedDB: idb});
  await gone.context.GbrAssetsLoaded;
  assert.equal(gone.get('start').disabled, true);
}

async function test_developer_parameters() {
  const q = '?args=--no-window%20--frames%205&env=GBARECOMP_X=1&sha1=' + 'f'.repeat(40) + '&rom=other.gba';
  // Normal bundle: every developer parameter is ignored, loudly.
  let p = page({search: q, hostname: 'localhost'});
  await p.context.GbrAssetsLoaded;
  await p.pick('romfile', ROM, 'game.gba');
  await p.pick('biosfile', BIOS, 'gba_bios.bin');
  await p.get('start').onclick();
  let args = argv(p);
  assert.deepEqual(args.slice(args.indexOf('--state-dir') + 2), ['--window']);
  assert.equal(args[args.indexOf('--rom-sha1') + 1], sha1(ROM), '?sha1= never overrides the gate');
  assert(p.logs.some(l => /Ignored \?sha1=/.test(l)));
  assert(p.logs.some(l => /Ignored \?args=: developer URL parameters need a bundle built with --dev/.test(l)));
  p.context.Module.ENV = {};
  p.context.Module.FS = {mkdir() {}, writeFile() {}, mkdirTree() {}, filesystems: {}};
  p.context.Module.preRun[0]();
  assert.deepEqual({...p.context.Module.ENV}, {});
  // --dev bundle, but not served from this machine: still ignored.
  p = page({search: q, hostname: 'games.example', build: {dev: true}});
  await p.context.GbrAssetsLoaded;
  await p.pick('romfile', ROM, 'game.gba');
  await p.pick('biosfile', BIOS, 'gba_bios.bin');
  await p.get('start').onclick();
  args = argv(p);
  assert.deepEqual(args.slice(args.indexOf('--state-dir') + 2), ['--window']);
  // --dev bundle on localhost: argv/env honoured; the SHA-1 gate still is not.
  for (const hostname of ['localhost', '127.0.0.1', '[::1]']) {
    p = page({search: q, hostname, build: {dev: true}});
    await p.context.GbrAssetsLoaded;
    await p.pick('romfile', ROM, 'game.gba');
    await p.pick('biosfile', BIOS, 'gba_bios.bin');
    await p.get('start').onclick();
    args = argv(p);
    assert.deepEqual(args.slice(args.indexOf('--state-dir') + 2), ['--no-window', '--frames', '5'], hostname);
    assert.equal(args[args.indexOf('--rom-sha1') + 1], sha1(ROM));
    p.context.Module.ENV = {};
    p.context.Module.FS = {mkdir() {}, writeFile() {}, mkdirTree() {}, filesystems: {}};
    p.context.Module.preRun[0]();
    assert.equal(p.context.Module.ENV.GBARECOMP_X, '1');
  }
  // Missing build identity: Start refuses instead of running without a gate.
  const bare = page({sha: ''});
  await bare.context.GbrAssetsLoaded;
  assert.equal(bare.get('start').disabled, true);
  await bare.get('start').onclick();
  assert.equal(bare.context.Module, undefined);
  assert(bare.logs.some(l => /no expected ROM SHA-1/.test(l)));
}

async function test_embedded_private_build() {
  const embedded = {rom: 'PRIVATE-game.gba', bios: 'PRIVATE-gba_bios.bin'};
  const p = page({build: {embedded}});
  // fetch returns 4 bytes: the embedded "ROM" does not match the baked SHA-1.
  await p.context.GbrAssetsLoaded;
  assert.match(p.get('romstatus').textContent, /PRIVATE developer build/);
  assert.equal(p.get('start').disabled, false);
  await p.get('start').onclick();
  assert.deepEqual(p.requests.slice(0, 2), ['PRIVATE-game.gba', 'PRIVATE-gba_bios.bin']);
  assert.match(p.context.GbrHost.error, /too small to be a GBA ROM|Wrong ROM/, 'embedded files pass the same gate');
  assert.equal(p.context.Module, undefined);
}

async function test_unload_and_reload_guards() {
  const p = page({confirmAnswer: false});
  await p.context.GbrAssetsLoaded;
  const saves = p.context.GbrSaves;
  // Runtime wrote a battery save that is not in IndexedDB yet.
  const syncs = [];
  saves.mounted = true; saves.fs = {syncfs(populate, done) { syncs.push(done); }};
  saves.onWrite(1, 1);
  const event = {preventDefault() { this.prevented = true; }};
  p.fire('beforeunload', event);
  assert(event.prevented, 'unload warns while a runtime write is not stored');
  p.get('reload').onclick();
  assert.equal(p.reloads(), 0, 'Restart asks before discarding unsaved data');
  assert.equal(p.confirms.length, 1);
  while (syncs.length) syncs.shift()(null);  // every coalesced sync completes
  const clean = {preventDefault() { this.prevented = true; }};
  p.fire('beforeunload', clean);
  assert(!clean.prevented, 'no warning once stored');
  p.get('reload').onclick();
  assert.equal(p.reloads(), 1);
  // Memory-only storage: still warns (the data would be lost).
  const m = page();
  await m.context.GbrAssetsLoaded;
  m.context.GbrSaves.state = 'unavailable';
  m.context.GbrSaves.onWrite(1, 1);
  const lost = {preventDefault() { this.prevented = true; }};
  m.fire('beforeunload', lost);
  assert(lost.prevented);
  assert.match(m.get('savestatus').textContent, /unavailable, NOT stored/);
  // A touch/policy settings request reveals the page controls.
  m.fire('gbrsettings');
  assert(m.get('bar').scrolled);
}

(async () => {
  await test_bring_your_own_rom();
  await test_developer_parameters();
  await test_embedded_private_build();
  await test_unload_and_reload_guards();
  console.log('web bootstrap PASS (bring-your-own ROM gate + persistence, developer parameters, embedded private build, unload/reload guards)');
})().catch(error => { console.error(error); process.exitCode = 1; });
