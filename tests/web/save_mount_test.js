// Run with node tests/web/save_mount_test.js. No IndexedDB or ROM required.
'use strict';
const assert = require('node:assert/strict');
const {SaveStore} = require('../../packaging/web/save_store.js');

function fixture(sha, {idbfs = true, mountFails = false, syncThrows = false} = {}) {
  const files = new Map(), syncs = [], mounts = [], dependencies = new Set(), logs = [];
  const read = path => {
    if (!files.has(path)) throw Error('missing file: ' + path);
    return files.get(path);
  };
  const FS = {
    filesystems: idbfs ? {IDBFS: {}} : {}, mkdirTree() {},
    mount(type, options, dir) { if (mountFails) throw Error('mount failed'); mounts.push(dir); },
    syncfs(populate, done) { if (syncThrows) throw Error('sync threw'); syncs.push({populate, done}); },
    stat: read, readFile: read, isFile: mode => (mode & 0o170000) === 0o100000,
    writeFile: (path, bytes) => files.set(path, typeof bytes === 'string' ? bytes : Uint8Array.from(bytes)),
    rename(from, to) { files.set(to, read(from)); files.delete(from); },
    unlink(path) { read(path); files.delete(path); },
    readdir: dir => ['.', '..', ...Array.from(files.keys())
      .filter(path => path.startsWith(dir + '/')).map(path => path.slice(dir.length + 1))],
  };
  const store = new SaveStore((message, error) => logs.push([message, error]));
  const module = {FS,
    addRunDependency(name) { assert(!dependencies.has(name)); dependencies.add(name); },
    removeRunDependency(name) { assert(dependencies.delete(name)); },
  };
  return {store, files, syncs, mounts, dependencies, logs, dir: '/saves/' + sha,
    mount: () => store.mount(module, sha)};
}

async function test_mount_and_pending_import() {
  for (const sha of ['a'.repeat(40), 'b'.repeat(40)]) {
    const f = fixture(sha);
    const marker = f.dir + '/.legacy-migrated-v1';
    f.files.set(marker, '1');
    f.files.set(f.dir + '/battery.sav', new Uint8Array(512).fill(3));
    f.files.set(f.dir + '/state1.tmp', new Uint8Array(1));
    await f.store.importBattery(new Uint8Array(512).fill(7));
    f.mount();
    assert.deepEqual(f.mounts, [f.dir], 'each ROM mounts its own database, never /saves');
    assert.deepEqual([...f.dependencies], ['saves']);
    assert.equal(f.syncs[0].populate, true);
    assert.equal(f.store.exportBattery()[0], 3, 'queued import waits for population');
    await f.syncs[0].done(null);
    assert.equal(f.dependencies.size, 0, 'main unblocks after loading and pending operations');
    assert.equal(f.store.exportBattery()[0], 7);
    assert.equal(f.files.get(f.dir + '/battery.sav.bak')[0], 3);
    assert(!f.files.has(f.dir + '/state1.tmp'));
    assert.equal(f.syncs.length, 2);
    assert.equal(f.syncs[1].populate, false);
    f.syncs[1].done(null);
    await f.store.flush();
    f.store.apply({op: 'delete'});
    assert.deepEqual([...f.files.keys()], [marker], 'Delete preserves migration marker');
    assert.deepEqual(f.store.files(), [], 'migration marker stays out of user operations');
  }
}

async function test_migration_barrier() {
  const f = fixture('c'.repeat(40));
  let legacyReads = 0;
  f.store.readLegacy = async () => {
    ++legacyReads;
    return [{name: 'battery.sav', contents: new Uint8Array(512).fill(9)}];
  };
  f.mount();
  const loading = f.syncs[0].done(null);
  await Promise.resolve();
  assert.equal(f.syncs.length, 2);
  assert.equal(f.syncs[1].populate, false);
  assert(f.dependencies.has('saves'), 'main stays blocked until migration commits');
  assert.equal(f.store.exportBattery()[0], 9);
  f.syncs[1].done(null);
  await loading;
  assert.equal(f.dependencies.size, 0);
  await f.store.migrateLegacy();
  assert.equal(legacyReads, 1, 'committed migration does not import again');

  const existing = fixture('d'.repeat(40));
  existing.files.set(existing.dir + '/state1', new Uint8Array([5]));
  existing.store.readLegacy = () => { throw Error('must not merge into newer per-game data'); };
  existing.mount();
  const populated = existing.syncs[0].done(null);
  assert.equal(existing.syncs.length, 2);
  existing.syncs[1].done(null);
  await populated;
  assert.equal(existing.store.state, 'idle');
  assert.equal(existing.files.get(existing.dir + '/state1')[0], 5);
}

