"""Browser save persistence acceptance (packaging/web/README.md, "Browser
acceptance tests").

Build a developer bundle that carries its own ROM/BIOS (the test boots through
?args=/?env=, which only --dev bundles honour on localhost), serve it with
packaging/web/serve.py, then:

  bash packaging/web/build_web.sh --dev --embed-private-rom <project> <bios-gen> <rom> <bios> [config]
  python3 packaging/web/serve.py <project>/web-PRIVATE 18083
  python3 tests/web/test_save.py --url http://127.0.0.1:18083/ /tmp/gbr-save

One Chrome profile is reused for the whole run, so IndexedDB survives page
reloads and a killed browser. Every game session's console and coverage files
are written to the output directory (dispatch-miss rule). Not covered here and
reported as NOT RUN: T2 (in-game save from a native input recording), T8 (native
A/B), T9 (mGBA .sav compatibility).

Evidence of the battery load: `--window` implies --quiet, so `save_loaded` is
only printed by headless boots (`--no-window --frames N`), which go through the
same page flow (lock, IDBFS mount, import, sync). Save-state, pause and tab
tests need the host window. wasm stdout is fully buffered; `savestate_*` lines
are flushed by the runtime, the rest reaches the page at exit.
"""
import argparse, hashlib, json, pathlib, re, shutil, time, urllib.parse
from browser import Browser

p=argparse.ArgumentParser()
p.add_argument('output')
p.add_argument('--url',default='http://127.0.0.1:18083/')
p.add_argument('--gpu',action='store_true')
a=p.parse_args()
out=pathlib.Path(a.output); out.mkdir(parents=True,exist_ok=True)
profile=out/'chrome-profile'; shutil.rmtree(profile,ignore_errors=True)
ENV='GBARECOMP_SELFHEAL_RECOMPILE=0'
WINDOW=a.url+'?'+urllib.parse.urlencode({'args':'--window','env':ENV})
HEADLESS=a.url+'?'+urllib.parse.urlencode({'args':'--no-window --frames 120','env':ENV})
results={'T2':'NOT RUN','T8':'NOT RUN (native)','T9':'NOT RUN'}
sessions=0

def wait(tab,expr,timeout=60,what=None):
 end=time.monotonic()+timeout;last=None
 while time.monotonic()<end:
  try:last=tab.eval(expr)
  except RuntimeError as e:last=str(e)[:300]
  if last:return last
  time.sleep(.25)
 raise AssertionError(f'timeout waiting for {what or expr}; last={last!r}')
def console(tab):return tab.eval('document.getElementById("log").innerText')
def open_page(tab,url=WINDOW,before=''):
 tab.call('Page.navigate',{'url':url})
 wait(tab,'document.readyState==="complete"&&!!globalThis.GbrSaves&&!!document.getElementById("start")',30,'page load')
 if before:tab.eval(before)
def click(tab,id):
 x,y=tab.eval(f'(()=>{{const r=document.getElementById("{id}").getBoundingClientRect();return [r.x+r.width/2,r.y+r.height/2];}})()')
 for t in ['mousePressed','mouseReleased']:tab.call('Input.dispatchMouseEvent',{'type':t,'x':x,'y':y,'button':'left','clickCount':1})
def exited(tab,timeout=180):
 wait(tab,'document.getElementById("status").textContent.startsWith("exited")||!!GbrHost.snapshot().fatal',timeout,'exit + save flush')
 assert not tab.eval('GbrHost.snapshot().fatal'),tab.eval('GbrHost.snapshot().fatal')
def start(tab,frames=60):
 click(tab,'start')
 wait(tab,f'(GbrHost.snapshot().published||0)>={frames}||!!GbrHost.snapshot().fatal',180,'game frames')
 assert not tab.eval('GbrHost.snapshot().fatal'),tab.eval('GbrHost.snapshot().fatal')
def idle(tab):wait(tab,'(s=>s.state==="idle"&&!s.pending.length)(GbrSaves.snapshot())',30,'saves idle')
def record(tab,name):
 global sessions;sessions+=1
 (out/f'{sessions:02}-{name}-console.txt').write_text(console(tab))
 files=tab.eval('(()=>{const o={};if(!globalThis.Module?.FS)return o;for(const n of Module.FS.readdir("/"))if(/miss|coverage|frag/.test(n)){try{o[n]=Module.FS.readFile("/"+n,{encoding:"utf8"});}catch(e){}}return o;})()')
 (out/f'{sessions:02}-{name}-coverage.json').write_text(json.dumps(files,indent=2))
def stop(tab,name):
 tab.eval('document.getElementById("stop").click()')
 exited(tab,90);record(tab,name)
 return console(tab)
def boot(tab,name,before='',queue_import=None):
 """Headless boot to exit: returns the console, which carries save_loaded."""
 open_page(tab,HEADLESS,before)
 if queue_import:assert tab.eval(f'GbrSaves.importBattery({queue_import})')=='queued'
 click(tab,'start');exited(tab);record(tab,name)
 return console(tab)
