"""Capture fresh game evidence; no historical directory can satisfy this run."""
import argparse,json,time,urllib.parse
from browser import Browser
p=argparse.ArgumentParser();p.add_argument('frames',type=int);p.add_argument('output');p.add_argument('--url',default='http://127.0.0.1:18083/');p.add_argument('--headless',action='store_true');p.add_argument('--strict',action='store_true');a=p.parse_args()
b=Browser(a.output,True,autoplay=False)
try:
 env='GBARECOMP_SELFHEAL_RECOMPILE=0'+(',GBARECOMP_STRICT_STATIC=1' if a.strict else '')
 q=urllib.parse.urlencode({'args':f'{"--no-window" if a.headless else "--window"} --frames {a.frames} --dump-png /data/final.png','env':env})
 b.call('Page.navigate',{'url':a.url+'?'+q});time.sleep(.5)
 for t in ['mousePressed','mouseReleased']:b.call('Input.dispatchMouseEvent',{'type':t,'x':45,'y':32,'button':'left','clickCount':1})
 deadline=time.monotonic()+max(90,a.frames/30+30);samples=[]
 while time.monotonic()<deadline:
  time.sleep(1);s=b.eval('GbrHost.snapshot()');samples.append(s)
  if (s.get('exitCode') is not None and s.get('state')=='exited') or s.get('fatal'):break
 b.screenshot('canvas-final.png')
 console=b.eval('document.getElementById("log").innerText');(b.output/'console.txt').write_text(console)
 (b.output/'samples.json').write_text(json.dumps(samples,indent=2))
 files=b.eval('(()=>{const out={};for(const d of ["/","/data"]){for(const n of Module.FS.readdir(d)){if(/miss|coverage|frag/.test(n)){try{out[d.replace(/\\/$/,"")+"/"+n]=Module.FS.readFile(d+"/"+n,{encoding:"utf8"});}catch{}}}}return out;})()')
 (b.output/'coverage-files.json').write_text(json.dumps(files,indent=2))
 if not a.strict:
  data=b.eval('Array.from(Module.FS.readFile("/data/final.png"))');(b.output/'guest-final.png').write_bytes(bytes(data))
  assert samples[-1].get('exitCode')==0,samples[-1]
  if not a.headless:
   final=b.eval('GbrHost.snapshot()');assert final['published']==a.frames and final['lastSeq']==a.frames and final['lastUploadedSeq']==a.frames,final
  assert files,'coverage artifacts absent'
 else:assert samples[-1].get('fatal') and 'STRICT_STATIC dispatch miss' in console,'strict static did not report its coverage failure'
 print(json.dumps(samples[-1]))
finally:b.close()
