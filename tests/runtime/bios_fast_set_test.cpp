// Original-BIOS oracle for the native CpuFastSet caller contract. No firmware
// bytes are embedded; supply the owner's canonical dump as the first argument.
#include "bios_hle.h"
#include "runtime_arm.h"
#include "arm_decode.h"
#include "thumb_decode.h"
#include "interpreter.h"
#include "cpu_state.h"
#include "bus.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
constexpr uint32_t kRom = 0x08000000, kUserStack = 0x03007F00;
struct Memory final : armv4t::Bus {
    std::vector<uint8_t> ewram = std::vector<uint8_t>(0x40000);
    std::vector<uint8_t> iwram = std::vector<uint8_t>(0x8000);
    std::vector<uint8_t> bios;
    std::array<uint8_t,4> rom{};
    bool bad = false;
    uint32_t access_cycles(uint32_t, uint8_t, bool) const override { return 1; }
    uint8_t* ptr(uint32_t a, unsigned size) {
        uint8_t* p = nullptr; size_t capacity=0, offset=0;
        if (a < 0x4000) {p=bios.data();capacity=bios.size();offset=a;}
        else if ((a>>24)==2) {p=ewram.data();capacity=ewram.size();offset=a&0x3ffff;}
        else if ((a>>24)==3) {p=iwram.data();capacity=iwram.size();offset=a&0x7fff;}
        else if ((a>>24)==8) {p=rom.data();capacity=rom.size();offset=a&0x1ffffff;}
        if (!p || offset>capacity || size>capacity-offset) {bad=true;return nullptr;}
        return p+offset;
    }
    uint8_t read8(uint32_t a) override {auto* p=ptr(a,1);return p?*p:0;}
    uint16_t read16(uint32_t a) override {auto* p=ptr(a,2);return p?uint16_t(p[0]|uint16_t(p[1])<<8):0;}
    uint32_t read32(uint32_t a) override {
        auto* p=ptr(a,4);return p?uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24):0;
    }
    void write8(uint32_t a,uint8_t v) override {if(auto* p=ptr(a,1))*p=v;}
    void write16(uint32_t a,uint16_t v) override {if(auto* p=ptr(a,2)){p[0]=uint8_t(v);p[1]=uint8_t(v>>8);}}
    void write32(uint32_t a,uint32_t v) override {if(auto* p=ptr(a,4))for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
};
Memory* active = nullptr;
uint64_t charged_cycles = 0;
bool persistent_iwram_equal(const Memory& a,const Memory& b) {
    for(unsigned i=0;i<a.iwram.size();++i) {
        // The SWI wrapper and subroutine use dead caller/SVC stack scratch.
        // Native service ABI does not promise to reproduce those stack writes.
        if((i>=0x7ec0 && i<0x7f00) || (i>=0x7f60 && i<0x7fe0))continue;
        if(a.iwram[i]!=b.iwram[i])return false;
    }
    return true;
}
uint32_t pack(const armv4t::CPSR& p) {
    return (uint32_t(p.n)<<31)|(uint32_t(p.z)<<30)|(uint32_t(p.c)<<29)|(uint32_t(p.v)<<28)|
        (uint32_t(p.i)<<7)|(uint32_t(p.f)<<6)|(uint32_t(p.t)<<5)|uint32_t(p.mode);
}
armv4t::CPUState original(Memory& memory, const ArmCpuState& caller, bool thumb, uint64_t& instructions) {
    armv4t::CPUState cpu{};
    std::copy(std::begin(caller.R),std::end(caller.R),std::begin(cpu.R));
    cpu.cpsr.mode=uint8_t(armv4t::Mode::Supervisor);cpu.cpsr.i=true;
    cpu.thumb=cpu.cpsr.t=false;
    cpu.banked_sp[armv4t::Bank_User]=caller.R[13];
    cpu.banked_lr[armv4t::Bank_User]=caller.R[14];
    cpu.banked_sp[armv4t::Bank_Supervisor]=0x03007FE0;
    cpu.banked_spsr[armv4t::Bank_Supervisor]=caller.cpsr;
    cpu.R[13]=0x03007FE0;cpu.R[14]=kRom+(thumb?2:4);cpu.R[15]=8;
    memory.rom=thumb?std::array<uint8_t,4>{0x0c,0xdf,0,0}:std::array<uint8_t,4>{0,0,0x0c,0xef};
    unsigned steps=0;
    while(cpu.R[15]!=kRom+(thumb?2:4)) {
        if(++steps>100000)throw std::runtime_error("BIOS instruction bound");
        const uint32_t pc=cpu.R[15];
        const auto instruction=cpu.cpsr.t?armv4t::ThumbDecoder::decode(memory.read16(pc),pc):
            armv4t::ArmDecoder::decode(memory.read32(pc),pc);
        uint32_t cycles=0;
        const auto result=armv4t::Interpreter::step(cpu,memory,instruction,&cycles);
        if(result==armv4t::Interpreter::Result::Undefined || result==armv4t::Interpreter::Result::NotImplemented ||
           result==armv4t::Interpreter::Result::Swi || memory.bad)
            throw std::runtime_error("BIOS oracle instruction/access failed");
    }
    instructions+=steps;
    return cpu;
}
}

