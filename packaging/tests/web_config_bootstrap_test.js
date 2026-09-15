// Run with node packaging/tests/web_config_bootstrap_test.js. No DOM or ROM required.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require('node:path').join(__dirname, '../web/bootstrap.js'), 'utf8');

async function launch(sha, configStatus = 200) {
  const elements = new Map(), files = new Map(), requests = [], scripts = [];
  const element = () => ({disabled: false, children: [], appendChild() {},
    addEventListener() {}, replaceChildren() {}});
  const document = {getElementById(id) {
    if (!elements.has(id)) elements.set(id, element());
    return elements.get(id);
  }, createElement: element, addEventListener() {}, body: {appendChild(s) {scripts.push(s.src);}}};
  class Host {
    constructor() {this.stats = {};}
    preflight() {}
    async startAudio() {}
    fail(e) {this.error = String(e);}
    async shutdown() {}
  }
  class Saves {
    snapshot() {return {state: 'idle', pending: []};}
    busy() {return false;}
    async acquireLock(s) {this.sha = s; return true;}
    mount(module, s) {this.mountedSha = s;}
    persist() {}
    async flush() {}
  }
  const runtimeConfig = '[save]\ntype = "eeprom"\nsize = 512\n';
  const context = vm.createContext({document, GbrWebHost: Host, GbrSaveStore: Saves,
    GBARECOMP_ROM_SHA1: sha, location: {search: ''}, URLSearchParams, Uint8Array,
    console: {log() {}, error() {}}, addEventListener() {}, setInterval() {},
    fetch: async name => {
      requests.push(name);
      return {ok: name !== 'runtime.toml' || configStatus === 200, status: configStatus,
        arrayBuffer: async () => Buffer.from(name === 'runtime.toml' ? runtimeConfig : name)};
    }});
  vm.runInContext(source, context);
  await document.getElementById('start').onclick();
  if (configStatus !== 200) {
    assert.equal(context.Module, undefined);
    assert.match(context.GbrHost.error, /runtime.toml: HTTP 404/);
    assert.deepEqual(scripts, []);
    return;
  }
  context.Module.ENV = {};
  context.Module.FS = {mkdir() {}, writeFile(path, bytes) {files.set(path, Buffer.from(bytes).toString());}};
  context.Module.preRun[0]();
  const args = Array.from(context.Module.arguments);
  const value = flag => args[args.indexOf(flag) + 1];
  assert.equal(value('--config'), '/data/runtime.toml');
  assert.equal(files.get(value('--config')), runtimeConfig);
  assert.equal(value('--rom'), '/data/game.gba');
  assert.equal(value('--bios'), '/data/gba_bios.bin');
  assert.equal(value('--save-path'), `/saves/${sha}/battery.sav`);
  assert.equal(value('--state-dir'), `/saves/${sha}`);
  assert.equal(context.GbrSaves.mountedSha, sha);
  assert.deepEqual(requests, ['game.gba', 'gba_bios.bin', 'runtime.toml']);
  assert.deepEqual(scripts, ['game.js']);
}

(async () => {
  await launch('a'.repeat(40));
  await launch('b'.repeat(40));
  await launch('a'.repeat(40), 404);
  console.log('web config bootstrap PASS (preRun, host paths, per-game save paths, missing config)');
})().catch(error => {console.error(error); process.exitCode = 1;});
