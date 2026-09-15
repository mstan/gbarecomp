'use strict';
(()=>{
const $=id=>document.getElementById(id),q=new URLSearchParams(location.search);
let started=false,running=false,exitCode=null,blocked=null,urls=[],romBytes=null;
function line(text,error=false){text=String(text).slice(0,2000);(error?console.error:console.log)(text);const el=document.createElement('div');el.textContent=text;if(error)el.className='err';$('log').appendChild(el);while($('log').children.length>4000)$('log').firstChild.remove();}
const host=globalThis.GbrHost=new GbrWebHost($('canvas'),t=>line(t));
// Battery saves and save states stay in this browser (IndexedDB), keyed by the
// verified ROM SHA-1. See packaging/web/save_store.js.
const saves=globalThis.GbrSaves=new GbrSaveStore((t,error)=>line(t,error),()=>renderSaves());
const sha=(q.get('sha1')||globalThis.GBARECOMP_ROM_SHA1||'').toLowerCase();
// Files in MEMFS are complete (.tmp + rename), so syncing after exit/abort is safe.
// Never rely on the runtime's last async notice having arrived first.
async function settleSaves(){saves.persist();try{await saves.flush();}catch(e){line('Save was not stored in this browser: '+e.message,true);}}
function stopped(){running=false;saves.running=false;renderSaves();}
function fail(e){host.fail(e);$('status').textContent='failed: '+e;line(e,true);stopped();void settleSaves().finally(()=>{$('reload').disabled=false;renderSaves();});void host.shutdown();}
function offer(name,data,type='application/octet-stream'){const url=URL.createObjectURL(new Blob([data],{type}));urls.push(url);const a=document.createElement('a');a.href=url;a.download=name;a.textContent=name;a.style.marginRight='12px';$('downloads').appendChild(a);}
addEventListener('error',e=>line(e.message,true));addEventListener('unhandledrejection',e=>line(e.reason,true));
$('start').onclick=async()=>{
 if(started)return;started=true;$('start').disabled=true;$('status').textContent='loading';host.stats.state='loading';
 try{
  host.preflight();
  const audioReady=host.startAudio(); // create/resume in this gesture, before fetch
  if(sha){
   if(!(await saves.acquireLock(sha))){
    blocked='game already open in another tab';host.stats.state='blocked';$('status').textContent='blocked: '+blocked;
    line('This game is already open in another tab. Close it there first, so one tab cannot overwrite the other\'s saves.',true);
    void audioReady.then(()=>host.audio?.close()).catch(()=>{});renderSaves();return;
   }
  }else line('ROM SHA-1 unknown (no rom_sha1.js or ?sha1=): saves are NOT kept in this browser',true);
  $('reload').disabled=true;
  const rom=q.get('rom')||'game.gba',bios=q.get('bios')||'gba_bios.bin';
  const assets=await Promise.all([rom,bios,'runtime.toml'].map(async name=>{const r=await fetch(name);if(!r.ok)throw Error(`${name}: HTTP ${r.status}`);return new Uint8Array(await r.arrayBuffer());}));
  await audioReady;romBytes=assets[0];
  const env={};for(const item of (q.get('env')||'').split(',')){const i=item.indexOf('=');if(i>0)env[item.slice(0,i)]=item.slice(i+1);}
  const extra=(q.get('args')||'--window').split(' ').filter(Boolean);
  const saveArgs=sha?['--save-path',`/saves/${sha}/battery.sav`,'--state-dir',`/saves/${sha}`]:[];
  globalThis.Module={
   arguments:['--config','/data/runtime.toml','--rom','/data/game.gba','--bios','/data/gba_bios.bin',...(sha?['--rom-sha1',sha]:[]),...saveArgs,...extra],
   print:t=>line(t),printErr:t=>line(t,true),
   preRun:[()=>{Object.assign(Module.ENV,env);Module.FS.mkdir('/data');Module.FS.writeFile('/data/game.gba',assets[0]);Module.FS.writeFile('/data/gba_bios.bin',assets[1]);Module.FS.writeFile('/data/runtime.toml',assets[2]);if(sha)saves.mount(Module,sha);}],
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
$('reload').onclick=()=>location.reload();
$('volume').oninput=()=>host.store('volume',Number($('volume').value));
$('filter').onchange=()=>host.store('filter',Number($('filter').value));
$('export').onclick=()=>{
 for(const url of urls)URL.revokeObjectURL(url);urls=[];$('downloads').replaceChildren();
 offer('host-diagnostics.json',JSON.stringify({...host.snapshot(),saves:saves.snapshot()},null,2),'application/json');
 if(globalThis.Module?.FS){const fs=Module.FS;const walk=(dir,depth=0)=>{if(depth>4)return;for(const name of fs.readdir(dir)){if(name==='.'||name==='..')continue;const path=dir.replace(/\/$/,'')+'/'+name;try{const stat=fs.stat(path);if(fs.isDir(stat.mode)){if(!['/proc','/dev'].includes(path))walk(path,depth+1);}else if(/(recomp_coverage_.*\.json|recomp_master_misses_.*\.toml\.frag|\.sav|\.sav\.bak|\.state[0-9]+|^state[0-9]+|\.ss[0-9]|\.csv|\.png)$/.test(name))offer(name,fs.readFile(path));}catch(e){line(e,true);}}};walk('/');}
};
// Visitor-side save management. Changes need a stopped game; before the first
// Start they are queued and applied once /saves is loaded, before main().
$('exportsave').onclick=()=>{try{offer(GbrSaveStore.exportName(romBytes),saves.exportBattery());}catch(e){line('Export failed: '+e.message,true);}};
async function changeSaves(action,label){
 try{const result=await action();renderSaves();if(result==='applied'&&exitCode!==null){line(label+' stored; reloading');location.reload();}}
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
 else if(s.state==='unavailable')text='unavailable, saves are lost on reload: '+s.error,bad=true;
 else if(s.state==='error')text='error: '+s.error,bad=true;
 else if(s.writeError)text='error: '+s.writeError,bad=true;
 else if(saves.busy())text='saving…';
 else if(s.lastPersisted)text=`saved ${Math.max(0,Math.round((Date.now()-s.lastPersisted)/1000))}s ago`+(s.persistent===false?' (best-effort storage: export a copy)':'');
 else if(s.pending.length)text=s.pending[0]+' queued for Start';
 else text=saves.mounted?'stored in this browser':'kept in this browser';
 $('savestatus').textContent='Browser save: '+text;$('savestatus').className=bad?'err':'';
 const locked=!sha||!!blocked||running||saves.busy();
 $('exportsave').disabled=!saves.fs;$('importsave').disabled=locked;$('restoresave').disabled=locked;$('deletesaves').disabled=locked;
}
document.addEventListener('visibilitychange',()=>{if(document.hidden)saves.persist();}); // what MEMFS already holds; the runtime flushes the rest on pause
addEventListener('beforeunload',e=>{if(saves.busy()){e.preventDefault();e.returnValue='';}});
setInterval(()=>{const s=host.snapshot();$('metrics').textContent=`${s.state} · published ${s.published||0} · consumed ${s.consumed} · GL uploads ${s.uploaded} · replaced ${s.replaced||0} · audio ${s.audio} · queue overflow ${s.audioOverflow||0}`;if(exitCode===null&&!blocked&&s.state!=='idle')$('status').textContent=s.fatal?'failed: '+s.fatal:s.state;renderSaves();},250);
renderSaves();
if(q.get('autostart')==='1')addEventListener('load',()=>$('start').click());
})();
