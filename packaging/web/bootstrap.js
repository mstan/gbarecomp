'use strict';
(()=>{
const $=id=>document.getElementById(id),q=new URLSearchParams(location.search);
let started=false,running=false,exitCode=null,blocked=null,urls=[],romBytes=null,picked={rom:null,bios:null};
function line(text,error=false){text=String(text).slice(0,2000);(error?console.error:console.log)(text);const el=document.createElement('div');el.textContent=text;if(error)el.className='err';$('log').appendChild(el);while($('log').children.length>4000)$('log').firstChild.remove();}
// build_info.js (build_web.sh): expected hashes, embedded PRIVATE assets, dev flag.
const build=globalThis.GBARECOMP_BUILD||{};
const sha=String(globalThis.GBARECOMP_ROM_SHA1||build.romSha1||'').toLowerCase();
const embedded=build.embedded&&build.embedded.rom&&build.embedded.bios?build.embedded:null;
// Developer URL parameters (?args=, ?env=, ?rom=, ?bios=) replace runtime
// argv/environment. Only a bundle built with --dev honours them, and only
// when served from this machine. Nothing can override the ROM SHA-1 gate.
const LOOPBACK=['localhost','127.0.0.1','[::1]','::1'];
const dev=build.dev===true&&LOOPBACK.includes(location.hostname);
const devParam=name=>dev?q.get(name):null;
const host=globalThis.GbrHost=new GbrWebHost($('canvas'),t=>line(t));
host.gameId=sha;
// Battery saves and save states stay in this browser (IndexedDB), keyed by the
// verified ROM SHA-1. See packaging/web/save_store.js.
const saves=globalThis.GbrSaves=new GbrSaveStore((t,error)=>line(t,error),()=>renderSaves());
const assets=globalThis.GbrAssets=typeof GbrAssetStore==='function'?new GbrAssetStore({romSha1:sha,biosSha1:build.biosSha1||'',log:(t,error)=>line(t,error)}):null;
for(const name of ['args','env','rom','bios','sha1'])if(q.has(name)&&!(dev&&name!=='sha1'))line(name==='sha1'?'Ignored ?sha1=: the ROM SHA-1 is fixed by this build':`Ignored ?${name}=: developer URL parameters need a bundle built with --dev, served from localhost`,true);
// Files in MEMFS are complete (.tmp + rename), so syncing after exit/abort is safe.
// Never rely on the runtime's last async notice having arrived first.
async function settleSaves(){saves.persist();try{await saves.flush();}catch(e){line('Save was not stored in this browser: '+e.message,true);}}
function stopped(){running=false;saves.running=false;renderSaves();renderAssets();}
function fail(e){host.fail(e);$('status').textContent='failed: '+e;line(e,true);stopped();void settleSaves().finally(()=>{$('reload').disabled=false;renderSaves();});void host.shutdown();}
function offer(name,data,type='application/octet-stream'){const url=URL.createObjectURL(new Blob([data],{type}));urls.push(url);const a=document.createElement('a');a.href=url;a.download=name;a.textContent=name;a.style.marginRight='12px';$('downloads').appendChild(a);}
addEventListener('error',e=>line(e.message,true));addEventListener('unhandledrejection',e=>line(e.reason,true));
async function fetchBytes(name){const r=await fetch(name);if(!r.ok)throw Error(`${name}: HTTP ${r.status}`);return new Uint8Array(await r.arrayBuffer());}
// ROM + BIOS: embedded PRIVATE files (developer build) or the player's own.
async function gameAssets(){
 if(embedded){
  const [rom,bios]=await Promise.all([fetchBytes(devParam('rom')||embedded.rom),fetchBytes(devParam('bios')||embedded.bios)]);
  if(assets){
   const r=await assets.verifyRom(rom);if(!r.ok)throw Error(r.error);
   const b=await assets.verifyBios(bios);if(!b.ok)throw Error(b.error);if(b.warning)line(b.warning,true);
  }
  return [rom,bios];
 }
 if(!picked.rom||!picked.bios)throw Error('Choose your ROM and GBA BIOS first');
 return [new Uint8Array(picked.rom.bytes),new Uint8Array(picked.bios.bytes)];
}
$('start').onclick=async()=>{
 if(started)return;
 if(!sha){line('This bundle has no expected ROM SHA-1 (build_info.js); rebuild it with packaging/web/build_web.sh.',true);return;}
 if(!embedded&&(!picked.rom||!picked.bios)){line('Choose your ROM and GBA BIOS first.',true);renderAssets();return;}
 started=true;$('start').disabled=true;$('status').textContent='loading';host.stats.state='loading';renderAssets();
 try{
  host.preflight();
  const audioReady=host.startAudio(); // create/resume in this gesture, before fetch
  if(!(await saves.acquireLock(sha))){
   blocked='game already open in another tab';host.stats.state='blocked';$('status').textContent='blocked: '+blocked;
   line('This game is already open in another tab. Close it there first, so one tab cannot overwrite the other\'s saves.',true);
   void audioReady.then(()=>host.audio?.close()).catch(()=>{});renderSaves();return;
  }
  $('reload').disabled=true;
  const [game,runtimeConfig]=await Promise.all([gameAssets(),fetchBytes('runtime.toml')]);
  await audioReady;romBytes=game[0];
  const env={};for(const item of (devParam('env')||'').split(',')){const i=item.indexOf('=');if(i>0)env[item.slice(0,i)]=item.slice(i+1);}
  const extra=(devParam('args')||'--window').split(' ').filter(Boolean);
  const saveArgs=['--save-path',`/saves/${sha}/battery.sav`,'--state-dir',`/saves/${sha}`];
  globalThis.Module={
   arguments:['--config','/data/runtime.toml','--rom','/data/game.gba','--bios','/data/gba_bios.bin','--rom-sha1',sha,...saveArgs,...extra],
   print:t=>line(t),printErr:t=>line(t,true),
   preRun:[()=>{Object.assign(Module.ENV,env);Module.FS.mkdir('/data');Module.FS.writeFile('/data/game.gba',game[0]);Module.FS.writeFile('/data/gba_bios.bin',game[1]);Module.FS.writeFile('/data/runtime.toml',runtimeConfig);saves.mount(Module,sha);}],
   onRuntimeInitialized:()=>{$('status').textContent='running';host.stats.state='running';},
   onAbort:fail,
   onExit:code=>{exitCode=code;line('[exit] code='+code);host.stats.exitCode=code;stopped();
    void settleSaves().then(()=>host.shutdown()).then(()=>{$('status').textContent='exited '+code;$('reload').disabled=false;renderSaves();globalThis.GbrExit?.(code);});},
  };
  running=true;saves.running=true;renderSaves();
  for(const id of ['pause','fullscreen','audio','stop','save','load'])$(id).disabled=false;
  const script=document.createElement('script');script.src='game.js';script.onerror=()=>fail('Failed to load game.js');document.body.appendChild(script);
 }catch(e){fail(e);}
};
$('pause').onclick=()=>{host.command('Pause');$('canvas').focus();};
$('save').onclick=()=>{host.command('Save',Number($('slot').value));$('canvas').focus();};
$('load').onclick=()=>{host.command('Load',Number($('slot').value));$('canvas').focus();};
$('fullscreen').onclick=()=>host.fullscreenRequest(document.fullscreenElement?0:1);
$('audio').onclick=()=>host.resumeAudio().catch(e=>line(e,true));
$('stop').onclick=()=>host.store('quit',1);
// Never reload away a change that exists only in memory without asking.
function reload(reason){
 if(saves.unsaved()&&!confirm('A save change is not stored in this browser yet and would be lost. Reload anyway? (Cancel, then use Export Save to keep a copy.)')){line(reason+' cancelled: unsaved save data',true);return false;}
 location.reload();return true;
}
$('reload').onclick=()=>reload('Restart');
$('volume').oninput=()=>host.store('volume',Number($('volume').value));
$('filter').onchange=()=>host.store('filter',Number($('filter').value));
$('export').onclick=()=>{
 for(const url of urls)URL.revokeObjectURL(url);urls=[];$('downloads').replaceChildren();
 offer('host-diagnostics.json',JSON.stringify({...host.snapshot(),saves:saves.snapshot()},null,2),'application/json');
 if(globalThis.Module?.FS){const fs=Module.FS;const walk=(dir,depth=0)=>{if(depth>4)return;for(const name of fs.readdir(dir)){if(name==='.'||name==='..')continue;const path=dir.replace(/\/$/,'')+'/'+name;try{const stat=fs.stat(path);if(fs.isDir(stat.mode)){if(!['/proc','/dev','/data'].includes(path))walk(path,depth+1);}else if(/(recomp_coverage_.*\.json|recomp_master_misses_.*\.toml\.frag|session-.*\.json|\.sav|\.sav\.bak|\.state[0-9]+|^state[0-9]+|\.ss[0-9]|\.csv|\.png)$/.test(name))offer(name,fs.readFile(path));}catch(e){line(e,true);}}};walk('/');}
};
// ---- bring your own ROM/BIOS ------------------------------------------------
function describeAsset(r){return `${r.name} (${Math.round(r.size/1024)} KiB)`;}
function renderAssets(){
 const locked=started||running;
 $('assetbar').hidden=false;
 if(embedded){
  $('romstatus').textContent='ROM + BIOS: embedded in this PRIVATE developer build (never publish it)';$('romstatus').className='err';
  for(const id of ['pickrom','pickbios','forgetassets','biosstatus'])$(id).hidden=true;
  if(!started)$('start').disabled=!sha;return;
 }
 $('romstatus').textContent='ROM: '+(picked.rom?describeAsset(picked.rom):sha?'not chosen (SHA-1 '+sha.slice(0,12)+'…)':'this bundle has no expected SHA-1');
 $('romstatus').className=picked.rom?'':'err';
 $('biosstatus').textContent='BIOS: '+(picked.bios?describeAsset(picked.bios):'not chosen');
 $('biosstatus').className=picked.bios?'':'err';
 $('pickrom').disabled=locked||!assets||!sha;$('pickbios').disabled=locked||!assets;$('forgetassets').disabled=locked||!(picked.rom||picked.bios);
 $('pickrom').textContent=picked.rom?'Replace ROM…':'Choose ROM…';$('pickbios').textContent=picked.bios?'Replace BIOS…':'Choose BIOS…';
 if(!started)$('start').disabled=!(sha&&picked.rom&&picked.bios);
}
async function pick(kind,file){
 if(!file||!assets)return;
 try{
  const result=await assets.accept(kind,file.name,new Uint8Array(await file.arrayBuffer()));
  if(!result.ok){line((kind==='rom'?'ROM':'BIOS')+' refused: '+result.error,true);return;}
  picked[kind]=result.record;
  if(result.warning)line(result.warning,true);
  line(`${kind==='rom'?'ROM':'BIOS'} accepted: ${result.record.name}${result.stored?' (kept in this browser)':' (this page only)'}`);
 }catch(e){line((kind==='rom'?'ROM':'BIOS')+' check failed: '+(e?.message||e),true);}
 finally{renderAssets();}
}
$('pickrom').onclick=()=>$('romfile').click();
$('pickbios').onclick=()=>$('biosfile').click();
$('romfile').onchange=()=>{const f=$('romfile').files[0];$('romfile').value='';void pick('rom',f);};
$('biosfile').onchange=()=>{const f=$('biosfile').files[0];$('biosfile').value='';void pick('bios',f);};
$('forgetassets').onclick=async()=>{
 if(!confirm('Forget the ROM and BIOS stored in this browser for this site? Saves are kept.'))return;
 await assets.forget();picked={rom:null,bios:null};line('ROM and BIOS removed from this browser');renderAssets();
};
const assetsLoaded=(async()=>{
 if(embedded||!assets||!sha){renderAssets();return;}
 try{picked=await assets.load();if(picked.rom||picked.bios)line('Using the ROM/BIOS kept in this browser');}
 catch(e){line('Could not read stored ROM/BIOS: '+(e?.message||e),true);}
 renderAssets();
})();
globalThis.GbrAssetsLoaded=assetsLoaded;
// Visitor-side save management. Changes need a stopped game; before the first
// Start they are queued in order and applied once /saves is loaded, before main().
$('exportsave').onclick=()=>{try{offer(GbrSaveStore.exportName(romBytes),saves.exportBattery());}catch(e){line('Export failed: '+e.message,true);}};
async function changeSaves(action,label){
 try{
  const result=await action();renderSaves();
  if(result==='persisted'){
   line(label+' stored in this browser');
   // After an exit the finished guest cannot reload the save: restart, but
   // only when nothing else is waiting to be stored.
   if(exitCode!==null&&!saves.unsaved()){line('Reloading to start with the stored save');location.reload();}
  }
  else if(result==='memory-only')line(label+' applied only in memory; browser storage is unavailable. This change will be lost on reload. Use Export Save to keep a copy of the current battery save.',true);
  else if(result==='queued')line(label+' queued: it is applied when you press Start, and the log will say whether it was stored');
 }
 catch(e){line(label+' failed: '+e.message,true);renderSaves();}
}
$('importsave').onclick=()=>$('importfile').click();
$('importfile').onchange=async()=>{const file=$('importfile').files[0];$('importfile').value='';if(file)await changeSaves(async()=>saves.importBattery(new Uint8Array(await file.arrayBuffer())),'Import');};
$('restoresave').onclick=()=>changeSaves(()=>saves.restoreBackup(),'Restore');
$('deletesaves').onclick=()=>{if(confirm('Delete the battery save, its backup and all save states for this game from this browser?'))void changeSaves(()=>saves.deleteAll(),'Delete');};
function renderSaves(){
 const s=saves.snapshot();let text,bad=false;
 if(!sha)text='not kept (ROM SHA-1 unknown)',bad=true;
 else if(blocked)text='not opened ('+blocked+')',bad=true;
 else if(s.memoryOnly)text=(s.state==='unavailable'?'unavailable, ':'')+'NOT stored: changes are in memory only and are lost on reload ('+(s.error||'browser storage unavailable')+'); use Export Save',bad=true;
 else if(s.state==='unavailable')text='unavailable, saves are lost on reload: '+s.error,bad=true;
 else if(s.state==='error')text='error: '+s.error+(s.unsaved?' (latest change NOT stored)':''),bad=true;
 else if(s.writeError)text='error: '+s.writeError,bad=true;
 else if(saves.busy())text='saving…';
 else if(s.unsaved)text='changed, not stored yet';
 else if(s.lastPersisted)text=`saved ${Math.max(0,Math.round((Date.now()-s.lastPersisted)/1000))}s ago`+(s.persistent===false?' (best-effort storage: export a copy)':'');
 else if(s.pending.length)text=s.pending.join(', ')+' queued for Start';
 else text=saves.mounted?'stored in this browser':'kept in this browser';
 $('savestatus').textContent='Browser save: '+text;$('savestatus').className=bad?'err':'';
 const locked=!sha||!!blocked||running||saves.busy();
 $('exportsave').disabled=!saves.fs;$('importsave').disabled=locked;$('restoresave').disabled=locked;$('deletesaves').disabled=locked;
}
// A touch long press / policy request for settings: reveal the page controls.
addEventListener('gbrsettings',()=>{$('bar').scrollIntoView?.({block:'start'});$('pause').disabled?$('start').focus?.():$('pause').focus?.();});
document.addEventListener('visibilitychange',()=>{if(document.hidden)saves.persist();}); // what MEMFS already holds; the runtime flushes the rest on pause
// Warn while anything the runtime (or the visitor) wrote is not yet stored in
// IndexedDB, including memory-only changes, not only while a sync is running.
addEventListener('beforeunload',e=>{if(saves.unsaved()){saves.persist();e.preventDefault();e.returnValue='';}});
setInterval(()=>{const s=host.snapshot();$('metrics').textContent=`${s.state} · published ${s.published||0} · consumed ${s.consumed} · GL uploads ${s.uploaded} · replaced ${s.replaced||0} · audio ${s.audio} · queue overflow ${s.audioOverflow||0}`;if(exitCode===null&&!blocked&&s.state!=='idle')$('status').textContent=s.fatal?'failed: '+s.fatal:s.state;renderSaves();},250);
renderSaves();renderAssets();
if(dev)line('Developer bundle on localhost: ?args= / ?env= are honoured');
if(q.get('autostart')==='1')addEventListener('load',()=>{void assetsLoaded.then(()=>$('start').click());});
})();
