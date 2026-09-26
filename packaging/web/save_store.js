/* Browser-local saves. The battery save (raw chip dump) and save states live
   under /saves/<rom sha1>, each on its own IDBFS mount/database in IndexedDB
   for the page's origin. Nothing is sent to the server and there are no
   external dependencies. The page decides when to sync (FS.syncfs); the runtime
   only reports that it wrote (web_notify_storage_write in host_window_web.cpp).
   Every change is tracked until IndexedDB confirms it (unsaved()), so the page
   can say truthfully whether data is stored or only in memory, and warn before
   a reload would lose it. See packaging/web/README.md. */
(function(root) {
'use strict';
const BATTERY_SIZES=[512,8192,32768,65536,131072];
const FS_RETRIES=3,RETRY_MS=50;
const KIND={1:'battery save',2:'save state'};
const MIGRATION_MARKER='.legacy-migrated-v1';
const describe=e=>e?.name?`${e.name}: ${e.message||e.errno||''}`:String(e);
class SaveStore {
  constructor(log=()=>{},onStatus=()=>{}) {
    this.log=log;this.onStatus=onStatus;
    // idle | syncing | again (coalesced/retrying) | error | unavailable
    this.state='idle';this.fs=null;this.sha1='';this.dir='';this.mounted=false;this.running=false;
    this.lastPersisted=0;this.persistent=null;this.error=null;this.writeError=null;
    this.writes=0;this.syncs=0;this.retries=0;this.lastSyncMs=0;this.attempt=0;
    this.waiters=[];this.pending=[];this.locked=null;
    // changeSeq counts MEMFS changes (runtime writes, visitor operations);
    // persistedSeq is the newest change a completed sync has stored.
    this.changeSeq=0;this.persistedSeq=0;this.syncSeq=0;
  }
  busy(){return this.state==='syncing'||this.state==='again';}
  // True while some change exists only in memory (not yet, or never, stored).
  unsaved(){return this.busy()||this.changeSeq>this.persistedSeq;}
  canPersist(){return this.mounted&&this.state!=='unavailable';}
  emit(){try{this.onStatus(this.snapshot());}catch(e){}}
  snapshot(){return {state:this.state,lastPersisted:this.lastPersisted,persistent:this.persistent,error:this.error,writeError:this.writeError,
    writes:this.writes,syncs:this.syncs,retries:this.retries,lastSyncMs:this.lastSyncMs,sha1:this.sha1,locked:this.locked,pending:this.pending.map(p=>p.op),
    unsaved:this.unsaved(),memoryOnly:this.changeSeq>this.persistedSeq&&!this.canPersist()};}
  // One tab per game: the last tab to write would otherwise win and the other
  // would keep running on a stale save. Resolves false when another tab holds it.
  acquireLock(sha1) {
    const locks=root.navigator?.locks;
    if(!locks){this.locked=false;this.log('Web Locks unavailable: a second tab of this game could overwrite its saves',true);return Promise.resolve(true);}
    return new Promise((resolve,reject)=>{
      locks.request('gbarecomp:'+sha1,{ifAvailable:true},lock=>{
        if(!lock){resolve(false);return undefined;}
        this.locked=true;resolve(true);
        return new Promise(release=>{this.releaseLock=release;}); // held for the page's lifetime
      }).catch(reject);
    });
  }
  // IDBFS names its database after the mount point and syncfs reconciles the
  // entire mount. Mounting /saves would let another game's stale tab erase this
  // game's files, even though the directory and Web Lock are SHA-specific.
  // Load and migrate before main(), retaining the existing runtime paths.
  // Storage failure never blocks the game: it runs on MEMFS with a visible warning.
  mount(Module,sha1) {
    const FS=this.fs=Module.FS;this.sha1=sha1;this.dir='/saves/'+sha1;
    FS.mkdirTree(this.dir);
    const unavailable=e=>{
      this.state='unavailable';this.error=describe(e);this.ensureDir();
      this.log('Browser save storage unavailable: '+this.error+'. The game runs, but saves are lost on reload; export them.',true);
      const applied=this.applyPending();
      if(applied.length)this.log(`Queued save change (${applied.join(', ')}) applied in memory only: it is NOT stored in this browser and is lost on reload. Use Export Save to keep a copy.`,true);
      this.emit();this.settle();
    };
    const IDBFS=FS.filesystems?.IDBFS;
    if(!IDBFS){unavailable('IDBFS is missing from this bundle');return;}
    try{FS.mount(IDBFS,{},this.dir);}catch(e){unavailable(e);return;}
    this.mounted=true;this.state='syncing';this.emit();
    Module.addRunDependency('saves');
    const done=()=>{Module.removeRunDependency('saves');};
    try {
      FS.syncfs(true,async err=>{
        try {
          if(err)throw err;
          await this.migrateLegacy();
          const coalesced=this.state==='again';
          this.ensureDir();const stale=this.cleanTmp();
          this.state='idle';this.error=null;
          this.log(`Browser saves loaded (${this.dir}: ${this.files().join(', ')||'empty'}${stale?`; removed ${stale} stale temp file(s)`:''})`);
          const applied=this.applyPending();
          if(applied.length||coalesced)this.persist();else this.settle();
          this.emit();
          if(applied.length)this.flush().then(()=>this.log(`Queued save change (${applied.join(', ')}) stored in this browser`),
            e=>this.log(`Queued save change (${applied.join(', ')}) applied but NOT stored: ${e.message}. Use Export Save to keep a copy.`,true));
        }catch(e){unavailable(e);}
        finally{done();}
      });
    }catch(e){unavailable(e);done();}
  }
  // Read only this game's flat files from the old shared /saves database.
  // Never mount/sync/delete that database: older bundles may still use it.
  readLegacy() {
    return new Promise((resolve,reject)=>{
      let absent=false;
      const request=root.indexedDB.open('/saves');
      // Opening a nonexistent database would create it. Abort that upgrade so
      // new users do not leave an empty legacy database behind.
      request.onupgradeneeded=()=>{absent=true;request.transaction.abort();};
      request.onerror=()=>absent?resolve([]):reject(request.error);
      request.onsuccess=()=>{
        const db=request.result;
        db.onversionchange=()=>db.close();
        try {
          // Some other script on this origin owns a database named '/saves'.
          // It is not ours to read: skip migration, keep saving.
          if(!db.objectStoreNames.contains('FILE_DATA')){db.close();resolve(null);return;}
          const transaction=db.transaction('FILE_DATA','readonly'),entries=[];
          const prefix=this.dir+'/';
          const cursor=transaction.objectStore('FILE_DATA').openCursor(root.IDBKeyRange.bound(prefix,prefix+'\uffff'));
          cursor.onsuccess=()=>{
            try {
              const item=cursor.result;
              if(!item)return;
              const name=item.key.slice(prefix.length),entry=item.value;
              if(name&&!name.includes('/')&&!name.endsWith('.tmp')&&name!==MIGRATION_MARKER&&this.fs.isFile(entry.mode)) {
                if(!ArrayBuffer.isView(entry.contents)&&!Array.isArray(entry.contents))throw Error('Invalid legacy save contents');
                entries.push({name,contents:entry.contents});
              }
              item.continue();
            }catch(e){transaction.abort();reject(e);}
          };
          transaction.oncomplete=()=>{db.close();resolve(entries);};
          transaction.onabort=transaction.onerror=()=>{db.close();reject(transaction.error||Error('Legacy save read failed'));};
        }catch(e){db.close();reject(e);}
      };
    });
  }
  async migrateLegacy() {
    const marker=this.dir+'/'+MIGRATION_MARKER;
    try{this.fs.stat(marker);return;}catch(e){}
    // An existing per-game database is authoritative. Never merge an old
    // backup/state into newer data, or resurrect files deliberately deleted.
    if(!this.files().some(name=>!name.endsWith('.tmp'))) {
      let entries;
      try{entries=await this.readLegacy();}
      catch(e){
        // Never let an old or foreign database disable saving. Without the
        // marker the migration is retried next launch while this game has no
        // files of its own yet; legacy copies are never modified.
        this.log('Legacy save migration skipped ('+describe(e)+'); saving works normally and any old saves are left untouched',true);
        return;
      }
      if(entries===null)this.log("Ignored an unrelated IndexedDB database named '/saves' (not a gbarecomp save store)");
      for(const {name,contents} of entries||[])this.fs.writeFile(this.dir+'/'+name,contents);
      if(entries?.length)this.log(`Migrated ${entries.length} save file(s) for this game; legacy copies kept`);
    }
    this.fs.writeFile(marker,'1');
    // Commit migration and its marker together before guest writes are allowed.
    // On failure mount() disables persistence; old data remains recoverable.
    await new Promise((resolve,reject)=>this.fs.syncfs(false,err=>err?reject(err):resolve()));
  }
  ensureDir(){try{this.fs.mkdirTree(this.dir);}catch(e){}}
  // Keep the migration marker out of user file operations, especially Delete:
  // deleting it would import old saves again on the next launch.
  files(){try{return this.fs.readdir(this.dir).filter(n=>n!=='.'&&n!=='..'&&n!==MIGRATION_MARKER);}catch(e){return [];}}
  cleanTmp(){let n=0;for(const name of this.files())if(name.endsWith('.tmp')){try{this.fs.unlink(this.dir+'/'+name);++n;}catch(e){}}return n;}
  // Runtime notice (worker -> page). ok=0 means the runtime's own write failed.
  onWrite(kind,ok) {
    ++this.writes;
    if(!ok){this.writeError=`the runtime failed to write the ${KIND[kind]||'save'}`;this.log('Save error: '+this.writeError+' (see the log above)',true);this.emit();return;}
    this.writeError=null;++this.changeSeq;
    if(!this.persist())this.emit();
  }
  // Sync MEMFS -> IndexedDB. Coalesced: never more than one syncfs in flight.
  persist() {
    if(!this.mounted||this.state==='unavailable')return false;
    if(this.busy()){this.state='again';return true;}
    this.attempt=0;this.sync();return true;
  }
  sync() {
    this.state='syncing';++this.syncs;this.syncSeq=this.changeSeq;this.emit();
    const t0=performance.now();
    try{this.fs.syncfs(false,err=>this.synced(err,t0));}catch(e){this.synced(e,t0);}
  }
  synced(err,t0) {
    this.lastSyncMs=performance.now()-t0;
    if(!err) {
      this.attempt=0;this.error=null;this.lastPersisted=Date.now();
      this.persistedSeq=Math.max(this.persistedSeq,this.syncSeq);
      if(this.state==='again'){this.sync();return;}
      this.state='idle';this.emit();this.settle();void this.requestPersistence();return;
    }
    // A path listed by the synchronous local scan was renamed or removed by a
    // concurrent worker write (typically *.tmp) before the IndexedDB
    // transaction. That is a race, not a storage failure: retry a few times.
    if(this.fs.ErrnoError&&err instanceof this.fs.ErrnoError&&this.attempt<FS_RETRIES) {
      ++this.retries;++this.attempt;this.state='again';
      this.log(`Save sync raced a concurrent write (errno ${err.errno}); retry ${this.attempt}/${FS_RETRIES}`);
      setTimeout(()=>this.sync(),RETRY_MS);return;
    }
    // IndexedDB failures (QuotaExceededError, AbortError, ...) are not retried in
    // a loop; the next write or pause tries again.
    this.attempt=0;this.state='error';this.error=describe(err);
    this.log('Browser save sync failed: '+this.error,true);this.emit();this.settle();
  }
  settle() {
    if(this.busy())return;
    const waiters=this.waiters;this.waiters=[];
    for(const w of waiters)this.state==='error'?w.reject(Error(this.error)):w.resolve();
  }
  // Resolves once nothing is pending; rejects when the last sync failed.
  flush() {
    if(this.state==='error')return Promise.reject(Error(this.error));
    if(!this.busy())return Promise.resolve();
    return new Promise((resolve,reject)=>this.waiters.push({resolve,reject}));
  }
  async requestPersistence() {
    const storage=root.navigator?.storage;
    if(this.persistent!==null||!storage?.persist)return;
    this.persistent='pending';
    try {
      this.persistent=(await storage.persisted?.())||await storage.persist();
      this.log(this.persistent?'Browser storage for this site is persistent':'Browser storage is best-effort: the browser may evict it. Export your save to keep a copy.');
    }catch(e){this.persistent=false;this.log('storage.persist() failed: '+describe(e),true);}
    this.emit();
  }
  estimate(){return root.navigator?.storage?.estimate?root.navigator.storage.estimate():Promise.resolve(null);}
  batteryPath(){return this.dir+'/battery.sav';}
  // Bytes the runtime already flushed (complete: it writes .tmp then renames).
  exportBattery() {
    if(!this.fs)throw Error('Start the game once to open its saves');
    try{return this.fs.readFile(this.batteryPath());}catch(e){throw Error('No battery save yet for this game');}
  }
  static exportName(rom) {
    if(!rom||rom.length<0xB0)return 'game.sav';
    const text=(a,b)=>String.fromCharCode(...rom.subarray(a,b)).replace(/[^A-Za-z0-9 _-]/g,'').trim();
    const title=text(0xA0,0xAC).replace(/\s+/g,'_')||'game',code=text(0xAC,0xB0);
    return `${title}${code?'-'+code:''}.sav`;
  }
  // The runtime stays the authority on sizes: a file larger than the chip is
  // refused at start, a smaller one is padded with 0xFF.
  importBattery(bytes) {
    bytes=new Uint8Array(bytes);
    if(!BATTERY_SIZES.includes(bytes.length))throw Error(`Unsupported save size ${bytes.length} bytes (expected one of ${BATTERY_SIZES.join(', ')})`);
    return this.modify({op:'import',bytes});
  }
  restoreBackup(){return this.modify({op:'restore'});}
  deleteAll(){return this.modify({op:'delete'});}
  // Only while the game is not running. Before the first Start there is no FS
  // yet: every change is queued, in order, and applied right after /saves
  // loads, before main() reads the save; the log then says whether it was
  // stored. After exit it is applied and synced immediately.
  // 'persisted' confirms IndexedDB stored this change; 'memory-only' means it
  // cannot survive a reload. A failed sync rejects, leaving the files exportable.
  async modify(op) {
    if(this.running)throw Error('Stop the game before changing its saves');
    if(!this.fs){
      this.pending.push(op);
      this.log(`Save ${op.op} queued (${this.pending.length} pending): applied in order when the game starts`);
      this.emit();return 'queued';
    }
    await this.flush().catch(()=>{});
    this.apply(op);
    const seq=this.changeSeq;
    if(!this.persist()){this.emit();return 'memory-only';}
    await this.flush();
    this.emit();return this.persistedSeq>=seq?'persisted':'memory-only';
  }
  applyPending() {
    const ops=this.pending;this.pending=[];const applied=[];
    for(const op of ops){try{this.apply(op);applied.push(op.op);}catch(e){this.log('Save '+op.op+' failed: '+e.message,true);}}
    return applied;
  }
  apply(op) {
    const FS=this.fs,path=this.batteryPath(),bak=path+'.bak';
    const exists=p=>{try{FS.stat(p);return true;}catch(e){return false;}};
    const write=(p,data)=>{FS.writeFile(p+'.tmp',data);FS.rename(p+'.tmp',p);};
    this.ensureDir();++this.changeSeq;
    if(op.op==='import') {
      if(exists(path))write(bak,FS.readFile(path));
      write(path,op.bytes);
      this.log(`Imported battery save (${op.bytes.length} bytes)${exists(bak)?'; previous copy kept as battery.sav.bak':''}`);
    }else if(op.op==='restore') {
      if(!exists(bak))throw Error('no battery.sav.bak to restore');
      const current=exists(path)?FS.readFile(path):null;
      write(path,FS.readFile(bak));if(current)write(bak,current);
      this.log('Restored battery.sav from battery.sav.bak (the replaced save is now the backup)');
    }else if(op.op==='delete') {
      const names=this.files();for(const name of names)FS.unlink(this.dir+'/'+name);
      this.log(`Deleted ${names.length} saved file(s) for this game`);
    }else throw Error('unknown save operation '+op.op);
  }
}
root.GbrSaveStore=SaveStore;
if(typeof module!=='undefined')module.exports={SaveStore,BATTERY_SIZES};
})(globalThis);
