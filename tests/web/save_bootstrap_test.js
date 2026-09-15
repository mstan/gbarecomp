// Real bootstrap + SaveStore; only browser/FS services are substituted.
// Run with node tests/web/save_bootstrap_test.js. No ROM or browser required.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = name => fs.readFileSync(path.join(__dirname, '../../packaging/web', name), 'utf8');
const turn = () => new Promise(resolve => setImmediate(resolve));

function fixture() {
  const elements = new Map(), files = new Map(), syncs = [], logs = [], exports = [];
  let reloads = 0;
  const element = () => ({children: [], style: {}, disabled: false,
    appendChild(child) { this.children.push(child); },
    addEventListener() {}, replaceChildren() { this.children = []; }});
  const document = {
    getElementById(id) {
      if (!elements.has(id)) elements.set(id, element());
      return elements.get(id);
    }, createElement: element, addEventListener() {}, body: element(),
  };
  class Host {
    constructor() { this.stats = {}; }
    preflight() {}
    async startAudio() {}
    async shutdown() {}
    fail(error) { throw error; }
  }
  const read = name => {
    if (!files.has(name)) throw Error('missing file: ' + name);
    return files.get(name);
  };
  const FS = {
    mkdirTree() {}, stat: read, readFile: read,
    writeFile: (name, bytes) => files.set(name, Uint8Array.from(bytes)),
    rename(from, to) { files.set(to, read(from)); files.delete(from); },
    unlink(name) { read(name); files.delete(name); },
    readdir: dir => ['.', '..', ...Array.from(files.keys())
      .filter(name => name.startsWith(dir + '/')).map(name => name.slice(dir.length + 1))],
    syncfs(populate, done) { assert.equal(populate, false); syncs.push(done); },
  };
  const context = vm.createContext({document, GbrWebHost: Host,
    GBARECOMP_ROM_SHA1: 'a'.repeat(40),
    location: {search: '', reload() { ++reloads; }}, URLSearchParams,
    Uint8Array, Blob, performance,
    URL: {createObjectURL(blob) { exports.push(blob); return 'blob:test'; }, revokeObjectURL() {}},
    console: {log: message => logs.push(message), error: message => logs.push(message)},
    addEventListener() {}, setInterval() {}, confirm: () => true,
    fetch: async () => ({ok: true, arrayBuffer: async () => new ArrayBuffer(0)}),
  });
  vm.runInContext(source('save_store.js'), context);
  vm.runInContext(source('bootstrap.js'), context);
  const get = id => document.getElementById(id);
  const saves = context.GbrSaves;
  const dir = '/saves/' + 'a'.repeat(40);
  async function exit(code) {
    await get('start').onclick();
    assert(context.Module, 'bootstrap created the runtime');
    // Initialization/mounting has its own suite. Here guest writes are complete
    // and the exit callbacks run with persistence unavailable until the test
    // chooses its storage scenario below.
    saves.fs = FS;
    saves.dir = dir;
    saves.state = 'unavailable';
    const exited = new Promise(resolve => { context.GbrExit = resolve; });
    context.Module.onExit(code);
    assert.equal(await exited, code);
    logs.length = 0;
  }
  function change(operation) {
    if (operation === 'import') {
      get('importfile').files = [{arrayBuffer: async () => new Uint8Array(512).fill(7).buffer}];
      return get('importfile').onchange();
    }
    return get(operation === 'restore' ? 'restoresave' : 'deletesaves').onclick();
  }
  return {saves, files, syncs, logs, exports, dir, exit, change, get, reloads: () => reloads};
}

async function test_post_exit_changes() {
  for (const code of [0, 1]) {
    for (const storage of ['unavailable', 'unmounted', 'success', 'failure']) {
      for (const operation of ['import', 'restore', 'delete']) {
        const f = fixture();
        await f.exit(code);
        f.files.set(f.dir + '/battery.sav', new Uint8Array(512).fill(3));
        f.files.set(f.dir + '/battery.sav.bak', new Uint8Array(512).fill(5));
        f.saves.mounted = storage !== 'unmounted';
        f.saves.state = storage === 'unavailable' ? 'unavailable' : 'idle';
        const changing = f.change(operation);
        await turn();
        const context = `${operation}, ${storage}, exit ${code}`;
        assert.equal(f.reloads(), 0, context + ': no reload without confirmed persistence');
        assert(!f.logs.some(line => /stored; reloading/.test(line)), context);
        if (storage === 'success' || storage === 'failure') {
          assert.equal(f.syncs.length, 1, context);
          f.syncs[0](storage === 'failure' ? Error('quota exceeded') : null);
        } else {
          assert.equal(f.syncs.length, 0, context);
          assert(f.logs.some(line => /only in memory.*lost on reload.*Export Save/.test(line)), context);
        }
        await changing;
        await turn();  // Delete's UI handler deliberately does not return a promise.
        assert.equal(f.reloads(), storage === 'success' ? 1 : 0, context);
        assert.equal(f.logs.some(line => /stored; reloading/.test(line)), storage === 'success', context);
        if (storage === 'failure') assert(f.logs.some(line => /failed:.*quota exceeded/.test(line)), context);
        if (operation === 'delete') assert.equal(f.files.size, 0, context);
        else if (storage !== 'success') {
          // Exercise the actual Export Save handler while the page remains open.
          assert.equal(f.get('exportsave').disabled, false, context);
          f.get('exportsave').onclick();
          assert.equal(f.exports.length, 1, context);
          const bytes = new Uint8Array(await f.exports[0].arrayBuffer());
          assert.equal(bytes.length, 512, context);
          assert.equal(bytes[0], operation === 'import' ? 7 : 5, context);
        }
      }
    }
  }
}

async function test_import_before_start() {
  const f = fixture();
  await f.change('import');
  assert.equal(f.reloads(), 0);
  assert.equal(f.syncs.length, 0);
  assert.equal(f.files.size, 0);
  assert.equal(f.saves.pending[0].op, 'import');
  assert.equal(f.saves.pending[0].bytes[0], 7);
  assert(!f.logs.some(line => /stored; reloading/.test(line)));
}

(async () => {
  await test_post_exit_changes();
  await test_import_before_start();
  console.log('web save bootstrap PASS (reload after confirmed sync only, failures, export, queued import)');
})().catch(error => { console.error(error); process.exitCode = 1; });