async function test_storage_failures() {
  for (const options of [{idbfs: false}, {mountFails: true}, {syncThrows: true}, {}]) {
    const f = fixture('e'.repeat(40), options);
    await f.store.importBattery(new Uint8Array(512).fill(4));
    f.mount();
    if (f.syncs.length) await f.syncs[0].done(Error('IndexedDB denied'));
    assert.equal(f.store.state, 'unavailable');
    assert.equal(f.dependencies.size, 0, 'storage failure cannot deadlock startup');
    assert.equal(f.store.exportBattery()[0], 4, 'queued import remains available in MEMFS');
    assert.equal(f.store.persist(), false);
    assert(f.logs.some(([message, error]) => error && /lost on reload/.test(message)));
    await f.store.flush();
  }
}

async function test_modification_results() {
  for (const storage of ['unavailable', 'unmounted', 'pending', 'failure']) {
    for (const operation of ['import', 'restore', 'delete']) {
      const f = fixture('f'.repeat(40));
      f.files.set(f.dir + '/.legacy-migrated-v1', '1');
      f.mount();
      await f.syncs.shift().done(null);
      f.files.set(f.dir + '/battery.sav', new Uint8Array(512).fill(3));
      f.files.set(f.dir + '/battery.sav.bak', new Uint8Array(512).fill(5));
      if (storage === 'unavailable') f.store.state = 'unavailable';
      if (storage === 'unmounted') f.store.mounted = false;
      let settled = false;
      const changing = (operation === 'import' ? f.store.importBattery(new Uint8Array(512).fill(7))
        : operation === 'restore' ? f.store.restoreBackup() : f.store.deleteAll())
        .then(value => { settled = true; return value; });
      if (storage === 'unavailable' || storage === 'unmounted') {
        assert.equal(await changing, 'memory-only');
        assert.equal(f.syncs.length, 0, 'unavailable storage never promises persistence');
      } else {
        // Let modify() pass its initial flush barrier without completing syncfs.
        await Promise.resolve();
        await Promise.resolve();
        assert.equal(f.syncs.length, 1);
        assert.equal(f.syncs[0].populate, false);
        assert.equal(settled, false, 'return value waits for storage confirmation');
        if (storage === 'failure') {
          const rejected = assert.rejects(changing, /quota exceeded/);
          f.syncs[0].done(Error('quota exceeded'));
          await rejected;
        } else {
          f.syncs[0].done(null);
          assert.equal(await changing, 'persisted');
        }
      }
      if (operation === 'delete') assert.deepEqual(f.store.files(), []);
      else {
        assert.equal(f.store.exportBattery()[0], operation === 'import' ? 7 : 5,
          'memory-only and failed-sync changes remain exportable');
        assert.equal(f.files.get(f.dir + '/battery.sav.bak')[0], 3);
      }
    }
  }
  const f = fixture('f'.repeat(40));
  assert.equal(await f.store.importBattery(new Uint8Array(512)), 'queued');
  assert.equal(f.syncs.length, 0);
  assert.equal(f.files.size, 0);
}

const turn = () => new Promise(resolve => setImmediate(resolve));

