// Canonical-firmware SWI fixture for caller-contract tests. No firmware
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
armv4t::CPUState original(Memory& memory, const ArmCpuState& caller, bool thumb, uint64_t& instructions, uint8_t swi) {
    armv4t::CPUState cpu{};
    std::copy(std::begin(caller.R),std::end(caller.R),std::begin(cpu.R));
    cpu.cpsr.mode=uint8_t(armv4t::Mode::Supervisor);cpu.cpsr.i=true;
    cpu.thumb=cpu.cpsr.t=false;
    cpu.banked_sp[armv4t::Bank_User]=caller.R[13];
    cpu.banked_lr[armv4t::Bank_User]=caller.R[14];
    cpu.banked_sp[armv4t::Bank_Supervisor]=0x03007FE0;
    cpu.banked_spsr[armv4t::Bank_Supervisor]=caller.cpsr;
    cpu.R[13]=0x03007FE0;cpu.R[14]=kRom+(thumb?2:4);cpu.R[15]=8;
    memory.rom=thumb?std::array<uint8_t,4>{swi,0xdf,0,0}:std::array<uint8_t,4>{0,0,swi,0xef};
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

