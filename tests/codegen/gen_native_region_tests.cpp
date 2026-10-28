#include "arm_codegen.h"
#include "arm_decode.h"
#include "thumb_decode.h"
#include "emit_function.h"
#include <fstream>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <unordered_set>
#include <stdexcept>

int main(int argc,char** argv) {
    if(argc!=2) return 2;
    // A native block must never bypass a configured callback, relocation or
    // a control transfer just because its ordinary lowering exists.
    const auto instruction=armv4t::ThumbDecoder::decode(0x2001,0x08001000);
    for(unsigned reject=0;reject!=4;++reject) {
        armv4t::CodegenCtx ctx;ctx.native_region=true;
        const std::unordered_set<uint32_t> overrides{instruction.pc};
        if(reject==0)ctx.runtime_pc_base="_relocated";
        if(reject==1)ctx.runtime_arm_immediate=true;
        if(reject==2)ctx.alu_immediate_override_pcs=&overrides;
        bool rejected=false;
        try {
            const auto candidate=reject==3 ? armv4t::ThumbDecoder::decode(0xe000,instruction.pc):instruction;
            (void)armv4t::ArmCodegen::emit_instr(candidate,ctx,nullptr);
        }catch(const std::invalid_argument&){rejected=true;}
        if(!rejected)throw std::runtime_error("unsupported native block bypassed its declared contract");
    }
    std::ofstream out(argv[1]);
    const std::vector<std::vector<uint32_t>> programs{
        {0x4148,0x4188,0x60d0,0x8821,0x4058,0x4098,0x40d9,0x4119,
         0x41d9,0x70d0,0x78d5,0x5f61,0x43c8,0x1800,0x2800,0x21ff},
        {0xe0b00001,0xe0d00001,0xe582000c,0xe1d410b0,0x10300001,
         0xe1b00310,0xe1b01331,0xe1b01351,0xe1b01371,0xe5c20003,
         0xe5d25003,0xe19410f5,0xe1f00001,0xe0800000,0xe3500000,0xe3b010ff}
    };
    for(unsigned mode=0;mode!=2;++mode) {
        const bool thumb=mode==0;
        out << "static const uint32_t program_"<<mode<<"[]={";
        for(auto opcode:programs[mode]) out<<opcode<<"u,";
        out<<"};\nuint32_t run_native_"<<mode<<"(RuntimeNativeRegion& _region) {\n"
              "    auto& g_cpu=_region.cpu;\n";
        uint32_t pc=0x08001000;
        std::vector<uint32_t> pcs;
        for(auto opcode:programs[mode]) {
            auto ins=thumb ? armv4t::ThumbDecoder::decode(uint16_t(opcode),pc)
                           : armv4t::ArmDecoder::decode(opcode,pc);
            if(!armv4t::ArmCodegen::supports_native_region(ins)) {
                std::fprintf(stderr,"unsupported fixture %08x at %08x\n",opcode,pc);return 3;
            }
            armv4t::CodegenCtx ctx;ctx.native_region=true;
            const auto emitted=armv4t::ArmCodegen::emit_instr(ins,ctx,nullptr);
            out<<"    {\n"<<emitted<<"    }\n";
            if(emitted.find("goto NR_REF_")!=std::string::npos)pcs.push_back(pc);
            pc+=thumb?2:4;
        }
        out<<"    _region.commit(); return "<<pc<<"u;\n";
        for(auto address:pcs) out<<"NR_REF_"<<std::hex<<address<<std::dec<<": return "<<address<<"u;\n";
        out<<"}\n";
    }
    out<<"#if defined(NATIVE_REGION_FULL_FUNCTION_TESTS)\n";
    for(unsigned mode=0;mode!=2;++mode) {
        const auto step=mode==0?2u:4u;
        std::vector<uint8_t> image;
        for(auto opcode:programs[mode]) for(unsigned byte=0;byte<step;++byte)
            image.push_back(uint8_t(opcode>>(byte*8)));
        const uint32_t ret=mode==0?0x4770u:0xe12fff1eu;
        for(unsigned byte=0;byte<step;++byte)image.push_back(uint8_t(ret>>(byte*8)));
        gbarecomp::Function function{};
        function.addr=function.source_addr=0x08001000;
        function.end_addr=function.addr+uint32_t(image.size());
        function.mode=mode==0?gbarecomp::CpuMode::Thumb:gbarecomp::CpuMode::Arm;
        function.alias_entries.push_back(function.addr+4*step);
        for(unsigned native=0;native!=2;++native) {
            out<<"void run_function_"<<mode<<"_"<<native<<"() {\n";
            out<<gbarecomp::emit_function_body_str(function,image.data(),image.size(),function.addr,
                {},nullptr,nullptr,false,native!=0);
            out<<"}\n";
        }
    }
    out<<"#endif\n";
    return out.good()?0:4;
}