async function test_queue_keeps_every_operation() {
  // Regression: `pending=[op]` silently dropped earlier queued operations.
  const f = fixture('1'.repeat(40));
  f.files.set(f.dir + '/.legacy-migrated-v1', '1');
  f.files.set(f.dir + '/battery.sav', new Uint8Array(512).fill(1));
  f.files.set(f.dir + '/state2', new Uint8Array(4));
  assert.equal(await f.store.importBattery(new Uint8Array(512).fill(2)), 'queued');
  assert.equal(await f.store.deleteAll(), 'queued');
  assert.equal(await f.store.importBattery(new Uint8Array(8192).fill(3)), 'queued');
  assert.deepEqual(f.store.snapshot().pending, ['import', 'delete', 'import']);
  f.mount();
  await f.syncs[0].done(null);
  // Applied in order: import (backs up 1), delete everything, import 3.
  assert.equal(f.store.exportBattery().length, 8192);
  assert.equal(f.store.exportBattery()[0], 3);
  assert(!f.files.has(f.dir + '/state2'), 'the queued delete ran');
  assert(!f.files.has(f.dir + '/battery.sav.bak'), 'the delete ran before the second import');
  assert.equal(f.store.snapshot().unsaved, true, 'applied but not stored yet');
  f.syncs[1].done(null);
  await turn();
  assert.equal(f.store.snapshot().unsaved, false);
  assert(f.logs.some(([m, e]) => !e && /Queued save change \(import, delete, import\) stored in this browser/.test(m)));
  // Queued before a mount that fails: the log says memory-only, never "stored".
  const u = fixture('2'.repeat(40), {mountFails: true});
  await u.store.importBattery(new Uint8Array(512).fill(4));
  u.mount();
  assert(u.logs.some(([m, e]) => e && /applied in memory only: it is NOT stored/.test(m)));
  assert(!u.logs.some(([m]) => /(?<!NOT )stored in this browser/.test(m)));
  assert.equal(u.store.snapshot().memoryOnly, true);
  assert.equal(u.store.unsaved(), true);
  // Queued before a mount whose sync then fails.
  const q = fixture('3'.repeat(40));
  q.files.set(q.dir + '/.legacy-migrated-v1', '1');
  await q.store.importBattery(new Uint8Array(512).fill(5));
  q.mount();
  await q.syncs[0].done(null);
  q.syncs[1].done(Object.assign(Error('quota exceeded'), {name: 'QuotaExceededError'}));
  await turn();
  assert(q.logs.some(([m, e]) => e && /applied but NOT stored: QuotaExceededError/.test(m)));
  assert.equal(q.store.unsaved(), true);
}

async function test_unsaved_tracking() {
  // beforeunload: a runtime write is unsaved until a sync that started after
  // it completes, not only while a sync is in flight.
  const f = fixture('4'.repeat(40));
  f.files.set(f.dir + '/.legacy-migrated-v1', '1');
  f.mount();
  await f.syncs.shift().done(null);
  assert.equal(f.store.unsaved(), false);
  f.store.onWrite(1, 1);
  assert.equal(f.store.unsaved(), true);
  f.store.onWrite(2, 1);  // coalesced behind the running sync
  f.syncs.shift().done(null);
  assert.equal(f.store.unsaved(), true, 'the second write is not covered by the first sync');
  f.syncs.shift().done(null);
  assert.equal(f.store.unsaved(), false);
  // A failed sync leaves the change unsaved; a failed runtime write is not a change.
  f.store.onWrite(1, 1);
  f.syncs.shift().done(Object.assign(Error('quota'), {name: 'QuotaExceededError'}));
  assert.equal(f.store.state, 'error');
  assert.equal(f.store.unsaved(), true);
  const g = fixture('5'.repeat(40));
  g.files.set(g.dir + '/.legacy-migrated-v1', '1');
  g.mount();
  await g.syncs.shift().done(null);
  g.store.onWrite(1, 0);
  assert.equal(g.store.unsaved(), false);
  assert.equal(g.syncs.length, 0);
  // Storage unavailable: every runtime write is memory-only (and reported so).
  const u = fixture('6'.repeat(40), {idbfs: false});
  u.mount();
  assert.equal(u.store.unsaved(), false);
  u.store.onWrite(1, 1);
  assert.equal(u.store.unsaved(), true);
  assert.equal(u.store.snapshot().memoryOnly, true);
  // Visitor change after exit with unavailable storage: 'memory-only', unsaved.
  assert.equal(await u.store.restoreBackup().catch(e => e.message), 'no battery.sav.bak to restore');
  u.files.set(u.dir + '/battery.sav.bak', new Uint8Array(512));
  assert.equal(await u.store.restoreBackup(), 'memory-only');
  assert.equal(u.store.unsaved(), true);
}

