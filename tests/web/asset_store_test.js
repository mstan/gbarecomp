// Run with node tests/web/asset_store_test.js. Bring-your-own-ROM gate and
// IndexedDB persistence of the player's ROM/BIOS; no browser or real ROM.
'use strict';
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const {AssetStore, BIOS_SIZE} = require('../../packaging/web/asset_store.js');
const {fakeIndexedDB} = require('./fake_indexeddb.js');

const sha1 = bytes => crypto.createHash('sha1').update(bytes).digest('hex');
const rom = new Uint8Array(0x4000 + 0x100).map((_, i) => (i * 7) & 255);
const bios = new Uint8Array(BIOS_SIZE).map((_, i) => (i * 13) & 255);
const make = (options = {}) => {
  const logs = [];
  const store = new AssetStore({romSha1: sha1(rom).toUpperCase(), biosSha1: sha1(bios), subtle: crypto.webcrypto.subtle,
    indexedDB: fakeIndexedDB(), log: (t, e) => logs.push([t, !!e]), ...options});
  store.logs = logs;
  return store;
};

(async () => {
  // The gate: exact SHA-1 only (case-insensitive), with helpful refusals.
  let s = make();
  assert.equal((await s.verifyRom(rom)).ok, true);
  const tampered = rom.slice(); tampered[0x200] ^= 1;
  const refused = await s.verifyRom(tampered);
  assert.equal(refused.ok, false);
  assert.match(refused.error, new RegExp(`its SHA-1 is ${sha1(tampered)}, but this build needs ${sha1(rom)}`));
  assert.match((await s.verifyRom(bios)).error, /GBA BIOS, not a game ROM/);
  assert.match((await s.verifyRom(new Uint8Array(10))).error, /too small/);
  assert.match((await new AssetStore({subtle: crypto.webcrypto.subtle}).verifyRom(rom)).error, /no expected ROM SHA-1/);
  assert.match((await s.verifyBios(rom)).error, /exactly 16384 bytes/);
  const otherBios = bios.slice(); otherBios[5] ^= 0xff;
  const warned = await s.verifyBios(otherBios);
  assert.equal(warned.ok, true, 'an uncatalogued BIOS is accepted, as natively');
  assert.match(warned.warning, /is not the expected/);
  await assert.rejects(new AssetStore({romSha1: sha1(rom), subtle: null}).verifyRom(rom), /secure context/);

  // Accepted files persist in IndexedDB and are re-verified on the next visit.
  const idb = fakeIndexedDB();
  s = make({indexedDB: idb});
  assert.deepEqual(await s.load(), {rom: null, bios: null});
  const wrong = await s.accept('rom', 'bad.gba', tampered);
  assert.equal(wrong.ok, false);
  const r = await s.accept('rom', 'Game (USA).gba', rom);
  assert.equal(r.ok, true); assert.equal(r.stored, true);
  const b = await s.accept('bios', 'gba_bios.bin', bios);
  assert.equal(b.ok, true); assert.equal(b.warning, null);
  let next = make({indexedDB: idb});
  let loaded = await next.load();
  assert.equal(loaded.rom.name, 'Game (USA).gba');
  assert.equal(loaded.rom.sha1, sha1(rom));
  assert.deepEqual(new Uint8Array(loaded.rom.bytes), rom);
  assert.deepEqual(new Uint8Array(loaded.bios.bytes), bios);
  // The ROM key is per game; the BIOS is shared on the origin.
  const other = make({indexedDB: idb, romSha1: 'f'.repeat(40)});
  loaded = await other.load();
  assert.equal(loaded.rom, null);
  assert.equal(loaded.bios.name, 'gba_bios.bin');
  // A stored record that no longer verifies is dropped, with a message.
  const db = idb.databases.get('gbarecomp-assets');
  db.stores.get('files').get('rom:' + sha1(rom)).bytes = tampered.buffer;
  next = make({indexedDB: idb});
  loaded = await next.load();
  assert.equal(loaded.rom, null);
  assert(next.logs.some(([t, e]) => e && /no longer verifies/.test(t)));
  assert.equal(db.stores.get('files').has('rom:' + sha1(rom)), false);
  // Forget clears this game's ROM and the BIOS.
  await next.accept('rom', 'again.gba', rom);
  await next.forget();
  assert.deepEqual(await make({indexedDB: idb}).load(), {rom: null, bios: null});

  // No IndexedDB (private mode, blocked storage): kept for this page only.
  for (const indexedDB of [undefined, fakeIndexedDB({failOpen: true}), fakeIndexedDB({failWrites: true})]) {
    s = make({indexedDB});
    const kept = await s.accept('rom', 'g.gba', rom);
    assert.equal(kept.ok, true);
    assert.equal(kept.stored, false);
    assert(s.logs.some(([t, e]) => e && /kept for this page only/.test(t)));
    assert.equal((await s.load()).rom.name, 'g.gba', 'memory copy serves this page');
  }
  console.log('web asset store PASS (ROM SHA-1 gate, BIOS checks, IndexedDB persistence, re-verify, forget, memory fallback)');
})().catch(error => { console.error(error); process.exitCode = 1; });
