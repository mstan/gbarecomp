import argparse, base64, json, os, pathlib, subprocess, tempfile, time, urllib.parse, urllib.request
import websocket
class Browser:
 # profile_dir: reuse a Chrome profile across instances (IndexedDB survives
 # close/kill); it is never deleted. Default: a temporary profile.
 def __init__(self, output, gpu=False, autoplay=True, profile_dir=None):
  self.output=pathlib.Path(output); self.output.mkdir(parents=True,exist_ok=True)
  if profile_dir: pathlib.Path(profile_dir).mkdir(parents=True,exist_ok=True); self.profile=None; profile=str(profile_dir)
  else: self.profile=tempfile.TemporaryDirectory(prefix='gbr-canvas-chrome-'); profile=self.profile.name
  self.log=open(self.output/'chrome.log','a')
  flags=[os.environ.get('CHROME', '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'),'--headless=new','--user-data-dir='+profile,'--no-first-run','--no-default-browser-check','--remote-debugging-port=0','--remote-allow-origins=*','--window-size=800,700']
  if autoplay: flags+=['--autoplay-policy=no-user-gesture-required']
  if not gpu: flags+=['--use-angle=swiftshader','--enable-unsafe-swiftshader']
  portfile=pathlib.Path(profile)/'DevToolsActivePort'
  portfile.unlink(missing_ok=True)  # stale after a killed instance
  self.proc=subprocess.Popen(flags+['about:blank'],stdout=self.log,stderr=self.log)
  for i in range(100):
   if portfile.exists() and len(portfile.read_text().splitlines())>=2: break
   time.sleep(.1)
  port=int(portfile.read_text().splitlines()[0]); self.port=port
  tabs=json.load(urllib.request.urlopen(f'http://127.0.0.1:{port}/json'))
  tab=next(t for t in tabs if t['type']=='page')
  self.ws=websocket.create_connection(tab['webSocketDebuggerUrl'],timeout=20); self.seq=0; self.events=[]
  self.call('Runtime.enable');self.call('Page.enable')
 def call(self, method, params=None):
  self.seq+=1; mid=self.seq; self.ws.send(json.dumps({'id':mid,'method':method,'params':params or {}}))
  while True:
   x=json.loads(self.ws.recv())
   if x.get('id')==mid:
    if 'error' in x: raise RuntimeError(x)
    return x.get('result',{})
   self.events.append(x)
 def eval(self, expr):
  r=self.call('Runtime.evaluate',{'expression':expr,'returnByValue':True,'awaitPromise':True})
  if 'exceptionDetails' in r: raise RuntimeError(r)
  return r.get('result',{}).get('value')
 def screenshot(self,name):
  r=self.call('Page.captureScreenshot',{'format':'png','captureBeyondViewport':False})
  (self.output/name).write_bytes(base64.b64decode(r['data']))
 def new_tab(self, url):
  """Second page in the same browser/profile, driven like the first."""
  req=urllib.request.Request(f'http://127.0.0.1:{self.port}/json/new?'+urllib.parse.quote(url,safe=':/?&=%'),method='PUT')
  t=json.load(urllib.request.urlopen(req))
  tab=Browser.__new__(Browser); tab.output=self.output; tab.seq=0; tab.events=[]
  tab.ws=websocket.create_connection(t['webSocketDebuggerUrl'],timeout=20)
  tab.call('Runtime.enable');tab.call('Page.enable')
  return tab
 def kill(self):
  """Abrupt browser death (SIGKILL): no unload handlers, no flushes."""
  try:self.ws.close()
  except Exception:pass
  self.proc.kill();self.proc.wait();self.log.close()
  if self.profile:self.profile.cleanup()
 def close(self):
  (self.output/'events.json').write_text(json.dumps(self.events,indent=2))
  self.ws.close(); self.proc.terminate()
  try:self.proc.wait(timeout=10)
  except subprocess.TimeoutExpired:self.proc.kill();self.proc.wait()
  self.log.close()
  if self.profile:self.profile.cleanup()
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('url');p.add_argument('out');p.add_argument('--gpu',action='store_true');a=p.parse_args()
 b=Browser(a.out,a.gpu)
 try:
  b.call('Page.navigate',{'url':a.url}); samples=[]
  for i in range(10):
   time.sleep(.8)
   samples.append(b.eval('({init:window.init,probe:window.probe,errors:window.errors})'))
   b.screenshot(f'{i:02}.png')
  (b.output/'samples.json').write_text(json.dumps(samples,indent=2));print(json.dumps(samples))
 finally:b.close()
