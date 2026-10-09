'use strict';
const assert=require('node:assert/strict');
const {Host,layout}=require('../../packaging/web/host_web.js');
const reports=[],h=new Host({},x=>reports.push(x));
h.configure('[player1]\na=4\nb=RShift\nleft=bogus','[KeyMap]\nPause=Shift+P\nFullscreen=Alt+Return\nVolumeUp=F12');
assert.equal(h.keys[0],'KeyA');assert.equal(h.keys[1],'ShiftRight');assert.equal(h.keys[5],'ArrowLeft');assert(reports.some(x=>x.includes('bogus')));
assert.equal(h.hotkeys.Pause,'Shift+KeyP');assert.equal(h.hotkeys.Fullscreen,'Alt+Enter');
assert.deepEqual(layout(600,350,241,160),{x:59,y:15,w:482,h:320,integer:true});
assert.deepEqual(layout(1,1,480,160),{x:0,y:0,w:0,h:0,integer:true});
// Interleaved devices: one release cannot release another device's button.
h.control=new Int32Array(new SharedArrayBuffer(256));h.d={fields:{keys:0,turbo:1,inputUpdates:2,commandWrite:3,commandRead:4,commandOverflow:5},commands:64,commandSlots:4};h.ptr=0;h.buffer=h.control.buffer;h.active=true;
h.keyboard.add('KeyA');h.gamepad=2;h.publishKeys();assert.equal(h.load('keys'),1020);h.keyboard.clear();h.publishKeys();assert.equal(h.load('keys'),1021);h.clearInput();assert.equal(h.load('keys'),1023);
// Command cursor wrap and full queue. No overwritten saves/loads.
h.store('commandRead',0xfffffffe);h.store('commandWrite',0xfffffffe);
for(let i=0;i<4;++i)assert(h.command(i%2?'Load':'Save',i+1));assert(!h.command('Pause'));assert.equal(h.load('commandOverflow'),1);assert.equal(h.load('commandWrite'),2);
assert.deepEqual(Array.from(new Uint32Array(h.buffer,64+2*12,3)),[0xfffffffe,2,1]);
console.log('web host JS PASS (layout, bindings, device union, command overflow/wrap)');

