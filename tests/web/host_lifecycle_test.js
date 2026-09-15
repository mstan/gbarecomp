// Run with node tests/web/host_lifecycle_test.js. Timers and browser APIs are controlled.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../../packaging/web/host_web.js'), 'utf8');

function fixture({ack = true, closeFails = false, audio = true} = {}) {
  const timers = [], events = [];
  const context = vm.createContext({module: {exports: {}}, navigator: {}, performance,
    cancelAnimationFrame() {}, setTimeout: fn => timers.push(fn),
    ResizeObserver: class { observe() {} disconnect() {} },
  });
  vm.runInContext(source, context);
  const host = new context.module.exports.Host({parentElement: {}});
  const control = new Int32Array(new SharedArrayBuffer(32));
  host.control = control;
  host.buffer = control.buffer;
  host.d = {fields: {detached: 0, keys: 1, inputUpdates: 2, turbo: 3}};
  host.active = true;
  if (audio) {
    host.node = {port: {postMessage(message) {
      assert.equal(message.type, 'stop');
      events.push('stop');
      assert.equal(control[0], 0, 'memory remains owned before worklet ACK');
      if (ack) host.stopAck();
    }}, disconnect() { events.push('disconnect'); }};
    host.audio = {state: 'running', async close() {
      events.push('close');
      assert.equal(control[0], 0, 'ACK alone cannot release memory before context close');
      if (closeFails) throw Error('close failed');
      this.state = 'closed';
    }};
  }
  return {host, control, timers, events};
}

function test_video_ownership() {
  const {host} = fixture({audio: false});
  host.buffer = new SharedArrayBuffer(256);
  host.control = new Int32Array(host.buffer);
  host.ptr = 0;
  host.d = {fields: {middle: 0}, video: 64, videoStride: 32, pixels: 16,
    maxWidth: 2, maxHeight: 2, pixelBytes: 12};
  host.staging = new Uint8Array(12);
  const publish = (slot, seq, width = 2) => {
    const offset = host.d.video + slot * host.d.videoStride;
    new Uint32Array(host.buffer, offset, 4).set([width, 2, width * 3, seq]);
    new Uint8Array(host.buffer, offset + 16, 12).fill(seq);
    Atomics.store(host.control, 0, slot | 4);
  };
  assert.equal(host.acquire(), false, 'clean exchange has no new frame');
  publish(1, 5);
  assert.equal(host.acquire(), true);
  assert.equal(Atomics.load(host.control, 0), 0, 'consumer returns old front to producer');
  assert.equal(host.front, 1);
  assert.equal(host.stats.lastSeq, 5);
  assert(host.staging.every(value => value === 5));
  new Uint8Array(host.buffer, 64 + 32 + 16, 12).fill(99);
  assert(host.staging.every(value => value === 5), 'staging owns a private copy');
  assert.equal(host.acquire(), false, 'same frame is not counted twice');
  publish(2, 8);
  assert.equal(host.acquire(), true);
  assert.equal(Atomics.load(host.control, 0), 1);
  assert.equal(host.stats.consumed, 2);
  assert.equal(host.stats.lastSeq, 8, 'consumer accepts newest frame after dropped publications');
  publish(0, 9, 3);
  assert.throws(() => host.acquire(), /Invalid published video dimensions/);
}

(async () => {
  test_video_ownership();
  for (const options of [{}, {ack: false}, {closeFails: true}, {audio: false}]) {
    const f = fixture(options);
    const pending = f.host.detach();
    assert.equal(f.host.detach(), pending, 'concurrent detach shares cleanup');
    if (options.ack === false) f.timers.forEach(fn => fn());
    await pending;
    const expected = options.ack === false || options.closeFails ? 2 : 1;
    assert.equal(f.control[0], expected, 'safe-to-free requires ACK and confirmed close');
    assert.equal(f.host.finalStats.detached, expected);
    assert.equal(f.host.control, null);
    assert.equal(f.host.buffer, null);
    assert.equal(f.host.getBuffer, null);
    assert.equal(f.host.audio, null);
    assert.equal(f.host.node, null);
    if (options.audio !== false) assert.deepEqual(f.events, ['stop', 'disconnect', 'close']);

    // A later session must execute cleanup again, not reuse a settled promise.
    const next = new Int32Array(new SharedArrayBuffer(32));
    f.host.control = next;
    f.host.buffer = next.buffer;
    await f.host.detach();
    assert.equal(next[0], 1);
  }
  console.log('web host lifecycle PASS (video ownership, ACK, timeout, close failure, repeated detach)');
})().catch(error => { console.error(error); process.exitCode = 1; });
