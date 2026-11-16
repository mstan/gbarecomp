#define SDL_MAIN_HANDLED
#include "stereo_audio_stream.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>

int main() {
    SDL_setenv("SDL_AUDIODRIVER","dummy",1);
    if (SDL_Init(SDL_INIT_AUDIO) != 0) return 8;
    gbarecomp::StereoAudioStream stream;
    std::array<int16_t,2048> pcm{};
    for(unsigned i=0;i<1024;++i){pcm[i*2]=12000;pcm[i*2+1]=-6000;}
    for(int host:{32000,32040,44100,48000,65536}) for(uint32_t rate:{32768u,65536u,131072u,262144u}) {
        stream.reset();std::vector<int16_t> output;
        // Same total stream split at odd boundaries: state must survive calls.
        if(!stream.convert(pcm.data(),13,rate,host,output)||
           !stream.convert(pcm.data()+26,1011,rate,host,output)||!stream.finish(output))return 1;
        const auto expected=(1024u*host+rate-1)/rate;
        if(output.size()!=expected*2){std::printf("duration %u: %zu != %u\n",rate,output.size(),expected*2);return 2;}
        bool audible=false;
        for(size_t i=0;i<output.size()/2;++i){
            if(std::abs(int(output[i*2])+2*int(output[i*2+1]))>3)return 3;
            audible|=output[i*2]>1000;
        }
        if(!audible)return 4;
    }
    // Distinct L/R tones expose pitch changes and missing/merged channels.
    for (int host : {32000,32040,44100,48000}) {
        constexpr unsigned rate=32768;
        std::vector<int16_t> tone(rate*2), converted;
        for (unsigned i=0;i<rate;++i) {
            tone[i*2]=static_cast<int16_t>(12000*std::sin(6.283185307179586*220*i/rate));
            tone[i*2+1]=static_cast<int16_t>(9000*std::sin(6.283185307179586*440*i/rate));
        }
        stream.reset();
        if (!stream.convert(tone.data(),rate,rate,host,converted)||!stream.finish(converted)) return 12;
        if (converted.size()!=size_t(host)*2) return 13;
        for (unsigned channel=0;channel<2;++channel) {
            unsigned crossings=0;
            for (int i=65;i<host-64;++i)
                crossings+=converted[(i-1)*2+channel]<=0 && converted[i*2+channel]>0;
            const double hz=double(crossings)*host/(host-129);
            if (std::abs(hz-(channel?440:220))>2.0) return 14;
        }
    }
    stream.reset();std::vector<int16_t> output;
    for(unsigned i=0;i<127;++i) {
        const size_t frames=547+(i&1);
        if(!stream.convert(pcm.data(),frames,32768,48000,output))return 9;
    }
    if(!stream.finish(output)){std::puts("nonintegral frame-block duration failed");return 10;}
    const uint64_t duration=((547u*127+63u)*48000u+32767)/32768;
    if(output.size()/2>duration || duration-output.size()/2>127)return 11;
    stream.reset();output.clear();
    if(stream.convert(pcm.data(),1024,48000,65536,output)||
       stream.convert(nullptr,1024,65536,65536,output)||
       stream.convert(pcm.data(),1024,65536,0,output)||!output.empty())return 5;
    // A rate transition drains the old rate before accepting the new one.
    if(!stream.convert(pcm.data(),1024,32768,65536,output)||
       !stream.convert(pcm.data(),1024,131072,65536,output)||!stream.finish(output)||
       output.size()!=(2048+512)*2)return 6;
    stream.reset();output.clear();pcm.fill(0);
    if(!stream.convert(pcm.data(),1024,65536,65536,output)||!stream.finish(output)||
       !std::all_of(output.begin(),output.end(),[](int16_t v){return v==0;}))return 7;
    std::puts("queued stereo conversion: rates, segmentation, isolation, rejection and reset passed");
    return 0;
}
