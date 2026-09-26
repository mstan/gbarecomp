// Run with node tests/web/audio_worklet_test.js. No browser, ROM or SDK needed.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../../packaging/web/audio_worklet.js'), 'utf8');

function fixture(factory) {
  const messages = [], pushed = [], resets = [];
  let Processor, frees = 0;
  const dsp = {
    HEAP16: new Int16Array(4096),
    _dsp_init: rate => { assert.equal(rate, 48000); return 0; },
    _dsp_free: () => { ++frees; },
    _dsp_input: () => 0, _dsp_output: () => 4096,
    _dsp_reset: index => resets.push(index),
    _dsp_push: n => pushed.push(Array.from(dsp.HEAP16.subarray(0, n))),
    _dsp_pull: (n, ending) => {
      dsp.HEAP16.fill(16384, 2048, 2048 + n);
      return ending ? 0 : n;
    },
    _dsp_fill: () => 0, _dsp_underruns: () => 3,
    _dsp_concealed: () => 4, _dsp_overflow: () => 5,
  };
  const context = vm.createContext({
    AudioWorkletProcessor: class {
      constructor() { this.port = {postMessage: message => messages.push(message)}; }
    },
    registerProcessor: (name, cls) => { assert.equal(name, 'gbr-audio'); Processor = cls; },
    createGbrAudioDSP: factory || (async () => dsp), sampleRate: 48000,
  });
  vm.runInContext(source, context);
  const processor = new Processor();
  const fields = Object.fromEntries(['hostRate', 'audioReady', 'resetRequest', 'resetAck',
    'audioRead', 'audioWrite', 'audioConsumed', 'audioFill', 'underruns', 'concealed',
    'audioDspOverflow'].map((name, index) => [name, index]));
  const descriptor = {fields, audio: 128, audioSlots: 4, audioSamples: 8, audioStride: 28};
  const buffer = new SharedArrayBuffer(256), control = new Int32Array(buffer);
  const send = data => processor.port.onmessage({data});
  const attach = () => send({type: 'attach', buffer, ptr: 0, descriptor});
  const load = name => Atomics.load(control, fields[name]) >>> 0;
  const store = (name, value) => Atomics.store(control, fields[name], value);
  const block = (cursor, count, rate, epoch = 0) => {
    const offset = descriptor.audio + (cursor & 3) * descriptor.audioStride;
    new Uint32Array(buffer, offset, 3).set([count, rate, epoch]);
    new Int16Array(buffer, offset + 12, 8).set([100, -200, 300, -400]);
  };
  const render = () => {
    const output = new Float32Array(128).fill(1);
    const running = processor.process([], [[output]]);
    return {output, running};
  };
  return {processor, dsp, messages, pushed, resets, attach, send, load, store, block,
    render, frees: () => frees};
}

async function test_queue_and_reset() {
  const f = fixture();
  assert(f.render().output.every(x => x === 0), 'silence before attach');
  await f.attach();
  assert.equal(f.load('audioReady'), 1);
  assert.equal(f.load('hostRate'), 48000);
  assert.equal(f.messages[0].type, 'ready');
  f.store('audioRead', 0xffffffff);
  f.store('audioWrite', 1);
  f.block(0xffffffff, 4, 65536);
  f.block(0, 2, 32768);
  const {output} = f.render();
  assert.deepEqual(f.pushed, [[100, -200, 300, -400], [100, -200]]);
  assert.deepEqual(f.resets, [1, 0], 'rate transition changes DSP bank');
  assert.equal(f.load('audioRead'), 1, 'consumer cursor wraps at uint32');
  assert.equal(f.load('audioConsumed'), 6);
  assert(output.some(x => x > 0) && output.every(x => Number.isFinite(x) && Math.abs(x) <= 1));
  assert.equal(f.load('underruns'), 3);
  assert.equal(f.load('concealed'), 4);
  assert.equal(f.load('audioDspOverflow'), 5);

  f.block(1, 4, 32768);
  f.store('audioWrite', 2);
  f.store('resetRequest', 7);
  assert(f.render().output.every(x => x === 0), 'restore quantum is silent');
  assert.equal(f.load('resetAck'), 7);
  assert.equal(f.load('audioRead'), 2, 'restore discards all old queued blocks');
  assert.equal(f.pushed.length, 2);
  f.block(2, 4, 32768, 7);
  f.store('audioWrite', 3);
  f.render();
  assert.equal(f.load('audioRead'), 3, 'new epoch resumes playback');
  await f.send({type: 'stop'});
  assert.equal(f.messages.at(-1).type, 'stopped');
  assert.equal(f.processor.control, null);
  assert.equal(f.processor.buffer, null);
  const stopped = f.render();
  assert.equal(stopped.running, false);
  assert(stopped.output.every(x => x === 0));
  assert.equal(f.frees(), 1);
}

async function test_bad_metadata() {
  for (const [count, rate] of [[0, 65536], [9, 65536], [4, 48000]]) {
    const f = fixture();
    await f.attach();
    f.block(0, count, rate);
    f.store('audioWrite', 1);
    assert(f.render().output.every(x => x === 0));
    assert.equal(f.load('audioReady'), 2, 'malformed block disables audio');
    assert.equal(f.load('audioRead'), 0, 'malformed block is never consumed');
    assert.deepEqual(f.pushed, []);
  }
}

async function test_stop_during_initialization() {
  let resolve;
  const f = fixture(() => new Promise(done => { resolve = done; }));
  const attaching = f.attach();
  await f.send({type: 'stop'});
  assert.equal(f.messages[0].type, 'stopped');
  resolve(f.dsp);
  await attaching;
  assert.equal(f.frees(), 1, 'late DSP instance is released');
  assert.equal(f.load('audioReady'), 0, 'late initialization cannot publish ready');
  assert.equal(f.processor.control, null);
  assert.equal(f.render().running, false);
  await f.attach();
  assert.equal(f.processor.control, null, 'stopped worklet rejects reattach');

  const bad = fixture(async () => { throw Error('DSP unavailable'); });
  await bad.attach();
  assert.equal(bad.messages[0].type, 'error');
  assert.match(bad.messages[0].message, /DSP unavailable/);
  assert(bad.render().output.every(x => x === 0));
}

(async () => {
  await test_queue_and_reset();
  await test_bad_metadata();
  await test_stop_during_initialization();
  console.log('web audio worklet PASS (queue, rates, reset, invalid metadata, stop races)');
})().catch(error => { console.error(error); process.exitCode = 1; });