def command(tab,kind,slot):assert tab.eval(f'GbrHost.command("{kind}",{slot})'),f'{kind} {slot} not queued'
def logged(tab,pattern,timeout=30):return wait(tab,f'new RegExp({json.dumps(pattern)}).test(document.getElementById("log").innerText)',timeout,pattern)
def exported(tab):return hashlib.sha256(bytes(tab.eval('Array.from(GbrSaves.exportBattery())'))).hexdigest()
def check(name,fn):
 print(f'== {name}',flush=True)
 try:fn()
 except BaseException as e:
  results[name]=f'FAIL: {type(e).__name__}: {e}'[:500]
  try:record(b,name+'-FAILED')
  except Exception:pass
  raise
 results[name]='PASS';print(f'   {name} PASS',flush=True)

b=Browser(out,a.gpu,autoplay=True,profile_dir=profile)
try:
 open_page(b)
 sha=b.eval('(globalThis.GBARECOMP_ROM_SHA1||"").toLowerCase()');assert re.fullmatch('[0-9a-f]{40}',sha),'bundle has no build_info.js'
 assert b.eval('!!(GBARECOMP_BUILD.dev&&GBARECOMP_BUILD.embedded)'),'needs a bundle built with --dev --embed-private-rom'
 d=f'/saves/{sha}'
 # Chip size from the cartridge's library signature, matching the runtime defaults.
 kind=b.eval('(async()=>{const r=new Uint8Array(await (await fetch(GBARECOMP_BUILD.embedded.rom)).arrayBuffer());const m=new TextDecoder("latin1").decode(r).match(/(EEPROM|SRAM_F|SRAM|FLASH1M|FLASH512|FLASH)_V\\d\\d\\d/);return m?m[1]:null;})()')
 size={'EEPROM':8192,'SRAM':32768,'SRAM_F':32768,'FLASH':65536,'FLASH512':65536,'FLASH1M':131072}.get(kind)
 assert size,f'no battery save chip detected ({kind})'
 payload=bytes((i*131+7)&255 for i in range(size));digest=hashlib.sha256(payload).hexdigest()
 js_payload=f'new Uint8Array({json.dumps(list(payload))})'
 loaded=f'save_loaded path="{d}/battery.sav" size={size}/{size}'
 print(f'rom sha1={sha} chip={kind} size={size}')

 def t7_unavailable():
  # IndexedDB refuses to open: status says unavailable, the game still runs, a
  # queued import still reaches the runtime, export works.
  log=boot(b,'t7-unavailable','indexedDB.open=()=>{throw new DOMException("blocked by test","SecurityError");}',js_payload)
  assert 'Browser save storage unavailable: SecurityError' in log
  assert b.eval('GbrSaves.snapshot().state')=='unavailable'
  assert 'unavailable' in b.eval('document.getElementById("savestatus").textContent')
  assert loaded in log,'runtime did not load the imported save'
  assert exported(b)==digest
 check('T7a',t7_unavailable)

 def t1_import():
  warned=len(b.dialogs)
  first=boot(b,'t1-import',queue_import=js_payload)
  # T7a left a battery save that existed only in memory: leaving must warn.
  assert any(d.get('type')=='beforeunload' for d in b.dialogs[warned:]),'no beforeunload warning for memory-only saves'
  assert loaded in first
  assert b.eval('GbrSaves.snapshot().lastPersisted')>0 and b.eval('GbrSaves.snapshot().state')=='idle'
  # Page reload: only IndexedDB can bring the file back.
  second=boot(b,'t1-reload')
  assert re.search(r'Browser saves loaded \([^)]*battery\.sav',second),'battery.sav not restored from IndexedDB'
  assert f'save_loaded path="{d}/battery.sav" size=' in second
  wrote='save_flushed' in first or 'save_flushed' in second
  assert exported(b)==digest or wrote,'exported save differs and the game never flushed one'
  if exported(b)!=digest:print('   note: the game rewrote its save; export equals the flushed file, not the import')
 check('T1',t1_import)

 open_page(b);start(b)

 def t6_second_tab():
  tab=b.new_tab(WINDOW)
  try:
   wait(tab,'document.readyState==="complete"&&!!globalThis.GbrSaves',30,'second tab load')
   tab.eval('document.getElementById("start").click()')
   wait(tab,'document.getElementById("status").textContent.includes("another tab")',20,'second tab blocked')
   assert tab.eval('typeof Module==="undefined"'),'second tab started the runtime'
  finally:
   tab.call('Page.close')
 check('T6',t6_second_tab)

 def t4_hidden():
  s0=b.eval('GbrSaves.snapshot().syncs')
  b.eval('Object.defineProperty(document,"hidden",{configurable:true,get:()=>true});Object.defineProperty(document,"visibilityState",{configurable:true,get:()=>"hidden"});document.dispatchEvent(new Event("visibilitychange"))')
  wait(b,f'GbrSaves.snapshot().syncs>{s0}',10,'persist on hide');idle(b)
  wait(b,'GbrHost.snapshot().paused===1',10,'runtime auto-pause')
  b.eval('delete document.hidden;delete document.visibilityState;document.dispatchEvent(new Event("visibilitychange"))')
  wait(b,'GbrHost.snapshot().paused===0',10,'resume')
 check('T4',t4_hidden)

 def t7_quota():
  r0=b.eval('GbrSaves.snapshot().retries')
  b.eval('globalThis.__syncfs=Module.FS.syncfs;Module.FS.syncfs=(p,cb)=>setTimeout(()=>cb(new DOMException("quota by test","QuotaExceededError")),0);GbrSaves.persist()')
  wait(b,'GbrSaves.snapshot().state==="error"',10,'quota error')
  assert b.eval('GbrSaves.snapshot().retries')==r0,'an IndexedDB error must not be retried'
  assert 'QuotaExceededError' in b.eval('document.getElementById("savestatus").textContent')
  b.eval('Module.FS.syncfs=globalThis.__syncfs;GbrSaves.persist()');idle(b)
 check('T7b',t7_quota)

 def t3_states():
  command(b,'Save',1);logged(b,f'savestate_saved slot=1 path="{d}/state1"');idle(b)
  assert b.eval('GbrSaves.snapshot().writes')>=1,'runtime notice for the save state never arrived'
 check('T3-save',t3_states)

 def t12_race():
  # A sync whose IndexedDB phase is delayed while a listed temp file vanishes
  # and a real save state is written concurrently.
  r0=b.eval(f'''(()=>{{const I=Module.FS.filesystems.IDBFS;globalThis.__remote=I.getRemoteSet;
    I.getRemoteSet=(m,cb)=>setTimeout(()=>globalThis.__remote(m,cb),200);
    Module.FS.writeFile("{d}/race.tmp",new Uint8Array(64));const r=GbrSaves.snapshot().retries;GbrSaves.persist();
    setTimeout(()=>Module.FS.unlink("{d}/race.tmp"),50);GbrHost.command("Save",3);return r;}})()''')
  logged(b,f'savestate_saved slot=3 path="{d}/state3"');idle(b)
  s=b.eval('GbrSaves.snapshot()');assert s['retries']>r0 and s['state']=='idle' and not s['error'],s
  b.eval('Module.FS.filesystems.IDBFS.getRemoteSet=globalThis.__remote;GbrSaves.persist()');idle(b)
  state_bytes=b.eval('Module.FS.stat('+json.dumps(d+'/state1')+').size');sync_ms=b.eval('GbrSaves.snapshot().lastSyncMs')
  print(f'   state1 size={state_bytes} bytes; last sync {sync_ms:.1f} ms; retries {s["retries"]}')
  stop(b,'t3-t12-save')
 check('T12',t12_race)

 def t3_reload():
  open_page(b);start(b)
  assert not [n for n in b.eval(f'Module.FS.readdir("{d}")') if n.endswith('.tmp')],'temp files survived the reload'
  command(b,'Load',1);logged(b,f'savestate_loaded slot=1 path="{d}/state1"')
  command(b,'Load',3);logged(b,f'savestate_loaded slot=3 path="{d}/state3"')
 check('T3',t3_reload)

 def t11_stop_after_write():
  # Stop in the same breath as a write: onExit must sync without waiting for the
  # runtime's async notice.
  b.eval('GbrHost.command("Save",2);document.getElementById("stop").click()')
  exited(b,90);record(b,'t11-stop')
  assert f'savestate_saved slot=2 path="{d}/state2"' in console(b)
  idle(b)
  open_page(b);start(b)
  command(b,'Load',2);logged(b,f'savestate_loaded slot=2 path="{d}/state2"')
 check('T11',t11_stop_after_write)

 def t5_kill():
  global b
  record(b,'t5-before-kill')
  b.kill()  # while the game runs; everything above was already persisted
  b=Browser(out,a.gpu,autoplay=True,profile_dir=profile)
  assert f'save_loaded path="{d}/battery.sav" size=' in boot(b,'t5-after-kill-battery')
  open_page(b);start(b)
  command(b,'Load',1);logged(b,f'savestate_loaded slot=1 path="{d}/state1"')
  stop(b,'t5-after-kill-state')
 check('T5',t5_kill)
finally:
 (out/'results.json').write_text(json.dumps(results,indent=2))
 print(json.dumps(results,indent=2))
 try:b.close()
 except Exception:pass
expected={'T1','T3-save','T3','T4','T5','T6','T7a','T7b','T11','T12'}
failed=[k for k,v in results.items() if v.startswith('FAIL')]
raise SystemExit(1 if failed or expected-{k for k,v in results.items() if v=='PASS'} else 0)
