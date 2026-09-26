/* Page-owned WebGL and control service. No guest execution or worker GL. */
(function(root) {
'use strict';
const DEFAULT_KEYS=['KeyX','KeyZ','ShiftRight','Enter','ArrowRight','ArrowLeft','ArrowUp','ArrowDown','KeyV','KeyC'];
const COMMAND={Pause:1,Save:2,Load:3,WindowBigger:4,WindowSmaller:5,VolumeUp:6,VolumeDown:7,DisplayPerf:8,Rewind:9,SolarBrighter:10,SolarDimmer:11,SolarLive:12};
const DETACH={Pending:0,SafeToFree:1,UnsafeRetain:2};
const ABI_VERSION=2;
// host_web_shared.h TouchKind / TouchMode.
const TOUCH={Down:0,Move:1,Up:2,Cancel:3,TwoFingerTap:16,ThreeFingerTap:17,Back:18,FromMouse:0x100};
const TOUCH_MODE={Policy:1,Emulate:2};
// host_web_shared.h OverlayKind.
const OVERLAY={Line:1,FillRect:2,StrokeRect:3,FillCircle:4,StrokeCircle:5,Arc:6};
// Physical modifier keys. A modifier that is itself bound to a game button
// (Select = right Shift by default) never turns a key into a hotkey chord.
const MODIFIER_CODES={ShiftLeft:'Shift',ShiftRight:'Shift',ControlLeft:'Ctrl',ControlRight:'Ctrl',AltLeft:'Alt',AltRight:'Alt'};
const MODIFIERS=['Ctrl','Alt','Shift'];
const EVENT_FLAG={Ctrl:'ctrlKey',Alt:'altKey',Shift:'shiftKey'};
// Save-state slots, same semantics as the native backend: F1-F9 load,
// Shift+F1-F9 save. Matched with the same exact-modifier rule as [KeyMap].
const SLOT_BINDINGS=[];
for(let n=1;n<=9;++n)SLOT_BINDINGS.push({command:'Load',slot:n,binding:'F'+n},{command:'Save',slot:n,binding:'Shift+F'+n});
// Virtual pad buttons (KEYINPUT bits).
const PAD_BUTTONS=[['a','A',1],['b','B',2],['select','Select',4],['start','Start',8],['r','R',256],['l','L',512]];
const DPAD={right:16,left:32,up:64,down:128};
const gcd=(a,b)=>b?gcd(b,a%b):a;
function layout(dw,dh,w,h){const g=gcd(w,h),u=Math.floor(Math.min(dw/(w/g),dh/(h/g)));return {x:Math.floor((dw-u*w/g)/2),y:Math.floor((dh-u*h/g)/2),w:u*w/g,h:u*h/g,integer:u%g===0};}
// "Ctrl+Shift+KeyX" -> {code:'KeyX',Ctrl:true,Alt:false,Shift:true,count:2}
function parseBinding(text){
  if(!text)return null;
  const parts=String(text).split('+'),code=parts.pop();if(!code)return null;
  const b={code,count:0};for(const m of MODIFIERS){b[m]=parts.includes(m);if(b[m])++b.count;}
  return b;
}
const exactMods=(b,mods)=>MODIFIERS.every(m=>b[m]===!!mods[m]);
const requiredMods=(b,mods)=>MODIFIERS.every(m=>!b[m]||!!mods[m]);
// 8-way d-pad from a point relative to the pad centre (dead zone in the middle).
function dpadMask(dx,dy,radius){
  const d=Math.hypot(dx,dy);if(d<radius*0.18)return 0;
  const a=Math.atan2(dy,dx),s=Math.PI/8;let m=0;
  if(Math.abs(a)<3*s)m|=DPAD.right;
  if(Math.abs(a)>5*s)m|=DPAD.left;
  if(a<-s&&a>-7*s)m|=DPAD.up;
  if(a>s&&a<7*s)m|=DPAD.down;
  return m;
}
const css=(rgba)=>`rgba(${rgba&255},${(rgba>>>8)&255},${(rgba>>>16)&255},${((rgba>>>24)&255)/255})`;
// Replay a host overlay display list (drawable pixels) on a 2D context.
function drawOverlay(ctx,u32,f32,count){
  const rounded=(x,y,w,h,r)=>{ctx.beginPath();if(r>0&&ctx.roundRect)ctx.roundRect(x,y,w,h,Math.min(r,w/2,h/2));else ctx.rect(x,y,w,h);};
  for(let i=0;i<count;++i){
    const o=i*8,kind=u32[o],color=css(u32[o+1]),v=f32.subarray(o+2,o+8);
    ctx.fillStyle=color;ctx.strokeStyle=color;ctx.lineCap='round';ctx.lineJoin='round';
    switch(kind){
      case OVERLAY.Line:ctx.lineWidth=Math.max(0.5,v[4]);ctx.beginPath();ctx.moveTo(v[0],v[1]);ctx.lineTo(v[2],v[3]);ctx.stroke();break;
      case OVERLAY.FillRect:rounded(v[0],v[1],v[2],v[3],v[4]);ctx.fill();break;
      case OVERLAY.StrokeRect:ctx.lineWidth=Math.max(0.5,v[5]);rounded(v[0],v[1],v[2],v[3],v[4]);ctx.stroke();break;
      case OVERLAY.FillCircle:ctx.beginPath();ctx.arc(v[0],v[1],Math.max(0,v[2]),0,2*Math.PI);ctx.fill();break;
      case OVERLAY.StrokeCircle:ctx.lineWidth=Math.max(0.5,v[3]);ctx.beginPath();ctx.arc(v[0],v[1],Math.max(0,v[2]),0,2*Math.PI);ctx.stroke();break;
      case OVERLAY.Arc:{ctx.lineWidth=Math.max(0.5,v[3]);const top=-Math.PI/2;ctx.beginPath();ctx.arc(v[0],v[1],Math.max(0,v[2]),top+v[4]*2*Math.PI,top+v[5]*2*Math.PI);ctx.stroke();break;}
      default:break;  // unknown primitive from a newer runtime: skip, never throw
    }
  }
}
class Host {
  constructor(canvas,report=()=>{}) {
    this.canvas=canvas;this.report=report;this.keys=DEFAULT_KEYS.slice();
    this.hotkeys={Pause:'Shift+KeyP',Turbo:'Tab',Fullscreen:'Alt+Enter',WindowBigger:'',WindowSmaller:'',VolumeUp:'',VolumeDown:'',DisplayPerf:'KeyF'};
    this.stats={state:'idle',consumed:0,uploaded:0,lastSeq:0,contextLosses:0,contextRestores:0,viewGenerations:0,copyMs:0,uploadMs:0,audio:'unavailable',fatal:null,overlayDropped:0,touchOverflow:0};
    // keyboard: physical keys held while the canvas has focus. consumed: keys
    // whose press was taken by a hotkey chord (Alt+Enter never presses Start).
    this.front=0;this.keyboard=new Set();this.consumed=new Set();this.eventMods={};this.gamepad=0;this.touch=0;this.listeners=[];this.inputListeners=[];this.lost=false;this.active=false;
    this.touchCfg={policy:false,padDefault:-1,emulate:false};this.touchPointers=new Map();this.padPointers=new Map();this.padVisible=false;this.padCapable=false;
    this.orientation=0;this.insets=[0,0,0,0];this.overlayCount=0;this.gameId='';
  }
  fail(e){this.stats.fatal=String(e?.message||e||'WebAssembly runtime aborted');this.stats.state='failed';this.report(String(e));if(this.control)this.store('quit',1);}
  on(target,type,fn,list=this.listeners){target.addEventListener(type,fn);list.push(()=>target.removeEventListener(type,fn));}
  // Input listeners live for one attach; a later attach must not stack a second set.
  unbindInput(){for(const off of this.inputListeners)off();this.inputListeners=[];}
  preflight(){
    if(!isSecureContext||!crossOriginIsolated||typeof SharedArrayBuffer==='undefined')throw Error('Secure context and COOP/COEP are required for shared Wasm memory');
    // (module (func (export "f") (result i32) i32.const 0) (func (result i32) return_call 0))
    const tail=new Uint8Array([0,97,115,109,1,0,0,0,1,5,1,96,0,1,127,3,3,2,0,0,10,11,2,4,0,65,0,11,4,0,18,0,11]);
    if(!WebAssembly.validate(tail))throw Error('WebAssembly tail calls are unavailable');
    this.gl=this.canvas.getContext('webgl',{alpha:false,depth:false,stencil:false,antialias:false});
    if(!this.gl)throw Error('WebGL unavailable; cannot start video');
    this.createGL();
    this.on(this.canvas,'webglcontextlost',e=>{e.preventDefault();this.lost=true;++this.stats.contextLosses;this.report('Video context lost; waiting for restoration');});
    this.on(this.canvas,'webglcontextrestored',()=>{try{this.lost=false;++this.stats.contextRestores;this.createGL();this.textureSize='';this.redraw=true;this.tickVideo();}catch(e){this.fail(e);}});
  }
  createGL(){
    const gl=this.gl;
    const shader=(type,source)=>{const s=gl.createShader(type);gl.shaderSource(s,source);gl.compileShader(s);if(!gl.getShaderParameter(s,gl.COMPILE_STATUS)){const msg=gl.getShaderInfoLog(s);gl.deleteShader(s);throw Error(msg);}return s;};
    const vs=shader(gl.VERTEX_SHADER,'attribute vec2 p; varying vec2 uv; uniform float flip; void main(){gl_Position=vec4(p,0.,1.);uv=vec2((p.x+1.)*.5,mix((p.y+1.)*.5,(1.-p.y)*.5,flip));}');
    const fs=shader(gl.FRAGMENT_SHADER,'precision highp float; varying vec2 uv; uniform sampler2D tex; void main(){gl_FragColor=vec4(texture2D(tex,uv).rgb,1.);}');
    this.program=gl.createProgram();gl.attachShader(this.program,vs);gl.attachShader(this.program,fs);gl.linkProgram(this.program);gl.deleteShader(vs);gl.deleteShader(fs);
    if(!gl.getProgramParameter(this.program,gl.LINK_STATUS))throw Error(gl.getProgramInfoLog(this.program));
    gl.useProgram(this.program);this.flip=gl.getUniformLocation(this.program,'flip');
    this.vbo=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,this.vbo);gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([-1,-1,1,-1,-1,1,1,1]),gl.STATIC_DRAW);
    const p=gl.getAttribLocation(this.program,'p');gl.enableVertexAttribArray(p);gl.vertexAttribPointer(p,2,gl.FLOAT,false,0,0);
    this.texture=this.newTexture();this.sharpTexture=null;this.fbo=null;this.sharpSize='';this.textureSize='';
    gl.pixelStorei(gl.UNPACK_ALIGNMENT,1);gl.clearColor(0,0,0,1);
    this.limit=Math.min(8192,gl.getParameter(gl.MAX_TEXTURE_SIZE),gl.getParameter(gl.MAX_RENDERBUFFER_SIZE));
    this.stats.renderer=gl.getParameter(gl.RENDERER);
  }
  newTexture(){const g=this.gl,t=g.createTexture();g.bindTexture(g.TEXTURE_2D,t);g.texParameteri(g.TEXTURE_2D,g.TEXTURE_WRAP_S,g.CLAMP_TO_EDGE);g.texParameteri(g.TEXTURE_2D,g.TEXTURE_WRAP_T,g.CLAMP_TO_EDGE);g.texParameteri(g.TEXTURE_2D,g.TEXTURE_MIN_FILTER,g.NEAREST);g.texParameteri(g.TEXTURE_2D,g.TEXTURE_MAG_FILTER,g.NEAREST);return t;}
  // Must be called directly by the Start gesture, before network awaits.
  async startAudio(){
    try {
      const Context=root.AudioContext||root.webkitAudioContext;
      this.audio=new Context();const resumed=this.audio.resume();
      const context=this.audio; // detach nulls this.audio before close() reports 'closed'
      context.onstatechange=()=>{this.stats.audio=context.state;this.report('Audio '+context.state);};
      await this.audio.audioWorklet.addModule('audio_worklet_bundle.js');
      this.node=new AudioWorkletNode(this.audio,'gbr-audio',{numberOfInputs:0,numberOfOutputs:1,outputChannelCount:[1]});
      this.gain=this.audio.createGain();this.node.connect(this.gain).connect(this.audio.destination);
      this.node.onprocessorerror=()=>this.audioFailure('Audio processor failed');
      // resume() may remain pending under autoplay. Worklet setup can still finish.
      resumed.catch(e=>this.report(String(e)));
      this.stats.audio=this.audio.state;
    }catch(e){this.audioFailure(e);}
  }
  audioFailure(e){this.stats.audio='failed';this.report('Audio unavailable: '+e);if(this.control)this.store('audioReady',2);}
  async resumeAudio(){if(this.audio)await this.audio.resume();}
  refresh(){if(!this.getBuffer)return;const b=this.getBuffer();if(b!==this.buffer){this.buffer=b;this.control=new Int32Array(b,this.ptr);++this.stats.viewGenerations;}}
  load(k){this.refresh();return Atomics.load(this.control,this.d.fields[k])>>>0;}
  store(k,v){this.refresh();if(this.control&&this.d.fields[k]!==undefined)Atomics.store(this.control,this.d.fields[k],v);}
  attach(getBuffer,ptr,d,orientation=0){
    if(this.active||d.version!==ABI_VERSION||d.magic!==0x47425257)throw Error('Unsupported host ABI or duplicate attach');
    if(!this.gl)throw Error('WebGL was not initialized');
    this.getBuffer=getBuffer;this.ptr=ptr;this.d=d;this.refresh();
    if(this.load('magic')!==d.magic||this.load('version')!==d.version||this.load('bytes')!==d.bytes||ptr+d.bytes>this.buffer.byteLength)throw Error('Invalid host descriptor');
    this.store('detached',DETACH.Pending);
    this.orientation=orientation;
    this.staging=new Uint8Array(d.pixelBytes);this.front=0;this.active=true;this.finalStats=undefined;this.stats.state='ready';this.stats.generation=this.load('generation');
    this.overlayCount=0;this.overlayBytes=null;this.ensureOverlayCanvas();
    this.observer?.disconnect(); // the post-exit observer of a previous attach
    this.bindInput();this.observer=new ResizeObserver(()=>{this.redraw=true;});this.observer.observe(this.canvas.parentElement);this.redraw=true;
    this.publishTouchState();
    if(this.node){
      this.node.port.onmessage=({data})=>{if(data.type==='ready'){this.report('Audio processor ready');}else if(data.type==='error'){this.audioFailure(data.message);}else if(data.type==='stopped'){this.stopAck?.();}};
      this.node.port.postMessage({type:'attach',buffer:this.buffer,ptr,descriptor:d});
      // A suspended graph may not instantiate/process until a later user gesture.
      if(this.audio.state!=='running')this.store('audioReady',2);
    }else this.store('audioReady',2);
    this.canvas.focus();this.tick();return true;
  }
  acquire(){
    if(!(this.load('middle')&4))return false;
    this.front=Atomics.exchange(this.control,this.d.fields.middle,this.front)&3;
    const p=this.ptr+this.d.video+this.front*this.d.videoStride,m=new Uint32Array(this.buffer,p,4);
    const [w,h,stride,seq]=m;
    if(!w||!h||w>this.d.maxWidth||h>this.d.maxHeight||stride!==w*3||stride*h>this.d.pixelBytes)throw Error('Invalid published video dimensions');
    const t=performance.now();this.staging.set(new Uint8Array(this.buffer,p+this.d.pixels,stride*h));
    // The frame's host overlay travels in the same slot; copy it privately too.
    if(this.d.overlay!==undefined){
      const [count,dropped]=new Uint32Array(this.buffer,p+this.d.overlay,2);
      if(count>this.d.overlayCmds)throw Error('Invalid host overlay length');
      this.overlayCount=count;this.stats.overlayDropped+=dropped;
      this.overlayBytes=count?new Uint8Array(this.buffer,p+this.d.overlay+8,count*this.d.overlayStride).slice():null;
    }
    this.stats.copyMs=performance.now()-t;this.meta={w,h,seq};this.stats.lastSeq=seq;++this.stats.consumed;this.redraw=true;return true;
  }
  // Safe-area insets (CSS env) that fall inside the canvas, in drawable pixels.
  measureInsets(rect,w){
    const doc=root.document;if(!doc?.createElement||!root.getComputedStyle)return [0,0,0,0];
    if(!this.insetProbe){const p=this.insetProbe=doc.createElement('div');p.style.cssText='position:fixed;left:0;top:0;width:0;height:0;visibility:hidden;pointer-events:none;padding:env(safe-area-inset-top,0px) env(safe-area-inset-right,0px) env(safe-area-inset-bottom,0px) env(safe-area-inset-left,0px)';doc.body?.appendChild(p);}
    const s=root.getComputedStyle(this.insetProbe),px=v=>parseFloat(v)||0,k=rect.width>0?w/rect.width:1;
    const vw=root.innerWidth||rect.right,vh=root.innerHeight||rect.bottom;
    const l=Math.max(0,px(s.paddingLeft)-rect.left),t=Math.max(0,px(s.paddingTop)-rect.top);
    const r=Math.max(0,rect.right-(vw-px(s.paddingRight))),b=Math.max(0,rect.bottom-(vh-px(s.paddingBottom)));
    return [l,t,r,b].map(v=>Math.min(65535,Math.round(v*k)));
  }
  // Destination rectangle, honouring the runtime's portrait top anchoring
  // (HostWindow::set_presentation_anchor_top) like host_window.cpp.
  placement(w,h,mw,mh){
    const l=layout(w,h,mw,mh);
    if(this.control&&this.load('anchorTop')&&h>w&&l.h>0&&l.h<h)l.y=Math.min(this.insets[1],h-l.h);
    return l;
  }
  tickVideo(){
    if(!this.staging)return;
    const fresh=this.control?this.acquire():false;
    const rect=this.canvas.getBoundingClientRect(),w=Math.min(this.limit,Math.max(1,Math.round(rect.width*devicePixelRatio))),h=Math.min(this.limit,Math.max(1,Math.round(rect.height*devicePixelRatio)));
    if(this.canvas.width!==w||this.canvas.height!==h){this.canvas.width=w;this.canvas.height=h;this.redraw=true;}
    if(this.control){
      this.store('drawable',w|(h<<16)); // physical pixels on both sides
      this.store('dprMilli',Math.round(devicePixelRatio*1000));
      if(this.redraw){this.insets=this.measureInsets(rect,w);this.store('insetsLT',this.insets[0]|(this.insets[1]<<16));this.store('insetsRB',this.insets[2]|(this.insets[3]<<16));}
    }
    if(this.lost||!this.meta||!this.redraw)return;
    const gl=this.gl,m=this.meta,t=performance.now();
    gl.bindTexture(gl.TEXTURE_2D,this.texture);
    const size=m.w+'x'+m.h;
    if(this.textureSize!==size){gl.texImage2D(gl.TEXTURE_2D,0,gl.RGB,m.w,m.h,0,gl.RGB,gl.UNSIGNED_BYTE,null);this.textureSize=size;this.needsUpload=true;}
    if(fresh||this.needsUpload){gl.texSubImage2D(gl.TEXTURE_2D,0,0,0,m.w,m.h,gl.RGB,gl.UNSIGNED_BYTE,this.staging.subarray(0,m.w*m.h*3));this.needsUpload=false;this.stats.lastUploadedSeq=m.seq;++this.stats.uploaded;}
    const l=this.placement(w,h,m.w,m.h),filter=this.control?this.load('filter'):this.lastFilter||0;
    // The worker maps touches and overlays through the rectangle actually drawn.
    if(this.control){this.store('viewXY',l.x|(l.y<<16));this.store('viewWH',l.w|(l.h<<16));}
    let factor=filter===2&&!l.integer?Math.floor(Math.min(l.w/m.w,l.h/m.h)):0;
    factor=Math.min(factor,Math.floor(this.limit/Math.max(m.w,m.h)),8);
    if(factor>=2){
      const sw=m.w*factor,sh=m.h*factor,key=sw+'x'+sh;
      if(this.sharpSize!==key){if(this.sharpTexture)gl.deleteTexture(this.sharpTexture);if(this.fbo)gl.deleteFramebuffer(this.fbo);this.sharpTexture=this.newTexture();gl.texImage2D(gl.TEXTURE_2D,0,gl.RGB,sw,sh,0,gl.RGB,gl.UNSIGNED_BYTE,null);this.fbo=gl.createFramebuffer();gl.bindFramebuffer(gl.FRAMEBUFFER,this.fbo);gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,this.sharpTexture,0);if(gl.checkFramebufferStatus(gl.FRAMEBUFFER)!==gl.FRAMEBUFFER_COMPLETE)throw Error('Sharp scaling framebuffer incomplete');this.sharpSize=key;}
      gl.bindFramebuffer(gl.FRAMEBUFFER,this.fbo);gl.viewport(0,0,sw,sh);gl.bindTexture(gl.TEXTURE_2D,this.texture);this.filter(false);gl.uniform1f(this.flip,1);gl.drawArrays(gl.TRIANGLE_STRIP,0,4);
      gl.bindTexture(gl.TEXTURE_2D,this.sharpTexture);this.filter(true);gl.uniform1f(this.flip,0);
    }else {gl.bindTexture(gl.TEXTURE_2D,this.texture);this.filter(filter===1);gl.uniform1f(this.flip,1);}
    gl.bindFramebuffer(gl.FRAMEBUFFER,null);gl.viewport(0,0,w,h);gl.clear(gl.COLOR_BUFFER_BIT);gl.viewport(l.x,l.y,l.w,l.h);gl.drawArrays(gl.TRIANGLE_STRIP,0,4);
    this.renderOverlay(w,h);
    this.stats.uploadMs=performance.now()-t;this.stats.width=m.w;this.stats.height=m.h;this.redraw=false;this.lastFilter=filter;
  }
  ensureOverlayCanvas(){
    const parent=this.canvas.parentElement,doc=root.document;
    if(this.overlayCanvas||!doc?.createElement||!parent?.appendChild)return;
    const c=this.overlayCanvas=doc.createElement('canvas');c.className='gbr-overlay';c.setAttribute?.('aria-hidden','true');
    parent.appendChild(c);this.overlayCtx=c.getContext?.('2d')||null;
  }
  renderOverlay(w,h){
    const c=this.overlayCanvas,ctx=this.overlayCtx;if(!c||!ctx)return;
    if(c.width!==w||c.height!==h){c.width=w;c.height=h;}
    ctx.clearRect(0,0,w,h);
    if(this.overlayCount&&this.overlayBytes)drawOverlay(ctx,new Uint32Array(this.overlayBytes.buffer),new Float32Array(this.overlayBytes.buffer),this.overlayCount);
  }
  filter(linear){const g=this.gl;g.texParameteri(g.TEXTURE_2D,g.TEXTURE_MIN_FILTER,linear?g.LINEAR:g.NEAREST);g.texParameteri(g.TEXTURE_2D,g.TEXTURE_MAG_FILTER,linear?g.LINEAR:g.NEAREST);}
  tick(){
    if(!this.active)return;
    try{
      this.pollGamepad();
      const filter=this.load('filter');if(filter!==this.lastFilter)this.redraw=true;
      const anchor=this.load('anchorTop');if(anchor!==this.lastAnchor){this.lastAnchor=anchor;this.redraw=true;}
      const scale=this.load('scale');if(scale!==this.lastScale){this.lastScale=scale;this.canvas.parentElement.style.maxWidth=(240*scale)+'px';this.redraw=true;}
      if(this.gain){const volume=this.load('audioEnabled')?this.load('volume')/100:0;if(volume!==this.lastVolume){this.gain.gain.setTargetAtTime(volume,this.audio.currentTime,0.005);this.lastVolume=volume;}}
      this.tickVideo();this.stats.state=this.stats.fatal?'failed':this.load('paused')?'paused':this.load('state')===1?'running':'ready';
      if(this.load('fps'))this.canvas.title=`Published ${this.load('published')} · uploaded ${this.stats.uploaded}`;
    }catch(e){this.fail(e);}
    this.raf=requestAnimationFrame(()=>this.tick());
  }
  snapshot(){
    const s={...this.stats,capabilities:{webgl:true,gamepad:!!root.navigator?.getGamepads,gyro:false,solarSensor:false,exclusiveFullscreen:false,savePersistence:!!root.GbrSaves?.mounted&&root.GbrSaves.state!=='unavailable',runtimeOverlay:false,
      touch:this.padCapable,touchPad:this.padVisible,hostOverlay:!!this.overlayCtx,overlayText:false,haptics:typeof root.navigator?.vibrate==='function'}};
    if(this.control)for(const key of Object.keys(this.d.fields))if(key!=='_end')s[key==='state'?'producerState':key]=this.load(key);
    return {...this.finalStats,...s};
  }
  // ---- keyboard -----------------------------------------------------------
  gameCodes(){const out=new Set();for(const k of this.keys){const b=parseBinding(k);if(b)out.add(b.code);}return out;}
  // Modifier state for binding matches. A held modifier key bound to a game
  // button (Select = ShiftRight) does not count, so Select+F1 loads slot 1 and
  // Select+P is not Pause. Event flags cover modifiers pressed before focus.
  effectiveMods(e){
    const game=this.gameCodes(),src=e||this.eventMods,out={};
    for(const m of MODIFIERS){
      let free=false,gameHeld=false;
      for(const [code,kind] of Object.entries(MODIFIER_CODES)){if(kind!==m||!this.keyboard.has(code))continue;if(game.has(code))gameHeld=true;else free=true;}
      out[m]=free||(!!src[EVENT_FLAG[m]]&&!gameHeld);
    }
    return out;
  }
  // Held game buttons: the bound key is down (and not consumed by a hotkey
  // chord) and every modifier the binding names is held. When several bindings
  // share a key, only the most specific satisfied one engages.
  keyboardMask(){
    const mods=this.effectiveMods(),parsed=this.keys.map(parseBinding),best=new Map();
    const live=b=>b&&this.keyboard.has(b.code)&&!this.consumed.has(b.code)&&requiredMods(b,mods);
    for(const b of parsed)if(live(b))best.set(b.code,Math.max(best.get(b.code)??-1,b.count));
    let mask=0;parsed.forEach((b,i)=>{if(live(b)&&best.get(b.code)===b.count)mask|=1<<i;});
    return mask;
  }
  turboHeld(){const t=parseBinding(this.hotkeys.Turbo);return !!t&&this.keyboard.has(t.code)&&exactMods(t,this.effectiveMods());}
  // Edge hotkey (including save-state slots) for a key press, exact modifiers
  // as on the native backend. Meta chords stay with the browser/OS.
  hotkeyFor(e){
    if(e.metaKey)return null;
    const mods=this.effectiveMods(e);
    for(const [name,text] of Object.entries(this.hotkeys)){const b=parseBinding(text);if(b&&b.code===e.code&&exactMods(b,mods))return {name,binding:b};}
    for(const s of SLOT_BINDINGS){const b=parseBinding(s.binding);if(b.code===e.code&&exactMods(b,mods))return {name:s.command,slot:s.slot,binding:b};}
    return null;
  }
  publishKeys(){if(!this.control)return;const pressed=this.gamepad|this.touch|this.keyboardMask();this.store('keys',(~pressed)&1023);this.store('inputUpdates',this.load('inputUpdates')+1);this.store('turbo',this.turboHeld()?1:0);}
  clearInput(){this.keyboard.clear();this.consumed.clear();this.eventMods={};this.gamepad=0;this.touch=0;this.padPointers.clear();this.renderPadState();this.cancelTouches();this.publishKeys();}
  command(kind,arg=0){if(!this.active)return false;const k=typeof kind==='string'?COMMAND[kind]:kind;if(!k)return false;const w=this.load('commandWrite'),r=this.load('commandRead');if(((w-r)>>>0)>=this.d.commandSlots){this.store('commandOverflow',this.load('commandOverflow')+1);this.report('Control queue full');return false;}const p=this.ptr+this.d.commands+(w&(this.d.commandSlots-1))*12;new Uint32Array(this.buffer,p,3).set([w,k,arg]);this.store('commandWrite',(w+1)>>>0);return true;}
  keyDown(e){
    this.eventMods={ctrlKey:e.ctrlKey,altKey:e.altKey,shiftKey:e.shiftKey};
    const hot=this.hotkeyFor(e),game=this.gameCodes().has(e.code),modifier=!!MODIFIER_CODES[e.code];
    if(!game&&!hot&&!modifier)return false;
    if(game||hot)e.preventDefault?.();
    this.keyboard.add(e.code);
    // A chord (hotkey with modifiers) owns its key until release; a bare
    // hotkey that is also a game button drives both, like the native backend.
    if(hot&&(hot.binding.count>0||!game))this.consumed.add(e.code);
    this.publishKeys();
    if(e.repeat||!hot)return true;
    if(hot.slot)this.command(hot.name,hot.slot);
    else if(hot.name==='Fullscreen')this.fullscreenRequest(root.document?.fullscreenElement?0:1);
    else if(hot.name!=='Turbo')this.command(hot.name);
    return true;
  }
  keyUp(e){
    this.eventMods={ctrlKey:e.ctrlKey,altKey:e.altKey,shiftKey:e.shiftKey};
    const held=this.keyboard.delete(e.code);this.consumed.delete(e.code);
    if(held&&this.gameCodes().has(e.code))e.preventDefault?.();
    this.publishKeys();return held;
  }
  // ---- touch ----------------------------------------------------------------
  touchEnabled(){return this.padCapable||this.touchCfg.policy||this.touchCfg.emulate;}
  // Page -> worker pointer ring (HostWindow::pump feeds the TouchHub).
  pushTouch(kind,pointer,x,y){
    if(!this.control||this.d.touches===undefined)return false;
    const w=this.load('touchWrite'),r=this.load('touchRead');
    if(((w-r)>>>0)>=this.d.touchSlots){this.store('touchOverflow',this.load('touchOverflow')+1);++this.stats.touchOverflow;return false;}
    const p=this.ptr+this.d.touches+(w&(this.d.touchSlots-1))*this.d.touchStride;
    const head=new Int32Array(this.buffer,p,2);head[0]=kind;head[1]=pointer|0;
    const xy=new Float32Array(this.buffer,p+8,2);xy[0]=x;xy[1]=y;
    this.store('touchWrite',(w+1)>>>0);return true;
  }
  drawablePoint(e){const r=this.canvas.getBoundingClientRect();return [(e.clientX-r.left)*(r.width>0?this.canvas.width/r.width:1),(e.clientY-r.top)*(r.height>0?this.canvas.height/r.height:1)];}
  pointerDown(e){
    const mouse=e.pointerType==='mouse';
    if(mouse&&!this.touchCfg.emulate)return false;
    if(!this.touchEnabled()||!this.control)return false;
    e.preventDefault?.();this.canvas.focus?.();
    const [x,y]=this.drawablePoint(e);
    if(mouse&&e.button!==0){
      // Desktop stand-ins for multi-finger taps (native: right = two, middle = three).
      if(e.button===2||e.button===1)this.pushTouch((e.button===2?TOUCH.TwoFingerTap:TOUCH.ThreeFingerTap)|TOUCH.FromMouse,0,x,y);
      return true;
    }
    this.canvas.setPointerCapture?.(e.pointerId);
    this.touchPointers.set(e.pointerId,mouse);
    this.pushTouch(TOUCH.Down|(mouse?TOUCH.FromMouse:0),e.pointerId,x,y);return true;
  }
  pointerMove(e,phase=TOUCH.Move){
    if(!this.touchPointers.has(e.pointerId))return false;
    const mouse=this.touchPointers.get(e.pointerId),[x,y]=this.drawablePoint(e);
    if(phase!==TOUCH.Move)this.touchPointers.delete(e.pointerId);
    this.pushTouch(phase|(mouse?TOUCH.FromMouse:0),e.pointerId,x,y);return true;
  }
  cancelTouches(){for(const [id,mouse] of this.touchPointers)this.pushTouch(TOUCH.Cancel|(mouse?TOUCH.FromMouse:0),id,0,0);this.touchPointers.clear();}
  // HostWindow::configure_touch. The page owns the virtual pad: capability,
  // the platform default (touch-primary devices) and the saved choice.
  configureTouch(policy,padDefault,emulate){
    this.touchCfg={policy:!!policy,padDefault:padDefault|0,emulate:!!emulate};
    const nav=root.navigator||{},coarse=!!root.matchMedia?.('(pointer: coarse)')?.matches;
    this.padCapable=!!emulate||(nav.maxTouchPoints|0)>0||!!root.matchMedia?.('(any-pointer: coarse)')?.matches;
    const saved=this.savedPadChoice();
    const def=this.touchCfg.padDefault<0?coarse:this.touchCfg.padDefault!==0;
    this.padVisible=this.padCapable&&(saved===null?def:saved);
    this.buildPad();this.publishTouchState();
    this.report(`Touch: ${policy?'game policy':'no policy'}, pad ${this.padCapable?(this.padVisible?'shown':'hidden'):'unavailable'}${emulate?', mouse emulation':''}`);
  }
  padKey(){return 'gbarecomp.touchPad.'+(this.gameId||root.GBARECOMP_ROM_SHA1||'default');}
  savedPadChoice(){try{const v=root.localStorage?.getItem(this.padKey());return v===null||v===undefined?null:v==='1';}catch(e){return null;}}
  setPadVisible(visible,persist=true){
    visible=!!visible&&this.padCapable;
    if(persist){try{root.localStorage?.setItem(this.padKey(),visible?'1':'0');}catch(e){}}
    if(!visible){this.padPointers.clear();this.touch=0;this.publishKeys();}
    this.padVisible=visible;this.renderPadState();this.publishTouchState();
  }
  publishTouchState(){
    if(!this.control)return;
    this.store('touchControls',this.padCapable?1:0);this.store('padVisible',this.padVisible?1:0);
    this.canvas.style&&(this.canvas.style.touchAction=this.touchEnabled()?'none':'');
  }
  // DOM virtual pad over the canvas. Its presses feed the touch mask (setTouch)
  // exactly like a gamepad; every other pointer on the canvas goes to the
  // TouchHub. One owner per pointer, as in host_window.cpp.
  buildPad(){
    const doc=root.document,parent=this.canvas.parentElement;
    if(!doc?.createElement||!parent?.appendChild)return;
    if(!this.pad&&this.padCapable){
      const pad=this.pad=doc.createElement('div');pad.className='gbr-pad';
      const dpad=doc.createElement('div');dpad.className='gbr-pad-dpad';dpad.dataset.dpad='1';pad.appendChild(dpad);
      for(const [name,label,bit] of PAD_BUTTONS){const b=doc.createElement('div');b.className='gbr-pad-btn gbr-pad-'+name;b.textContent=label;b.dataset.mask=String(bit);pad.appendChild(b);}
      const toggle=this.padToggle=doc.createElement('button');toggle.type='button';toggle.className='gbr-pad-toggle';toggle.textContent='Pad';toggle.title='Show/hide the touch gamepad';
      // Never take keyboard focus away from the game canvas.
      toggle.tabIndex=-1;for(const type of ['pointerdown','mousedown'])toggle.addEventListener(type,e=>e.preventDefault());
      toggle.addEventListener('click',()=>{this.setPadVisible(!this.padVisible);this.canvas.focus?.();});
      parent.appendChild(pad);parent.appendChild(toggle);
      const handle=e=>this.padPointer(e);
      for(const type of ['pointerdown','pointermove','pointerup','pointercancel'])this.on(pad,type,handle);
    }
    this.renderPadState();
  }
  renderPadState(){
    if(this.pad)this.pad.hidden=!this.padVisible;
    // The toggle is offered only when something other than the pad can drive
    // the game (a game touch policy); a pad-only title keeps its controls.
    if(this.padToggle)this.padToggle.hidden=!(this.padCapable&&this.touchCfg.policy);
    if(this.pad)for(const el of this.pad.children){const bit=Number(el.dataset.mask||0);el.classList?.toggle('held',!!(bit&&(this.touch&bit)));}
  }
  padMaskAt(x,y){
    const el=root.document?.elementFromPoint?.(x,y);
    if(!el||!this.pad?.contains?.(el))return 0;
    if(el.dataset.dpad){const r=el.getBoundingClientRect();return dpadMask(x-(r.left+r.width/2),y-(r.top+r.height/2),Math.min(r.width,r.height)/2);}
    return Number(el.dataset.mask||0);
  }
  padPointer(e){
    e.preventDefault?.();
    if(e.type==='pointerup'||e.type==='pointercancel')this.padPointers.delete(e.pointerId);
    else if(e.type==='pointerdown'||this.padPointers.has(e.pointerId))this.padPointers.set(e.pointerId,this.padMaskAt(e.clientX,e.clientY));
    let mask=0;for(const m of this.padPointers.values())mask|=m;
    if(mask!==this.touch)this.setTouch(mask);
  }
  setTouch(mask){this.touch=mask&1023;this.renderPadState();this.publishKeys();}
  // ---- browser services for the runtime --------------------------------------
  // host_haptic_pulse: navigator.vibrate has no amplitude and is absent on
  // some browsers (reported once, then a silent no-op).
  haptic(ms){
    const nav=root.navigator;
    if(typeof nav?.vibrate!=='function'){if(!this.hapticReported){this.hapticReported=true;this.report('Haptics unavailable in this browser');}return false;}
    try{return nav.vibrate(Math.max(1,Math.min(5000,ms|0)));}catch(e){return false;}
  }
  // host_request_settings_menu / unclaimed long press, three-finger tap, Back.
  // The page's controls are the browser host's settings: leave fullscreen and
  // let the page reveal them.
  requestSettings(){
    const doc=root.document;
    if(doc?.fullscreenElement)doc.exitFullscreen?.().catch?.(()=>{});
    this.report('Settings requested');
    try{root.dispatchEvent?.(new root.CustomEvent('gbrsettings'));}catch(e){}
  }
  lockOrientation(){
    const o=root.screen?.orientation;if(!o?.lock||this.orientation===2)return;
    o.lock(this.orientation===1?'portrait':'landscape').catch(()=>{});
  }
  bindInput(){
    this.unbindInput();
    const on=(target,type,fn)=>this.on(target,type,fn,this.inputListeners);
    const editable=e=>e.target?.closest?.('input,textarea,select,[contenteditable="true"]');
    on(this.canvas,'click',()=>this.canvas.focus());
    on(root,'keydown',e=>{
      if(editable(e)||document.activeElement!==this.canvas){this.clearInput();return;}
      if(this.touchCfg.emulate&&e.code==='Backspace'&&!e.repeat&&this.control){e.preventDefault();this.pushTouch(TOUCH.Back|TOUCH.FromMouse,0,0,0);return;}
      this.keyDown(e);
    });
    on(root,'keyup',e=>this.keyUp(e));
    on(this.canvas,'pointerdown',e=>this.pointerDown(e));
    on(this.canvas,'pointermove',e=>this.pointerMove(e));
    on(this.canvas,'pointerup',e=>this.pointerMove(e,TOUCH.Up));
    on(this.canvas,'pointercancel',e=>this.pointerMove(e,TOUCH.Cancel));
    on(root,'blur',()=>this.clearInput());
    on(document,'focusin',e=>{if(e.target!==this.canvas&&!this.pad?.contains?.(e.target))this.clearInput();});
    on(document,'visibilitychange',()=>{this.clearInput();this.store('hidden',document.hidden?1:0);});
    on(root,'gamepaddisconnected',()=>{this.gamepad=0;this.publishKeys();});
    on(document,'fullscreenchange',()=>{this.store('fullscreen',document.fullscreenElement?1:0);if(document.fullscreenElement)this.lockOrientation();this.redraw=true;});
    this.store('hidden',document.hidden?1:0);
  }
  pollGamepad(){
    let mask=0;
    if(document.activeElement===this.canvas&&!document.hidden){for(const p of navigator.getGamepads?.()||[]){if(!p||p.mapping!=='standard')continue;const map=[0,1,8,9,15,14,12,13,5,4];for(let i=0;i<10;++i)if(p.buttons[map[i]]?.pressed)mask|=1<<i;if(p.axes[0]>.25)mask|=1<<4;if(p.axes[0]<-.25)mask|=1<<5;if(p.axes[1]<-.25)mask|=1<<6;if(p.axes[1]>.25)mask|=1<<7;}}
    if(mask!==this.gamepad){this.gamepad=mask;this.publishKeys();}
  }
  fullscreenRequest(mode){
    if(mode===2)this.report('Exclusive fullscreen normalized to browser fullscreen');
    if(!mode){if(document.fullscreenElement)document.exitFullscreen().catch(e=>this.report(String(e)));return;}
    if(!navigator.userActivation?.isActive){this.report('Use the Fullscreen button to grant a browser gesture');return;}
    const request=this.canvas.parentElement.requestFullscreen?.();if(request)request.catch(e=>this.report(String(e)));else this.report('Fullscreen unavailable');
  }
  configure(keys,config){
    const section=(text,name)=>{let active=false;const out={};for(const raw of text.split(/\r?\n/)){const line=raw.trim();if(!line||/^[;#]/.test(line))continue;if(line[0]==='['){active=line.toLowerCase()===`[${name.toLowerCase()}]`;continue;}const i=line.indexOf('=');if(active&&i>=0)out[line.slice(0,i).trim()]=line.slice(i+1).trim();}return out;};
    const convert=value=>{
      const names={Return:'Enter',RShift:'ShiftRight','Right Shift':'ShiftRight',LShift:'ShiftLeft',Up:'ArrowUp',Down:'ArrowDown',Left:'ArrowLeft',Right:'ArrowRight',Space:'Space',Tab:'Tab',Escape:'Escape',Equals:'Equal',Minus:'Minus',LeftBracket:'BracketLeft',RightBracket:'BracketRight'};
      // Explicit SDL scancode subset, never KeyboardEvent.keyCode.
      const numeric={40:'Enter',41:'Escape',43:'Tab',44:'Space',79:'ArrowRight',80:'ArrowLeft',81:'ArrowDown',82:'ArrowUp',229:'ShiftRight',225:'ShiftLeft'};
      const parts=value.split('+');const key=parts.pop();let code=names[key]||(/^[a-z]$/i.test(key)?'Key'+key.toUpperCase():/^F([1-9]|1[0-2])$/.test(key)?key:null);
      if(/^\d+$/.test(key)){const n=Number(key);code=n>=4&&n<=29?'Key'+String.fromCharCode(65+n-4):numeric[n];}
      if(!value)return '';if(!code){this.report('Unsupported web binding: '+value);return null;}
      // Canonical modifier names/order so bindings compare structurally.
      const mods=parts.map(p=>({control:'Ctrl',ctrl:'Ctrl',alt:'Alt',shift:'Shift'})[p.toLowerCase()]);
      if(mods.some(m=>!m)){this.report('Unsupported web binding: '+value);return null;}
      return [...MODIFIERS.filter(m=>mods.includes(m)),code].join('+');
    };
    const mapping=['a','b','select','start','right','left','up','down','r','l'];
    for(const [k,v] of Object.entries(section(keys,'player1'))){const i=mapping.indexOf(k.toLowerCase()),code=convert(v);if(i>=0&&code!==null)this.keys[i]=code;}
    for(const [k,v] of Object.entries(section(config,'KeyMap'))){if(k in this.hotkeys||k in COMMAND){const code=convert(v);if(code!==null)this.hotkeys[k]=code;}else this.report('Unsupported web hotkey: '+k);}
  }
  // Concurrent callers share one detach; once it settles a later attach can
  // detach again (a memoized promise would never publish the new `detached`).
  detach(){
    if(!this.detaching)this.detaching=this.finishDetach().finally(()=>{this.detaching=null;});
    return this.detaching;
  }
  async finishDetach(){
    this.stats.state='stopping';this.active=false;cancelAnimationFrame(this.raf);
    let workletStopped=!this.node,contextClosed=!this.audio||this.audio.state==='closed';
    try{
      try{this.tickVideo();}catch(e){this.fail(e);}
      this.clearInput();this.unbindInput();this.observer?.disconnect();
      // A stopped message runs between process callbacks. Closing the context is
      // also a device-level barrier, including suspended/partially started audio.
      if(this.node){
        const stopped=new Promise(resolve=>{this.stopAck=resolve;});this.node.port.postMessage({type:'stop'});
        workletStopped=await Promise.race([stopped,new Promise(resolve=>setTimeout(()=>resolve(false),1000))])!==false;
        this.stopAck=null;
        this.node.disconnect();
      }
      if(this.audio&&this.audio.state!=='closed')await this.audio.close();
      contextClosed=!this.audio||this.audio.state==='closed';
    }catch(e){this.report('Detach cleanup failed: '+(e?.message||e));}
    finally{
      // Backend storage may be freed only after both page-side audio barriers.
      const detached=workletStopped&&contextClosed?DETACH.SafeToFree:DETACH.UnsafeRetain;
      try{this.finalStats=this.snapshot();this.finalStats.detached=detached;}catch(e){this.report('Detach stats failed: '+(e?.message||e));}
      try{this.store('detached',detached);}catch(e){this.report('Detach flag failed: '+(e?.message||e));}
      if(detached===DETACH.UnsafeRetain)this.report('Audio detach not confirmed; retaining wasm host storage');
      // Keep only the private staging image for resize/context restoration after
      // guest exit. No callback may retain a view into freed game storage.
      this.control=null;this.buffer=null;this.getBuffer=null;
      this.node=null;this.gain=null;this.audio=null;this.lastVolume=undefined;
      this.stats.state=this.stats.fatal?'failed':'exited';this.stats.audio='closed';
      this.observer=new ResizeObserver(()=>{this.redraw=true;this.tickVideo();});this.observer.observe(this.canvas.parentElement);
    }
  }
  async shutdown(){await this.detach();}
  dispose(){cancelAnimationFrame(this.raf);this.observer?.disconnect();this.unbindInput();for(const off of this.listeners)off();this.listeners=[];if(this.gl&&!this.lost){const g=this.gl;g.deleteTexture(this.texture);g.deleteTexture(this.sharpTexture);g.deleteFramebuffer(this.fbo);g.deleteBuffer(this.vbo);g.deleteProgram(this.program);}}
}
root.GbrWebHost=Host;
if(typeof module!=='undefined')module.exports={Host,layout,parseBinding,dpadMask,drawOverlay,TOUCH,OVERLAY,ABI_VERSION};
})(globalThis);
