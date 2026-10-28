#include "runtime_arm.h"
#include "runtime_native_region.h"
#include "arm_decode.h"
#include "thumb_decode.h"
#include "cpu_state.h"
#include "bus.h"
#include "interpreter.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <vector>

unsigned commits=0;
extern "C" void runtime_native_region_end(RuntimeNativeRegion* region) {
    if(region->cycles>=region->bound) throw std::runtime_error("test region exceeded admission bound");
    ++commits;
}
#include "n02_generated.inc"

struct Bus final:armv4t::Bus {
    std::vector<uint8_t> ewram=std::vector<uint8_t>(0x40000);
    std::vector<uint8_t> iwram=std::vector<uint8_t>(0x8000);
    RuntimeWaitTable waits{};
    uint8_t* ptr(uint32_t address) {
        if(address>>24==2) return ewram.data()+(address&0x3ffff);
        if(address>>24==3) return iwram.data()+(address&0x7fff);
        throw std::runtime_error("unexpected original device access before native fallback");
    }
    uint8_t read8(uint32_t address) override {return *ptr(address);}
    uint16_t read16(uint32_t address) override {return read8(address)|(uint16_t(read8(address+1))<<8);}
    uint32_t read32(uint32_t address) override {return read16(address)|(uint32_t(read16(address+2))<<16);}
    void write8(uint32_t address,uint8_t value) override {*ptr(address)=value;}
    void write16(uint32_t address,uint16_t value) override {write8(address,uint8_t(value));write8(address+1,uint8_t(value>>8));}
    void write32(uint32_t address,uint32_t value) override {write16(address,uint16_t(value));write16(address+2,uint16_t(value>>16));}
    uint32_t access_cycles(uint32_t address,uint8_t width,bool sequential) const override {
        const auto region=address>>24;
        return 1u+(width==4?(sequential?waits.s32[region]:waits.n32[region]):
                             (sequential?waits.s16[region]:waits.n16[region]));
    }
    uint32_t code_wait(uint32_t pc,bool thumb,bool sequential) const override {
        return rwt_code_wait(&waits,pc,thumb,sequential);
    }
    int32_t prefetch_stall(int32_t wait,uint32_t pc,bool thumb) override {
        return rwt_prefetch_stall(&waits,wait,pc,thumb);
    }
};
uint32_t packed(const armv4t::CPSR& f) {
    return (f.n?CPSR_N_BIT:0)|(f.z?CPSR_Z_BIT:0)|(f.c?CPSR_C_BIT:0)|
           (f.v?CPSR_V_BIT:0)|(f.i?CPSR_I_BIT:0)|(f.f?CPSR_F_BIT:0)|
           (f.t?CPSR_T_BIT:0)|f.mode;
}
int main() {
    try {
        std::mt19937 random(0x4e303232); unsigned prefixes=0,steps=0;
        for(unsigned mode=0;mode!=2;++mode) for(unsigned test=0;test!=1024;++test) {
            const bool thumb=mode==0;
            Bus original;
            for(auto& byte:original.ewram) byte=uint8_t(random());
            for(auto& byte:original.iwram) byte=uint8_t(random());
            auto& w=original.waits;
            w.n16[2]=w.s16[2]=uint8_t(1+test%15);w.n32[2]=w.s32[2]=uint8_t(2*w.n16[2]+1);
            const unsigned first[]={4,3,2,8};const unsigned second[]={2,1,4,8};
            w.n16[8]=uint8_t(first[test%4]);w.s16[8]=uint8_t(second[(test/4)%4]);
            w.n32[8]=uint8_t(w.n16[8]+1+w.s16[8]);w.s32[8]=uint8_t(2*w.s16[8]+1);
            w.prefetch=test&1;w.last_prefetched_pc=0x08000ff0+(test%16)*2;
            Bus native=original;
            armv4t::CPUState cpu{};
            for(auto& value:cpu.R)value=random();
            cpu.R[2]=(test&1?0x02004000:0x03001000)+(test%4);
            cpu.R[3]=test%256;
            cpu.R[4]=test%7==0?0x04000000:test%5==0?0x03007fff:0x02000100+(test%4);
            cpu.R[15]=0x08001000;
            cpu.cpsr.n=random()&1;cpu.cpsr.z=random()&1;cpu.cpsr.c=random()&1;cpu.cpsr.v=random()&1;
            cpu.cpsr.mode=0x1f;cpu.cpsr.t=cpu.thumb=thumb;
            RuntimeNativeRegion region;
            std::copy_n(cpu.R,16,region.cpu.R);region.cpu.cpsr=packed(cpu.cpsr);
            region.waits=w;region.ewram=native.ewram.data();region.iwram=native.iwram.data();region.bound=100000;
            commits=0;
            const auto next=thumb?run_native_0(region):run_native_1(region);
            const auto* opcodes=thumb?program_0:program_1;
            uint32_t cycles=0;
            while(cpu.R[15]!=next) {
                const auto pc=cpu.R[15];const auto index=(pc-0x08001000)/(thumb?2:4);
                if(index>=16)throw std::runtime_error("native returned invalid prefix PC");
                auto ins=thumb?armv4t::ThumbDecoder::decode(uint16_t(opcodes[index]),pc):armv4t::ArmDecoder::decode(opcodes[index],pc);
                uint32_t cost=0;
                const auto result=armv4t::Interpreter::step(cpu,original,ins,&cost);
                if(result==armv4t::Interpreter::Result::Undefined ||
                   result==armv4t::Interpreter::Result::NotImplemented ||
                   result==armv4t::Interpreter::Result::Swi)
                    throw std::runtime_error("authored instruction not executable");
                cycles+=cost;++steps;
            }
            const bool equal=std::equal(cpu.R,cpu.R+16,region.cpu.R) && packed(cpu.cpsr)==region.cpu.cpsr &&
                original.ewram==native.ewram && original.iwram==native.iwram && cycles==region.cycles &&
                std::memcmp(&original.waits,&region.waits,sizeof(w))==0 && commits==1;
            if(!equal) {
                std::fprintf(stderr,"mode=%u test=%u next=%08x cycles ref=%u native=%u flags ref=%08x native=%08x\n",mode,test,next,cycles,region.cycles,packed(cpu.cpsr),region.cpu.cpsr);
                for(unsigned r=0;r!=16;++r)if(cpu.R[r]!=region.cpu.R[r])std::fprintf(stderr,"r%u ref=%08x native=%08x\n",r,cpu.R[r],region.cpu.R[r]);
                throw std::runtime_error("mixed native region differs from original prefix");
            }
            if(next!=0x08001000+16*(thumb?2:4))++prefixes;
        }
        std::printf("native_region_contract cases=2048 original_steps=%u prefix_settlements=%u registers_flags_RAM_prefetch_cycles=exact\n",steps,prefixes);
        return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"native_region_contract FAILED: %s\n",error.what());return 1;}
}
