#include "emit_function.h"
#include <array>
#include <fstream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ofstream out(argv[1], std::ios::binary);
    if (!out) return 3;
    out << R"CPP(
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "gba_bus.h"
#include <array>
#include <cstdio>
#include <cstring>
#if defined(GBARECOMP_NATIVE_RAM_CODE_VIEWS)
#define RAM_VIEW_QUALIFICATION_ENABLED 1
#endif
)CPP";
    for (bool thumb : {false,true}) {
        std::array<uint8_t,12> bytes{};
        if (thumb) bytes = {7,0x20,1,0x30,0x70,0x47};
        else {
            const uint32_t words[]{0xE3A00007u,0xE2800001u,0xE12FFF1Eu};
            for (unsigned n=0;n<3;++n) for (unsigned b=0;b<4;++b)
                bytes[n*4+b] = uint8_t(words[n]>>(b*8));
        }
        gbarecomp::Function fn{};
        fn.addr=0x03000400u; fn.source_addr=0x08000000u;
        fn.end_addr=fn.addr+(thumb?6u:12u);
        fn.mode=thumb?gbarecomp::CpuMode::Thumb:gbarecomp::CpuMode::Arm;
        fn.alias_entries={fn.addr+(thumb?2u:4u)};
        const auto body=gbarecomp::emit_function_body_str(fn,bytes.data(),bytes.size(),
            fn.source_addr,{},nullptr,nullptr,true);
        for (bool candidate : {false,true}) {
            out << "\n#undef GBARECOMP_NATIVE_RAM_CODE_VIEWS\n";
            if (candidate) out << "#define GBARECOMP_NATIVE_RAM_CODE_VIEWS 1\n";
            out << "static void " << (candidate?"candidate_":"reference_")
                << (thumb?"thumb":"arm") << "() {\n" << body << "}\n";
        }
    }
    out << R"CPP(
namespace {
gba::GbaBus a,b;
unsigned view_calls;
gba::GbaBus* rebound;
const uint8_t* counted_view(uint32_t pc,uint32_t size) {
    ++view_calls;
    return gbarecomp::active_bus()->native_code_view(pc,size);
}
void change_owner(uint32_t) { gbarecomp::set_active_bus(rebound); }
struct Result { ArmCpuState cpu; gbarecomp::RuntimeTimingContext timing; uint32_t depth; unsigned calls; };
Result run(void(*fn)(),bool thumb,unsigned alias,bool change,uint32_t encoded) {
    runtime_init(&a);
    gbarecomp::set_active_bus(&a);
    gbarecomp::set_active_ppu(nullptr);
    const std::array<uint32_t,3> words{0xE3A00000u|encoded,0xE2800001u,0xE12FFF1Eu};
    const uint8_t halfs[]{7,0x20,1,0x30,0x70,0x47};
    for (auto* bus : {&a,&b}) {
        if(thumb) std::memcpy(bus->iwram_ptr()+0x400,halfs,sizeof(halfs));
        else for(unsigned n=0;n<3;++n) for(unsigned k=0;k<4;++k)
            bus->iwram_ptr()[0x400+n*4+k]=uint8_t(words[n]>>(k*8));
    }
    if(!thumb) b.iwram_ptr()[0x400]^=2u; // live immediate from rebound owner
    g_runtime_ram_code_view=counted_view;
    g_runtime_native_ram_code_source={counted_view,a.ewram_ptr(),a.iwram_ptr()};
    g_cpu={};g_cpu.cpsr=0x1Fu|(thumb?CPSR_T_BIT:0u);
    g_cpu.R[0]=9;g_cpu.R[14]=0x08000000u|(thumb?1u:0u);g_cpu.R[15]=0x03000400u;
    runtime_call_stack_restore(nullptr,0);
    runtime_call_push_return(0x08000000u);
    g_runtime_native_ram_base=0x03000400u;
    g_runtime_resume_pc=alias?0x03000400u+(thumb?2u:4u):0u;
    gbarecomp::runtime_restore_timing_context({});
    g_runtime_fn_entry_hook=change?change_owner:nullptr;rebound=&b;view_calls=0;
    fn();
    Result result{};result.cpu=g_cpu;result.depth=runtime_call_stack_depth();result.calls=view_calls;
    gbarecomp::runtime_capture_timing_context(result.timing);
    g_runtime_fn_entry_hook=nullptr;
    return result;
}
bool equal(const Result& x,const Result& y) {
    return !std::memcmp(&x.cpu,&y.cpu,sizeof(x.cpu)) && x.depth==y.depth &&
        x.timing.cycles==y.timing.cycles && x.timing.pending_cycles==y.timing.pending_cycles &&
        x.timing.event_budget==y.timing.event_budget && x.timing.vblank_starts==y.timing.vblank_starts &&
        x.timing.yielded_vblank==y.timing.yielded_vblank;
}
}
int main() {
#if !defined(RAM_VIEW_QUALIFICATION_ENABLED)
    return 77;
#else
    unsigned cases=0,bypassed=0;
    for(bool thumb:{false,true}) for(unsigned alias:{0u,1u}) for(bool change:{false,true})
    for(uint32_t encoded:{7u,0x123u,0xA81u,0xF91u}) {
        const auto ref=run(thumb?reference_thumb:reference_arm,thumb,alias,change,encoded);
        const auto candidate=run(thumb?candidate_thumb:candidate_arm,thumb,alias,change,encoded);
        if(!equal(ref,candidate)) {std::fprintf(stderr,"RAM generated mismatch thumb=%d alias=%u rebind=%d encoded=%x\n",thumb,alias,change,encoded);return 1;}
        if(!alias&&!change) {
            if(!ref.calls||candidate.calls) {std::fprintf(stderr,"RAM generated view did not replace original calls\n");return 2;}
            bypassed+=ref.calls;
        }
        ++cases;
    }
    gbarecomp::set_active_bus(nullptr);
    std::printf("Generated RAM views: %u exact ARM/Thumb/resume/rebind/immediate cases; %u maintained view calls bypassed\n",cases,bypassed);
#endif
}
)CPP";
    return out ? 0 : 4;
}
