#define SDL_MAIN_HANDLED
#include "host_window.h"
#include <SDL.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstdlib>

static void set_env(const char* key,const char* value) {
    // SDL's Windows environment helper and the C runtime have distinct
    // caches. The host reads engine settings through std::getenv.
#ifdef _WIN32
    _putenv_s(key,value);
#else
    setenv(key,value,1);
#endif
    SDL_setenv(key,value,1);
}

int main(int argc,char** argv) {
    if(argc!=2)return 2;
    bool disk=false,dummy=false;
    for(int i=0;i<SDL_GetNumAudioDrivers();++i)
        disk|=std::string(SDL_GetAudioDriver(i))=="disk";
    for(int i=0;i<SDL_GetNumVideoDrivers();++i)
        dummy|=std::string(SDL_GetVideoDriver(i))=="dummy";
    if(!disk||!dummy)return 77; // Optional SDL test drivers, not runtime requirements.
    set_env("SDL_VIDEODRIVER","dummy");
    set_env("SDL_RENDER_DRIVER","software");
    set_env("SDL_AUDIODRIVER","disk");
    set_env("SDL_DISKAUDIODELAY","15");
    for(bool queued:{false,true}) {
        const auto file=std::string(argv[1])+(queued?"-queued.raw":"-callback.raw");
        set_env("SDL_DISKAUDIOFILE",file.c_str());
        // An explicit disk-device path also avoids SDL2's Windows getenv
        // scratch-buffer alias between its output filename and delay hint.
        set_env("GBARECOMP_AUDIO_DEVICE",file.c_str());
        set_env("GBARECOMP_AUDIO_DIRECT",queued?"1":"0");
        gbarecomp::HostWindow window;
        if(!window.open(1,240,160,"audio fixture"))return 3;
        window.set_volume(50);
        std::array<int16_t,4096> pcm{};
        for(unsigned i=0;i<2048;++i){pcm[i*2]=12000;pcm[i*2+1]=-6000;}
        for(unsigned block=0;block<40;++block) {
            window.push_audio_stereo_block(pcm.data(),2048,32768);
            SDL_Delay(16);
        }
        window.push_audio_stereo_block(pcm.data(),2048,48000); // invalid rate rejected
        window.reset_audio();
        pcm.fill(0);
        for(unsigned block=0;block<20;++block){window.push_audio_stereo_block(pcm.data(),2048,65536);SDL_Delay(16);}
        SDL_Delay(100);
        window.close();
        std::ifstream input(file,std::ios::binary|std::ios::ate);
        if(!input)return 4;
        const auto bytes=input.tellg();if(bytes<=0||bytes%4)return 5;
        std::vector<int16_t> out(size_t(bytes)/2);input.seekg(0);input.read(reinterpret_cast<char*>(out.data()),bytes);
        int peak=0;size_t steady=0,longest=0;
        for(size_t i=0;i<out.size()/2;++i){
            peak=std::max(peak,std::abs(int(out[i*2])));
            if(std::abs(int(out[i*2])+2*int(out[i*2+1]))>3)return 6;
            if(std::abs(int(out[i*2])-6000)<=60&&std::abs(int(out[i*2+1])+3000)<=30)
                longest=std::max(longest,++steady);
            else steady=0;
        }
        // Band-limited conversion may ring at the initial DC step. Check
        // sustained gain as well as a bounded peak, rather than forbidding it.
        if(longest<1024||peak>9000)return 7;
        if(!std::all_of(out.end()-std::min<size_t>(out.size(),2048),out.end(),[](int16_t v){return v==0;}))return 8;
        input.close();std::filesystem::remove(file);
    }
    std::puts("native stereo: callback/queued routing, gain, rates and reset passed");
    return 0;
}
