/* Appended to the single-file Emscripten DSP factory by build_web.sh. */
class GbrAudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ready = false; this.stopped = false; this.rate = 65536; this.epoch = 0;
    this.fade = 0; this.last = 0; this.transition = 0;
    this.port.onmessage = async ({data}) => {
      if (data.type === 'stop') {
        this.stopped = true; this.ready = false;
        if (this.dsp) this.dsp._dsp_free();
        this.control = null; this.buffer = null;
        // This acknowledgement is the page/C++ ownership barrier: no later
        // process callback may retain a view into the shared wasm storage.
        this.port.postMessage({type:'stopped'}); return;
      }
      if (data.type !== 'attach' || this.stopped) return;
      try {
        this.buffer=data.buffer; this.base=data.ptr; this.d=data.descriptor; this.f=this.d.fields;
        this.control=new Int32Array(this.buffer,this.base);
        this.dsp=await createGbrAudioDSP();
        if (this.stopped) {this.dsp._dsp_free(); return;}
        if (this.dsp._dsp_init(sampleRate)) throw Error('RAB initialization failed');
        this.dsp._dsp_reset(1);
        this.input=this.dsp.HEAP16.subarray(this.dsp._dsp_input()/2,this.dsp._dsp_input()/2+2048);
        this.output=this.dsp.HEAP16.subarray(this.dsp._dsp_output()/2,this.dsp._dsp_output()/2+2048);
        this.views=Array.from({length:this.d.audioSlots},(_,i)=>{
          const p=this.base+this.d.audio+i*this.d.audioStride;
          return {meta:new Uint32Array(this.buffer,p,3),pcm:new Int16Array(this.buffer,p+12,this.d.audioSamples)};
        });
        this.store('hostRate',sampleRate); this.store('audioReady',1); this.ready=true;
        this.port.postMessage({type:'ready'});
      } catch(e) {this.ready=false;this.port.postMessage({type:'error',message:String(e)});}
    };
  }
  load(k){return Atomics.load(this.control,this.f[k])>>>0;}
  store(k,v){Atomics.store(this.control,this.f[k],v);}
  process(inputs,outputs) {
    const out=outputs[0]?.[0]; if(!out)return !this.stopped;
    out.fill(0); if(!this.ready || this.stopped)return !this.stopped;
    const reset=this.load('resetRequest');
    if(reset!==this.load('resetAck')){
      this.store('audioRead',this.load('audioWrite'));this.dsp._dsp_reset(Math.log2(this.rate/32768));
      this.fade=0;this.store('resetAck',reset);this.epoch=reset;return true;
    }
    let offset=0, budget=32;
    while(offset<out.length && budget-->0){
      let r=this.load('audioRead'),w=this.load('audioWrite'),ending=false;
      // A bounded number of block copies, no allocation or communication here.
      for(let blocks=0;r!==w && blocks<8;++blocks){
        const v=this.views[r&(this.d.audioSlots-1)],n=v.meta[0],rate=v.meta[1],epoch=v.meta[2];
        if(n<1||n>this.d.audioSamples||!(rate===32768||rate===65536||rate===131072||rate===262144)){
          this.store('audioReady',2);this.ready=false;return true;
        }
        if(rate!==this.rate||epoch!==this.epoch){ending=true;break;}
        if(this.dsp._dsp_fill()>275)break;
        for(let i=0;i<n;++i)this.input[i]=v.pcm[i];
        this.dsp._dsp_push(n);r=(r+1)>>>0;
        this.store('audioRead',r);this.store('audioConsumed',(this.load('audioConsumed')+n)>>>0);
      }
      const n=this.dsp._dsp_pull(Math.min(2048,out.length-offset),ending?1:0);
      for(let i=0;i<n;++i){
        this.fade=Math.min(1,this.fade+1/(sampleRate*0.005));
        let value=this.output[i]/32768*this.fade;
        if(this.transition>0){const mix=this.transition/Math.round(sampleRate*0.005);value=value*(1-mix)+this.last*mix;--this.transition;}
        out[offset++]=value;
      }
      if(ending && n===0){
        this.last=offset?out[offset-1]:this.last;this.transition=Math.round(sampleRate*0.005);
        const meta=this.views[r&(this.d.audioSlots-1)].meta;
        this.rate=meta[1];this.epoch=meta[2];this.dsp._dsp_reset(Math.log2(this.rate/32768));
      } else if(n===0)break;
    }
    this.store('audioFill',Math.round(this.dsp._dsp_fill()*1000));
    this.store('underruns',this.dsp._dsp_underruns());this.store('concealed',this.dsp._dsp_concealed());
    this.store('audioDspOverflow',this.dsp._dsp_overflow());
    return true;
  }
}
registerProcessor('gbr-audio',GbrAudioProcessor);
