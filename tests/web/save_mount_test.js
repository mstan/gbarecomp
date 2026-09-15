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
    stat: read, readFile: read,
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

(async () => {
  await test_mount_and_pending_import();
  await test_migration_barrier();
  await test_storage_failures();
  await test_modification_results();
  console.log('web save mount PASS (isolation, migration, pending import, storage failures, modification results)');
})().catch(error => { console.error(error); process.exitCode = 1; });
