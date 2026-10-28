#include "arm_codegen.h"
#include "arm_decode.h"
#include "thumb_decode.h"
#include <fstream>
#include <cstdint>
#include <vector>
#include <cstdio>

int main(int argc,char** argv) {
    if(argc!=2) return 2;
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
    return out.good()?0:4;
}
