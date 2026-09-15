// Independent worklet module. All four RAB banks are owned by one audio thread.
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif
#define RECOMP_AUDIO_DRC_IMPL
#include "recomp_audio_drc.h"
#include <algorithm>
#include <cstring>
static rab_bridge banks[4], initial[4];
static int16_t input[2048], output[2048];
static int active=1;
static double segment_frames=0, rendered_frames=0;
static uint64_t under=0, over=0, conceal=0;
extern "C" {
EXPORT void dsp_free(){for(auto& b:banks)rab_free(&b);}
EXPORT int dsp_init(int host){
    dsp_free();
    for(int i=0;i<4;++i){rab_config c;rab_config_defaults(&c);c.channels=1;c.source_rate=32768u<<i;c.host_rate=host;c.target_ms=60;c.preroll_ms=250;
        if(rab_init(&banks[i],&c)){dsp_free();return 1;}initial[i]=banks[i];}
    return 0;
}
EXPORT void dsp_reset(int index){
    if(index<0||index>3)return;
    auto& old=banks[active];under+=old.stats.underrun_events;over+=old.stats.overflow_drops;conceal+=old.stats.stretch_frames;
    active=index; banks[active]=initial[active];
    std::memset(banks[active].ring,0,banks[active].cap*sizeof(float));
    segment_frames=rendered_frames=0;
}
EXPORT int16_t* dsp_input(){return input;}
EXPORT int16_t* dsp_output(){return output;}
EXPORT void dsp_push(int n){if(n>0&&n<=2048){rab_push(&banks[active],input,n);segment_frames+=double(n)*banks[active].cfg.host_rate/banks[active].cfg.source_rate;}}
// Ending a rate segment is bounded by its sample duration, not RAB silence
// (concealment can continue after its final source sample).
EXPORT int dsp_pull(int n,int ending){
    if(n<1||n>2048)return 0;
    auto& b=banks[active];
    int count=ending?std::min(n,int(std::max(0.0,ceil(segment_frames-rendered_frames)))):n;
    if(ending && !b.primed)b.primed=1;
    const bool primed=b.primed || rab_fill_ms(&b)>=b.prime_ms;
    rab_pull(&b,output,count);
    if(primed)rendered_frames+=count;
    return count;
}
EXPORT double dsp_fill(){return rab_fill_ms(&banks[active]);}
EXPORT double dsp_underruns(){return double(under+banks[active].stats.underrun_events);}
EXPORT double dsp_overflow(){return double(over+banks[active].stats.overflow_drops);}
EXPORT double dsp_concealed(){return double(conceal+banks[active].stats.stretch_frames);}
EXPORT double dsp_correction(){return banks[active].stats.last_correction;}
}
