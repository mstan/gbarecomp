/* Portable full-bridge oracle corpus. Compile unchanged against each header.
 * --emit writes PCM and complete pointer-free state for byte comparison. */
#define RECOMP_AUDIO_DRC_IMPL
#include "recomp_audio_drc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#endif
static int emitting;
static unsigned long long digest = 1469598103934665603ull, bytes;
static unsigned rng = 0x7b17331u;
static unsigned next_value(void) {rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static void emit(const void* p,size_t n) {
 const unsigned char* b=(const unsigned char*)p;
 for(size_t i=0;i<n;++i){digest^=b[i];digest*=1099511628211ull;}
 if(emitting && fwrite(p,1,n,stdout)!=n)exit(2);bytes+=n;
}
#define FIELD(x) emit(&b->x,sizeof(b->x))
static void state(const rab_bridge* b,int ring) {
 FIELD(cfg);FIELD(half);FIELD(cap);FIELD(in_count);FIELD(out_pos);
 FIELD(cur_step);FIELD(err_lp);FIELD(corr);FIELD(gain);FIELD(primed);
 FIELD(prime_ms);FIELD(last_out);FIELD(concealing);FIELD(loop_start);
 FIELD(loop_len);FIELD(loop_pos);FIELD(conceal_frames);FIELD(stats);
 if(ring){emit(b->ring,(size_t)b->cap*b->cfg.channels*sizeof(float));
 emit(b->coeffs,(size_t)b->cfg.phases*b->cfg.taps*sizeof(float));}
}
int main(int argc,char** argv) {
 emitting=argc==2 && !strcmp(argv[1],"--emit");
#if defined(_WIN32)
 if(emitting && _setmode(_fileno(stdout),_O_BINARY)<0)return 5;
#endif
 const double source[]={32768,65536,48000,44100};
 const double host[]={48000,48000,44100,96000};
 unsigned cases=0,pulls=0;
 for(int channels=1;channels<=2;++channels)for(int taps_i=0;taps_i<5;++taps_i)
 for(int rate=0;rate<4;++rate)for(int phase_i=0;phase_i<2;++phase_i) {
  rab_config cfg;memset(&cfg,0,sizeof(cfg));rab_config_defaults(&cfg);
  cfg.channels=channels;cfg.taps=4<<taps_i;cfg.phases=phase_i?512:17;
  cfg.source_rate=source[rate];cfg.host_rate=host[rate];
  cfg.ring_ms=7;cfg.target_ms=3;cfg.preroll_ms=3;
  cfg.stretch_min_ms=.3;cfg.stretch_max_ms=.7;cfg.stretch_xfade_ms=.2;cfg.stretch_limit_ms=3;
  rab_bridge bridge;memset(&bridge,0,sizeof(bridge));rab_bridge* b=&bridge;
  if(rab_init(b,&cfg))return 3;
  int maximum=(int)b->cap+32;if(maximum<1024)maximum=1024;
  int16_t* input=(int16_t*)malloc((size_t)maximum*channels*sizeof(int16_t));
  int16_t output[1600];if(!input)return 4;
  for(int call=0;call<72;++call) {
   int count=(int)(next_value()%700);
   if(call%13==0)count=(int)b->cap+17; /* overflow */
   if(call%11==0 || call%11==1)count=0; /* starvation / concealment */
   for(int i=0;i<count*channels;++i)input[i]=(int16_t)next_value();
   rab_push(b,input,count);
   int pulled=(int)(next_value()%750);if(call%17==0)pulled=0;
   rab_pull(b,output,pulled);++pulls;
   emit(output,(size_t)pulled*channels*sizeof(int16_t));state(b,call%8==0);
  }
  /* Exercise the scalar service's initial-history clamp, fractional phase,
     ring wrap and large absolute positions using the populated physical ring. */
  const double positions[]={-90.75,-16,-.125,0,.875,16.5,513.125,1048576.25};
  for(unsigned j=0;j<sizeof(positions)/sizeof(positions[0]);++j){
   float samples[2];rab__sample_at(b,positions[j],samples);emit(samples,sizeof(samples));
  }
  state(b,1);free(input);rab_free(b);++cases;
 }
 fprintf(stderr,"Audio bridge corpus: %u configurations/%u callbacks, mono/stereo,4..64 taps,17/512phases, rates32768..96000, wrap/overflow/preroll/conceal/clamp; %llu bytes hash=%016llx\n",cases,pulls,bytes,digest);
 return 0;
}