async function test_legacy_database() {
  const {fakeIndexedDB, keyRange} = require('./fake_indexeddb.js');
  const saved = {indexedDB: globalThis.indexedDB, IDBKeyRange: globalThis.IDBKeyRange};
  globalThis.IDBKeyRange = keyRange;
  try {
    // Regression: a foreign '/saves' database without FILE_DATA made readLegacy
    // throw and disabled saving for the origin.
    const idb = globalThis.indexedDB = fakeIndexedDB();
    idb.databases.set('/saves', {version: 1, stores: new Map([['other', new Map([['x', 1]])]])});
    const f = fixture('7'.repeat(40));
    f.mount();
    const loading = f.syncs[0].done(null);  // population; migration commits its marker next
    await turn();
    assert.equal(f.syncs.length, 2, 'migration marker committed');
    f.syncs[1].done(null);
    await loading;
    assert.equal(f.store.state, 'idle', 'saving stays enabled');
    assert(f.files.has(f.dir + '/.legacy-migrated-v1'));
    assert(f.logs.some(([m]) => /Ignored an unrelated IndexedDB database named '\/saves'/.test(m)));
    assert.equal(f.store.persist(), true);
    assert.deepEqual([...idb.databases.get('/saves').stores.keys()], ['other'], 'the foreign database is untouched');

    // A real legacy database migrates this game's flat files only.
    const legacy = globalThis.indexedDB = fakeIndexedDB();
    const dir = '/saves/' + '8'.repeat(40), file = 0o100644;
    legacy.databases.set('/saves', {version: 21, stores: new Map([['FILE_DATA', new Map([
      [dir + '/battery.sav', {mode: file, contents: new Uint8Array(512).fill(6)}],
      [dir + '/state1.tmp', {mode: file, contents: new Uint8Array(1)}],
      [dir + '/sub', {mode: 0o40755}],
      ['/saves/' + '9'.repeat(40) + '/battery.sav', {mode: file, contents: new Uint8Array(512)}],
    ])]])});
    const m = fixture('8'.repeat(40));
    m.mount();
    const migrating = m.syncs[0].done(null);
    for (let i = 0; i < 20 && m.syncs.length < 2; ++i) await turn();
    assert.equal(m.syncs.length, 2);
    assert.equal(m.store.exportBattery()[0], 6);
    assert.deepEqual(m.store.files(), ['battery.sav']);
    m.syncs[1].done(null);
    await migrating;
    assert.equal(m.store.state, 'idle');

    // Legacy read failure: migration skipped (retried next launch), saving works.
    globalThis.indexedDB = fakeIndexedDB({failOpen: true});
    const e = fixture('a1'.repeat(20));
    e.mount();
    await e.syncs[0].done(null);
    await turn();
    assert.equal(e.store.state, 'idle');
    assert(!e.files.has(e.dir + '/.legacy-migrated-v1'), 'no marker: a later launch may still migrate');
    assert(e.logs.some(([msg, err]) => err && /Legacy save migration skipped/.test(msg)));
    assert.equal(e.store.persist(), true);
  } finally {
    globalThis.indexedDB = saved.indexedDB;
    globalThis.IDBKeyRange = saved.IDBKeyRange;
  }
}

(async () => {
  await test_mount_and_pending_import();
  await test_migration_barrier();
  await test_storage_failures();
  await test_modification_results();
  await test_queue_keeps_every_operation();
  await test_unsaved_tracking();
  await test_legacy_database();
  console.log('web save mount PASS (isolation, migration, pending queue order, storage failures, truthful results, unsaved tracking, foreign legacy database)');
})().catch(error => { console.error(error); process.exitCode = 1; });
