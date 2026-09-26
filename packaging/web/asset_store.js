/* Bring-your-own-ROM: the player's ROM and BIOS, picked once per origin.
   A public bundle ships neither file (build_web.sh). The ROM must match the
   SHA-1 baked into build_info.js; the BIOS must be a 16 KiB GBA BIOS (an
   uncatalogued SHA-1 is a warning, as in the native runtime). Accepted files
   stay in this browser's IndexedDB for this site and are never uploaded;
   without IndexedDB they are kept for this page only. */
(function(root) {
'use strict';
const DB_NAME='gbarecomp-assets',STORE='files',BIOS_SIZE=16384;
const hex=buffer=>Array.from(new Uint8Array(buffer),b=>b.toString(16).padStart(2,'0')).join('');
const describe=e=>e?.name?`${e.name}: ${e.message||''}`:String(e);
class AssetStore {
  constructor({romSha1='',biosSha1='',log=()=>{},indexedDB=root.indexedDB,subtle=root.crypto?.subtle}={}) {
    this.romSha1=String(romSha1).toLowerCase();this.biosSha1=String(biosSha1).toLowerCase();
    this.log=log;this.idb=indexedDB;this.subtle=subtle;
    this.memory=new Map();this.persistent=null;this.error=null;
  }
  romKey(){return 'rom:'+this.romSha1;}
  async sha1(bytes) {
    if(!this.subtle)throw Error('SHA-1 verification needs a secure context (https or localhost)');
    return hex(await this.subtle.digest('SHA-1',bytes));
  }
  // ROM gate: refuse anything but the exact dump this build was made from.
  async verifyRom(bytes) {
    bytes=new Uint8Array(bytes);
    if(!this.romSha1)return {ok:false,error:'This bundle has no expected ROM SHA-1; rebuild it with packaging/web/build_web.sh.'};
    if(bytes.length===BIOS_SIZE)return {ok:false,error:'That file is 16 KiB, which is a GBA BIOS, not a game ROM. Pick the game ROM (.gba).'};
    if(bytes.length<0xC0)return {ok:false,error:`That file (${bytes.length} bytes) is too small to be a GBA ROM.`};
    const sha1=await this.sha1(bytes);
    if(sha1!==this.romSha1)return {ok:false,sha1,error:`Wrong ROM: its SHA-1 is ${sha1}, but this build needs ${this.romSha1}. Use an unmodified dump of the exact game revision this page was built for.`};
    return {ok:true,sha1};
  }
  async verifyBios(bytes) {
    bytes=new Uint8Array(bytes);
    if(bytes.length!==BIOS_SIZE)return {ok:false,error:`A GBA BIOS is exactly 16384 bytes; that file is ${bytes.length} bytes.`};
    const sha1=await this.sha1(bytes);
    const warning=this.biosSha1&&sha1!==this.biosSha1?`BIOS SHA-1 ${sha1} is not the expected ${this.biosSha1}; trying it anyway.`:null;
    return {ok:true,sha1,warning};
  }
  open() {
    if(this.db)return Promise.resolve(this.db);
    if(!this.idb)return Promise.reject(Error('IndexedDB is unavailable'));
    return new Promise((resolve,reject)=>{
      let request;
      try{request=this.idb.open(DB_NAME,1);}catch(e){reject(e);return;}
      request.onupgradeneeded=()=>{const db=request.result;if(!db.objectStoreNames.contains(STORE))db.createObjectStore(STORE);};
      request.onsuccess=()=>{this.db=request.result;this.db.onversionchange=()=>{this.db.close();this.db=null;};resolve(this.db);};
      request.onerror=()=>reject(request.error||Error('IndexedDB open failed'));
      request.onblocked=()=>reject(Error('IndexedDB is blocked by another tab'));
    });
  }
  async transact(mode,fn) {
    const db=await this.open();
    return new Promise((resolve,reject)=>{
      const tx=db.transaction(STORE,mode),store=tx.objectStore(STORE);let result;
      const request=fn(store);if(request)request.onsuccess=()=>{result=request.result;};
      tx.oncomplete=()=>resolve(result);
      tx.onerror=tx.onabort=()=>reject(tx.error||Error('IndexedDB transaction failed'));
    });
  }
  async read(key) {
    if(this.memory.has(key))return this.memory.get(key);
    try{const v=await this.transact('readonly',s=>s.get(key));this.persistent=true;return v||null;}
    catch(e){this.persistent=false;this.error=describe(e);return null;}
  }
  // Returns true when stored in IndexedDB, false when kept in memory only.
  async write(key,record) {
    try{await this.transact('readwrite',s=>s.put(record,key));this.memory.delete(key);this.persistent=true;return true;}
    catch(e){
      this.memory.set(key,record);this.persistent=false;this.error=describe(e);
      this.log(`Could not keep the ${record.kind} in this browser (${this.error}); it is kept for this page only, so you will have to pick it again next time.`,true);
      return false;
    }
  }
  // Stored files are re-verified: a corrupted or replaced record is dropped.
  async load() {
    const out={rom:null,bios:null};
    const rom=await this.read(this.romKey());
    if(rom?.bytes){const v=await this.verifyRom(rom.bytes);if(v.ok)out.rom=rom;else{this.log('Stored ROM no longer verifies; please pick it again. '+v.error,true);await this.remove(this.romKey());}}
    const bios=await this.read('bios');
    if(bios?.bytes){const v=await this.verifyBios(bios.bytes);if(v.ok)out.bios=bios;else{this.log('Stored BIOS is invalid; please pick it again. '+v.error,true);await this.remove('bios');}}
    return out;
  }
  // kind: 'rom' | 'bios'. Resolves {ok, record, stored, warning} or {ok:false, error}.
  async accept(kind,name,bytes) {
    bytes=new Uint8Array(bytes);
    const v=kind==='rom'?await this.verifyRom(bytes):await this.verifyBios(bytes);
    if(!v.ok)return v;
    const record={kind,name:String(name||kind),sha1:v.sha1,size:bytes.length,bytes:bytes.slice().buffer,added:Date.now()};
    const stored=await this.write(kind==='rom'?this.romKey():'bios',record);
    return {ok:true,record,stored,warning:v.warning||null};
  }
  async remove(key) {
    this.memory.delete(key);
    try{await this.transact('readwrite',s=>s.delete(key));}catch(e){this.error=describe(e);}
  }
  // Forget this game's ROM and the shared BIOS on this origin.
  async forget(){await this.remove(this.romKey());await this.remove('bios');}
}
root.GbrAssetStore=AssetStore;
if(typeof module!=='undefined')module.exports={AssetStore,BIOS_SIZE,DB_NAME};
})(globalThis);