extern "C" {
ArmCpuState g_cpu{};
int (*g_bios_hle_hook)(uint32_t)=nullptr;
uint8_t bus_read_u8(uint32_t a) {return active->read8(a);}
uint16_t bus_read_u16(uint32_t a) {return active->read16(a);}
uint32_t bus_read_u32(uint32_t a) {return active->read32(a);}
void bus_write_u8(uint32_t a,uint8_t v) {active->write8(a,v);}
void bus_write_u16(uint32_t a,uint16_t v) {active->write16(a,v);}
void bus_write_u32(uint32_t a,uint32_t v) {active->write32(a,v);}
void runtime_tick(uint32_t cycles) {charged_cycles+=cycles;}
}

int main(int argc,char** argv) try {
    if(argc!=2){std::fprintf(stderr,"Supply canonical BIOS dump for original CpuFastSet oracle\n");return 77;}
    std::ifstream input(argv[1],std::ios::binary);
    const std::vector<uint8_t> firmware{std::istreambuf_iterator<char>(input),{}};
    if(firmware.size()!=16384)throw std::runtime_error("BIOS size");
    uint32_t crc=~0u;
    for(uint8_t byte:firmware){crc^=byte;for(unsigned i=0;i<8;++i)crc=(crc>>1)^(0xedb88320u&uint32_t(-int32_t(crc&1)));}
    if((crc^~0u)!=0x81977335u)throw std::runtime_error("canonical BIOS CRC");
    constexpr unsigned counts[]{0,1,7,8,9,15,16,17,31,32,63,64};
    constexpr int offsets[]{4,16,32,-4,0,1024};
    unsigned cases=0,failures=0;uint64_t instructions=0;
    gba::bios_hle_set_mode(gba::BiosHleMode::Off);
    if(g_bios_hle_hook)throw std::runtime_error("Off must retain BIOS floor");
    gba::bios_hle_set_mode(gba::BiosHleMode::On);
    for(uint32_t source:{0x02001000u,0x03001000u})
    for(bool thumb:{false,true})for(bool fill:{false,true})for(unsigned count:counts)for(int offset:offsets) {
        Memory reference;reference.bios=firmware;
        for(unsigned i=0;i<reference.ewram.size();++i)reference.ewram[i]=uint8_t((i*73u+(i>>3)*19u)^0xa7u);
        for(unsigned i=0;i<reference.iwram.size();++i)reference.iwram[i]=uint8_t((i*31u+(i>>4)*7u)^0x3du);
        Memory candidate=reference;
        ArmCpuState caller{};
        for(unsigned i=0;i<16;++i)caller.R[i]=0x31410000u+i*0x10001u;
        caller.R[0]=source;caller.R[1]=caller.R[0]+offset;
        caller.R[2]=count|(fill?0x01000000u:0);caller.R[13]=kUserStack;
        caller.R[14]=0x08001000;caller.R[15]=kRom+(thumb?2:4);
        caller.cpsr=0x6000001fu|(thumb?0x20u:0);
        const auto expected=original(reference,caller,thumb,instructions);
        g_cpu=caller;active=&candidate;charged_cycles=0;
        if(!g_bios_hle_hook(0x0c))throw std::runtime_error("supported copy declined");
        bool equal=!candidate.bad && reference.ewram==candidate.ewram &&
            persistent_iwram_equal(reference,candidate) &&
            std::equal(std::begin(expected.R),std::end(expected.R),std::begin(g_cpu.R)) && pack(expected.cpsr)==g_cpu.cpsr;
        if(!equal) {
            if(failures++<6){
                std::fprintf(stderr,"diff case%u thumb%u fill%u count%u offset%d RAM=%u/%u CPSR=%08x/%08x\n",cases,thumb,fill,count,offset,reference.ewram==candidate.ewram,
                    persistent_iwram_equal(reference,candidate),pack(expected.cpsr),g_cpu.cpsr);
                for(unsigned i=0;i<16;++i)if(expected.R[i]!=g_cpu.R[i])std::fprintf(stderr," r%u %08x/%08x\n",i,expected.R[i],g_cpu.R[i]);
            }
        }
        ++cases;
    }
    // Unsupported alignment must decline before touching registers, memory or
    // the scheduler, so the real BIOS remains authoritative for that caller.
    for(unsigned which=0;which<2;++which) {
        Memory memory;memory.bios=firmware;active=&memory;
        ArmCpuState caller{};caller.R[0]=0x02001000u+(which==0);caller.R[1]=0x02002000u+(which==1);caller.R[2]=8;
        g_cpu=caller;charged_cycles=0;
        const int handled=g_bios_hle_hook(0x0c);
        if(handled || charged_cycles || std::memcmp(&g_cpu,&caller,sizeof(g_cpu)) ||
           std::any_of(memory.ewram.begin(),memory.ewram.end(),[](uint8_t v){return v!=0;}))++failures;
    }
    gba::bios_hle_set_mode(gba::BiosHleMode::Off);active=nullptr;
    std::printf("CpuFastSet original-BIOS oracle: %u cases/%llu instructions and 2 untouched alignment declines; caller registers/CPSR and persistent RAM; failures=%u\n",cases,(unsigned long long)instructions,failures);
    return failures?1:0;
} catch(const std::exception& e) {std::fprintf(stderr,"CpuFastSet oracle: %s\n",e.what());return 1;}