// Browser save store: persist() state machine against a fake FS.syncfs.
const {SaveStore,BATTERY_SIZES}=require('../../packaging/web/save_store.js');
class ErrnoError extends Error{constructor(errno){super('FS error');this.name='ErrnoError';this.errno=errno;}}
const quota=()=>Object.assign(new Error('quota exceeded'),{name:'QuotaExceededError'});
function store(syncfs){const logs=[],s=new SaveStore((t,e)=>logs.push([t,!!e]));s.fs={ErrnoError,syncfs};s.mounted=true;s.logs=logs;return s;}
const rejects=p=>p.then(()=>{throw Error('flush resolved');},e=>e);
(async()=>{
  // Coalescence: requests during a sync become one more sync, never two in flight.
  let calls=[],inFlight=0,maxInFlight=0;
  let s=store((populate,cb)=>{assert.equal(populate,false);calls.push(()=>{--inFlight;cb(null);});maxInFlight=Math.max(maxInFlight,++inFlight);});
  assert(s.persist());assert.equal(s.state,'syncing');assert(s.persist());s.persist();assert.equal(s.state,'again');assert.equal(calls.length,1);
  let flushed=false;const f=s.flush().then(()=>{flushed=true;});
  calls[0]();assert.equal(calls.length,2);assert.equal(s.state,'syncing');await Promise.resolve();assert(!flushed);
  calls[1]();await f;assert(flushed);assert.equal(s.state,'idle');assert.equal(s.syncs,2);assert.equal(maxInFlight,1);assert(s.lastPersisted>0);
  // Runtime notice: ok=1 syncs, ok=0 reports without syncing.
  s.onWrite(1,0);assert.equal(calls.length,2);assert.match(s.writeError,/battery save/);assert(s.logs.some(([t,e])=>e&&/failed to write/.test(t)));
  s.onWrite(2,1);assert.equal(calls.length,3);assert.equal(s.writeError,null);calls[2]();await s.flush();assert.equal(s.writes,2);
  // ErrnoError (rename/unlink raced the local scan) retries, then succeeds.
  let n=0;s=store((p,cb)=>cb(++n<=2?new ErrnoError(44):null));
  s.persist();await s.flush();assert.equal(s.state,'idle');assert.equal(s.retries,2);assert.equal(n,3);assert.equal(s.error,null);
  // ErrnoError more than FS_RETRIES times becomes a visible error.
  n=0;s=store((p,cb)=>{++n;cb(new ErrnoError(44));});
  s.persist();const errno=await rejects(s.flush());assert.match(errno.message,/ErrnoError/);assert.equal(s.state,'error');assert.equal(s.retries,3);assert.equal(n,4);
  // IndexedDB failure: error at once, no retry loop; the next persist() tries again.
  n=0;let fail=true;s=store((p,cb)=>{++n;cb(fail?quota():null);});
  s.persist();assert.equal(s.state,'error');assert.equal(s.retries,0);assert.equal(n,1);assert.match((await rejects(s.flush())).message,/QuotaExceededError/);
  fail=false;s.persist();await s.flush();assert.equal(s.state,'idle');assert.equal(s.error,null);assert.equal(n,2);
  // A throwing syncfs is an error too, never an exception out of persist().
  s=store(()=>{throw quota();});s.persist();assert.equal(s.state,'error');
  // Unmounted/unavailable storage never syncs and flush() resolves.
  s=store(()=>{throw Error('must not sync');});s.mounted=false;assert(!s.persist());await s.flush();
  s=store(()=>{throw Error('must not sync');});s.state='unavailable';assert(!s.persist());await s.flush();
  // Import validation, queued before Start, refused while running.
  s=new SaveStore();
  assert.throws(()=>s.importBattery(new Uint8Array(1000)),/Unsupported save size 1000/);
  assert.deepEqual(BATTERY_SIZES,[512,8192,32768,65536,131072]);
  assert.equal(await s.importBattery(new Uint8Array(8192)),'queued');assert.deepEqual(s.snapshot().pending,['import']);
  s.running=true;await assert.rejects(s.deleteAll(),/Stop the game/);
  // Applying ops against an in-memory FS: backup on import, swap on restore, delete all.
  const files=new Map(),fs={writeFile:(p,d)=>files.set(p,Uint8Array.from(d)),readFile:p=>{if(!files.has(p))throw new ErrnoError(44);return files.get(p);},
    rename:(a,b)=>{files.set(b,fs.readFile(a));files.delete(a);},stat:p=>fs.readFile(p),unlink:p=>{fs.readFile(p);files.delete(p);},mkdirTree:()=>{},
    readdir:d=>['.','..',...[...files.keys()].filter(p=>p.startsWith(d+'/')).map(p=>p.slice(d.length+1))]};
  s=new SaveStore();s.fs=fs;s.dir='/saves/abc';
  s.apply({op:'import',bytes:new Uint8Array(512).fill(1)});assert(!files.has('/saves/abc/battery.sav.bak'));
  s.apply({op:'import',bytes:new Uint8Array(512).fill(2)});assert.equal(files.get('/saves/abc/battery.sav')[0],2);assert.equal(files.get('/saves/abc/battery.sav.bak')[0],1);
  s.apply({op:'restore'});assert.equal(files.get('/saves/abc/battery.sav')[0],1);assert.equal(files.get('/saves/abc/battery.sav.bak')[0],2);
  assert(![...files.keys()].some(p=>p.endsWith('.tmp')));
  files.set('/saves/abc/state1',new Uint8Array(4));files.set('/saves/abc/x.tmp',new Uint8Array(1));assert.equal(s.cleanTmp(),1);
  s.apply({op:'delete'});assert.equal(files.size,0);
  // Export name from the cartridge header (title 0xA0..0xAB, code 0xAC..0xAF).
  const rom=new Uint8Array(0xC0);rom.set(Buffer.from('GBAZELDA MC\0BZMP'),0xA0);
  assert.equal(SaveStore.exportName(rom),'GBAZELDA_MC-BZMP.sav');assert.equal(SaveStore.exportName(null),'game.sav');
  console.log('web save store JS PASS (coalescence, errno retry, IndexedDB error, flush, import/backup/restore/delete, export name)');
})().catch(e=>{console.error(e);process.exit(1);});
