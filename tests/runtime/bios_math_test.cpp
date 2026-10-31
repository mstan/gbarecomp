// Edge cases for the caller-visible Div/DivArm register contract. In
// particular, abs(INT_MIN) is representable as a GBA unsigned register even
// though negating an int32_t INT_MIN is undefined C++ arithmetic.
#include "bios_hle.h"
#include "runtime_arm.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace {
uint32_t charged_cycles = 0;
}
extern "C" {
ArmCpuState g_cpu{};
int (*g_bios_hle_hook)(uint32_t) = nullptr;
uint8_t bus_read_u8(uint32_t) { std::abort(); }
uint16_t bus_read_u16(uint32_t) { std::abort(); }
uint32_t bus_read_u32(uint32_t) { std::abort(); }
void bus_write_u8(uint32_t,uint8_t) { std::abort(); }
void bus_write_u16(uint32_t,uint16_t) { std::abort(); }
void bus_write_u32(uint32_t,uint32_t) { std::abort(); }
void runtime_tick(uint32_t cycles) { charged_cycles += cycles; }
}

int main() {
    struct Case { int32_t numerator,denominator;uint32_t quotient,remainder,magnitude; };
    constexpr auto minimum = std::numeric_limits<int32_t>::min();
    constexpr auto maximum = std::numeric_limits<int32_t>::max();
    constexpr Case cases[] {
        {minimum,1,0x80000000u,0,0x80000000u},
        {minimum,-1,0x80000000u,0,0x80000000u},
        {minimum,2,0xc0000000u,0,0x40000000u},
        {maximum,1,0x7fffffffu,0,0x7fffffffu},
        {maximum,-1,0x80000001u,0,0x7fffffffu},
        {-7,3,0xfffffffeu,0xffffffffu,2},
        {7,-3,0xfffffffeu,1,2},
        {-7,-3,2,0xffffffffu,2},
        {0,1,0,0,0}, {1,1,1,0,1}, {0,-1,0,0,0},
        {minimum,maximum,0xffffffffu,0xffffffffu,1},
    };
    gba::bios_hle_set_mode(gba::BiosHleMode::Off);
    if (g_bios_hle_hook) return 1;
    gba::bios_hle_set_mode(gba::BiosHleMode::On);
    unsigned checks = 0;
    for (uint32_t cpsr : {0x6000001fu,0x9000003fu}) {
        for (uint32_t swi : {6u,7u}) for (const auto& test : cases) {
            ArmCpuState before{};
            for (unsigned i=0;i<16;++i) before.R[i]=0x31410000u+i*0x10001u;
            before.R[0]=uint32_t(swi==6?test.numerator:test.denominator);
            before.R[1]=uint32_t(swi==6?test.denominator:test.numerator);
            before.cpsr=cpsr;
            ArmCpuState expected=before;
            expected.R[0]=test.quotient;expected.R[1]=test.remainder;expected.R[3]=test.magnitude;
            g_cpu=before;charged_cycles=0;
            if (!g_bios_hle_hook(swi) || !charged_cycles || std::memcmp(&g_cpu,&expected,sizeof(g_cpu))) {
                std::fprintf(stderr,"Div caller contract differs: swi=%u numerator=%d denominator=%d\n",swi,test.numerator,test.denominator);
                return 2;
            }
            ++checks;
        }
    }
    gba::bios_hle_set_mode(gba::BiosHleMode::Off);
    if (g_bios_hle_hook) return 3;
    std::printf("BIOS Div/DivArm: %u exact boundary caller-register checks; no memory effects\n",checks);
}
