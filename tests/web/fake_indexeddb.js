// Minimal in-memory IndexedDB for the web unit tests: open/upgrade, object
// stores with get/put/delete/openCursor over a key range, transactions with
// complete/abort. Asynchronous like the real API (setImmediate).
'use strict';
const later = fn => setImmediate(fn);

function fakeIndexedDB({failOpen = false, failWrites = false} = {}) {
  const databases = new Map();  // name -> {version, stores: Map<name, Map>}
  const request = () => ({result: undefined, error: null});
  function database(name, entry) {
    return {
      name, closed: false,
      objectStoreNames: {contains: store => entry.stores.has(store)},
      createObjectStore(store) { entry.stores.set(store, new Map()); return {}; },
      close() { this.closed = true; },
      transaction(store, mode = 'readonly') {
        if (!entry.stores.has(store)) throw Object.assign(Error('NotFoundError'), {name: 'NotFoundError'});
        const data = entry.stores.get(store), tx = {error: null, pending: 0, aborted: false};
        const finish = () => later(() => {
          if (tx.pending || tx.aborted) return;
          tx.oncomplete?.();
        });
        const op = fn => {
          const r = request(); ++tx.pending;
          later(() => {
            try { r.result = fn(); r.onsuccess?.(); }
            catch (e) { r.error = tx.error = e; tx.aborted = true; tx.onerror?.(); tx.onabort?.(); }
            --tx.pending; finish();
          });
          return r;
        };
        const writable = () => {
          if (mode !== 'readwrite') throw Object.assign(Error('ReadOnlyError'), {name: 'ReadOnlyError'});
          if (failWrites) throw Object.assign(Error('quota exceeded'), {name: 'QuotaExceededError'});
        };
        tx.objectStore = () => ({
          get: key => op(() => data.get(key)),
          put: (value, key) => op(() => { writable(); data.set(key, value); return key; }),
          delete: key => op(() => { writable(); data.delete(key); }),
          openCursor(range) {
            const keys = [...data.keys()].filter(k => !range || range.includes(k)).sort();
            const r = request(); let i = 0; ++tx.pending;
            const step = () => later(() => {
              if (tx.aborted) { --tx.pending; return; }
              if (i >= keys.length) { r.result = null; r.onsuccess?.(); --tx.pending; finish(); return; }
              const key = keys[i++];
              r.result = {key, value: data.get(key), continue: step};
              r.onsuccess?.();
            });
            step();
            return r;
          },
        });
        tx.abort = () => { tx.aborted = true; later(() => tx.onabort?.()); };
        return tx;
      },
    };
  }
  return {
    databases,
    open(name, version) {
      const r = request();
      later(() => {
        if (failOpen) { r.error = Object.assign(Error('denied'), {name: 'SecurityError'}); r.onerror?.(); return; }
        let entry = databases.get(name);
        const fresh = !entry;
        if (fresh) entry = {version: 0, stores: new Map()};
        const target = version || Math.max(1, entry.version);
        const db = database(name, entry);
        r.result = db;
        if (target > entry.version) {
          let aborted = false;
          r.transaction = {abort() { aborted = true; }};
          r.onupgradeneeded?.();
          if (aborted) { r.error = Object.assign(Error('AbortError'), {name: 'AbortError'}); r.onerror?.(); return; }
          entry.version = target;
          databases.set(name, entry);
        }
        r.onsuccess?.();
      });
      return r;
    },
  };
}
const keyRange = {bound: (lower, upper) => ({includes: k => typeof k === 'string' && k >= lower && k <= upper})};
module.exports = {fakeIndexedDB, keyRange};
