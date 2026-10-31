// Actual canonical firmware is a private argument, never embedded. This tests
// the build-selected math-only hook with the full legacy catalog Off.
#include "bios_firmware_fixture.h"
#include <bit>
#include <limits>

int main(int argc,char** argv) try {
    if(argc!=2){std::fprintf(stderr,"Supply canonical BIOS for native math contract\n");return 77;}
    std::ifstream input(argv[1],std::ios::binary);
    const std::vector<uint8_t> firmware{std::istreambuf_iterator<char>(input),{}};
    if(firmware.size()!=16384)throw std::runtime_error("BIOS size");
    uint32_t crc=~0u;
    for(uint8_t byte:firmware){crc^=byte;for(unsigned i=0;i<8;++i)crc=(crc>>1)^(0xedb88320u&uint32_t(-int32_t(crc&1)));}
    if((crc^~0u)!=0x81977335u)throw std::runtime_error("canonical BIOS CRC");
    if(!gba::bios_native_math_enabled())throw std::runtime_error("fixture did not compile native math profile");
    gba::bios_hle_set_mode(gba::BiosHleMode::Off);
    if(!g_bios_hle_hook || gba::bios_hle_mode()!=gba::BiosHleMode::Off)
        throw std::runtime_error("math-only selection enabled full legacy catalog or lost hook");
    unsigned cases=0,failures=0,declines=0;uint64_t instructions=0;
    const auto check=[&](uint8_t swi,int32_t x,int32_t y,bool thumb) {
        Memory reference;reference.bios=firmware;
        for(unsigned i=0;i<reference.ewram.size();++i)reference.ewram[i]=uint8_t(i*73u+0xa7u);
        for(unsigned i=0;i<reference.iwram.size();++i)reference.iwram[i]=uint8_t(i*31u+0x3du);
        Memory candidate=reference;
        ArmCpuState caller{};
        for(unsigned i=0;i<16;++i)caller.R[i]=0x31410000u+i*0x10001u;
        caller.R[0]=uint32_t(x);caller.R[1]=uint32_t(y);caller.R[13]=kUserStack;
        caller.R[14]=0x08001000;caller.R[15]=kRom+(thumb?2:4);
        caller.cpsr=0x6000001fu|(thumb?0x20u:0);
        const auto expected=original(reference,caller,thumb,instructions,swi);
        g_cpu=caller;active=&candidate;charged_cycles=0;
        if(!g_bios_hle_hook(swi))throw std::runtime_error("selected valid math request declined");
        bool equal=!candidate.bad && charged_cycles && reference.ewram==candidate.ewram &&
            persistent_iwram_equal(reference,candidate) && pack(expected.cpsr)==g_cpu.cpsr &&
            std::equal(std::begin(expected.R),std::end(expected.R),std::begin(g_cpu.R));
        if(!equal) {
            if(failures++<16) {
                std::fprintf(stderr,"math diff swi=%u x=%d y=%d thumb=%u RAM=%u/%u CPSR=%08x/%08x\n",swi,x,y,thumb,
                    reference.ewram==candidate.ewram,persistent_iwram_equal(reference,candidate),pack(expected.cpsr),g_cpu.cpsr);
                for(unsigned i=0;i<16;++i)if(expected.R[i]!=g_cpu.R[i])
                    std::fprintf(stderr," r%u %08x/%08x\n",i,expected.R[i],g_cpu.R[i]);
            }
        }
        ++cases;
    };
    constexpr int32_t lo=std::numeric_limits<int32_t>::min(),hi=std::numeric_limits<int32_t>::max();
    constexpr int32_t numerators[]{lo,lo+1,-65536,-32768,-7,-1,0,1,7,32767,65535,hi};
    constexpr int32_t denominators[]{lo,-65536,-3,-1,1,2,3,32767,65536,hi};
    for(int32_t numerator:numerators)for(int32_t denominator:denominators)for(bool thumb:{false,true}) {
        check(6,numerator,denominator,thumb);
        check(7,denominator,numerator,thumb);
    }
    constexpr int32_t coordinates[]{lo,lo+1,-0x20000,-0x10000,-32768,-257,-1,0,1,257,32767,0x10000,0x20000,hi};
    for(int32_t x:coordinates)for(int32_t y:coordinates)for(bool thumb:{false,true})check(10,x,y,thumb);
    uint32_t seed=0x31a7b05du;
    for(unsigned i=0;i<96;++i) {
        seed=seed*1664525u+1013904223u;const int32_t x=std::bit_cast<int32_t>(seed);
        seed=seed*1664525u+1013904223u;const int32_t y=std::bit_cast<int32_t>(seed);
        for(bool thumb:{false,true})check(10,x,y,thumb);
    }
    Memory untouched;untouched.bios=firmware;active=&untouched;
    ArmCpuState caller{};
    for(unsigned i=0;i<16;++i)caller.R[i]=0x73510000u+i;
    caller.cpsr=0x9000003fu;
    for(unsigned swi=0;swi<256;++swi) {
        if(swi==6 || swi==7 || swi==10)continue;
        g_cpu=caller;charged_cycles=0;
        if(g_bios_hle_hook(swi) || charged_cycles || std::memcmp(&g_cpu,&caller,sizeof(g_cpu)) ||
            std::any_of(untouched.ewram.begin(),untouched.ewram.end(),[](uint8_t v){return v!=0;}) ||
            std::any_of(untouched.iwram.begin(),untouched.iwram.end(),[](uint8_t v){return v!=0;}))
            throw std::runtime_error("unselected service changed caller/memory/cycles");
        ++declines;
    }
    for(unsigned swi:{6u,7u}) {
        ArmCpuState zero=caller;zero.R[swi==6?1:0]=0;
        g_cpu=zero;charged_cycles=0;
        if(g_bios_hle_hook(swi) || charged_cycles || std::memcmp(&g_cpu,&zero,sizeof(g_cpu)))
            throw std::runtime_error("zero denominator did not decline untouched");
        ++declines;
    }
    active=nullptr;
    std::printf("Native math original-BIOS oracle: cases=%u instructions=%llu untouched-declines=%u failures=%u; catalog Off\n",
        cases,(unsigned long long)instructions,declines,failures);
    return failures?1:0;
} catch(const std::exception& error){std::fprintf(stderr,"Native math fixture: %s\n",error.what());return 2;}
