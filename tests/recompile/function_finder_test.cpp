#include "function_finder.h"
#include "emit_function.h"
#include <algorithm>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr std::uint32_t kBase = 0x08000000u;

void put16(std::vector<std::uint8_t>& rom, std::size_t off,
           std::uint16_t value) {
    rom[off] = static_cast<std::uint8_t>(value);
    rom[off + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::vector<std::uint8_t>& rom, std::size_t off,
           std::uint32_t value) {
    put16(rom, off, static_cast<std::uint16_t>(value));
    put16(rom, off + 2, static_cast<std::uint16_t>(value >> 16));
}

bool has_function(const gbarecomp::FunctionFinder& finder,
                  std::uint32_t addr, gbarecomp::CpuMode mode) {
    for (const auto& fn : finder.functions()) {
        if (fn.addr == addr && fn.mode == mode) return true;
    }
    return false;
}

const gbarecomp::Function* find_function(
    const gbarecomp::FunctionFinder& finder,
    std::uint32_t addr,
    gbarecomp::CpuMode mode) {
    for (const auto& fn : finder.functions()) {
        if (fn.addr == addr && fn.mode == mode) return &fn;
    }
    return nullptr;
}

static bool referenced_leaf_pair(unsigned fault) {
    std::vector<std::uint8_t> rom(0x240, 0);
    put16(rom, 0, 0x4770); // ordinary reachable entry
    put32(rom, 0x20, kBase + 0x1C0); // ROM reference to the descriptor
    put32(rom, 0x1C0, kBase + 0x101); // leaf callback
    put32(rom, 0x1C4, kBase + 0x141); // non-leaf corroboration
    put16(rom, 0x100, 0x4803); // ldr r0,[pc,#12] -> RAM-global literal
    put16(rom, 0x102, 0x6801); // ldr r1,[r0]
    put16(rom, 0x104, 0x2080); // movs r0,#128
    put16(rom, 0x106, 0x0100); // lsls r0,r0,#4
    put16(rom, 0x108, 0x6008); // str r0,[r1]
    put16(rom, 0x10A, 0x8088); // strh r0,[r1,#4]
    put16(rom, 0x10C, 0x4770); // bx lr
    put32(rom, 0x110, 0x03006538);
    put16(rom, 0x140, 0xB500); // push {lr}
    for (unsigned off = 0x142; off < 0x14A; off += 2) put16(rom, off, 0x2000);
    put16(rom, 0x14A, 0xBD00); // pop {pc}
    if (fault == 1) put32(rom, 0x20, 0); // unreferenced object
    if (fault == 2) put32(rom, 0x1C4, 0); // only one callback
    if (fault == 3) put32(rom, 0x1C4, kBase + 0x101); // repeated pointer
    if (fault == 4) put16(rom, 0x106, 0xE000); // branch in the leaf
    if (fault == 5) put32(rom, 0x110, kBase + 0x200); // asset literal, not RAM
    if (fault == 6) put16(rom, 0x10C, 0x4700); // bx r0, not a leaf return
    if (fault == 7) { put16(rom, 0x108, 0); put16(rom, 0x10A, 0); }
    if (fault == 8) put16(rom, 0x140, 0xB400); // anchor lacks saved LR
    if (fault == 9) put32(rom, 0x1C4, kBase + 0x140); // ARM-tagged pointer
    if (fault == 10) put16(rom, 0x104, 0x2480); // clobbers callee-saved r4
    if (fault >= 13 && fault <= 17) {
        put16(rom, 0x160, 0x4901); // separately referenced setter
        put16(rom, 0x162, 0x6809);
        put16(rom, 0x164, fault == 15 ? 0 : 0x7088);
        put16(rom, 0x166, 0x4770);
        put32(rom, 0x168, fault == 14 ? 0x03006666 : 0x03006538);
        if (fault != 16) put32(rom, 0x24, kBase + 0x161);
    }
    if (fault == 17) { // two non-leaf anchors do not corroborate a RAM global
        put16(rom, 0x100, 0xB500);
        put16(rom, 0x10C, 0xBD00);
    }
    if (fault == 18) put32(rom, 0x1C0, kBase + 0x181); // beyond executable end
    if (fault == 19) { // descriptor itself must be outside the executable span
        put32(rom, 0x20, kBase + 0x120);
        put32(rom, 0x120, kBase + 0x101);
        put32(rom, 0x124, kBase + 0x141);
    }
    gbarecomp::FunctionFinder finder(rom.data(), rom.size(), kBase);
    finder.set_speculative_literal_harvest(false);
    if (fault != 11) finder.set_aot_scan_range(kBase + 0x100, kBase + 0x180);
    if (fault == 12) finder.add_data_range(kBase + 0x100, kBase + 0x10E, "data");
    finder.add_seed({kBase, gbarecomp::CpuMode::Thumb, "entry"});
    finder.run(64);
    const bool leaf = has_function(finder, kBase + 0x100, gbarecomp::CpuMode::Thumb);
    const bool anchor = has_function(finder, kBase + 0x140, gbarecomp::CpuMode::Thumb);
    if (fault >= 13 && fault <= 17) {
        const bool setter = has_function(finder, kBase + 0x160, gbarecomp::CpuMode::Thumb);
        return leaf && anchor && setter == (fault == 13) &&
            finder.stats().aot_callback_seeds == (fault == 13 ? 3u : 2u);
    }
    return fault ? (!leaf && !anchor && finder.stats().aot_callback_seeds == 0)
                 : (leaf && anchor && finder.stats().aot_callback_seeds == 2 &&
                    finder.stats().aot_callback_objects == 1 && finder.collisions().empty());
}

static bool referenced_callback_prefix(unsigned fault) {
    std::vector<std::uint8_t> rom(0x500, 0);
    put16(rom,0,0x4770);
    put32(rom,0x20,kBase+0x300); // referenced object, within executable span
    auto leaf = [&](unsigned addr, uint32_t ram) {
        put16(rom,addr,0x480C); // first literal at +52
        put16(rom,addr+2,0x6001); // store without a frame
        for (unsigned n=4;n<50;n+=2) put16(rom,addr+n,0x2100);
        put16(rom,addr+50,0x4770); // 52-byte leaf, not a guessed end
        put32(rom,addr+52,ram);
    };
    leaf(0x100,0x03004000);leaf(0x140,0x03004010);leaf(0x240,0x03004000);
    for (unsigned addr : {0x180u,0x1C0u,0x200u,0x280u}) {
        put16(rom,addr,0xB500);
        for (unsigned n=2;n<10;n+=2) put16(rom,addr+n,0x2100);
        put16(rom,addr+10,0xBD00);
    }
    // Fixed four-record witness; NULL/NULL is a complete optional record.
    // No claimed array extent and no direct reference to any interior pair.
    put32(rom,0x308,kBase+0x101);put32(rom,0x30C,kBase+0x181);
    put32(rom,0x310,kBase+0x141);put32(rom,0x314,kBase+0x1C1);
    put32(rom,0x318,kBase+0x101);put32(rom,0x31C,kBase+0x201);
    // Independently located record: only the corroborated RAM global links it.
    put32(rom,0x380,kBase+0x241);put32(rom,0x384,kBase+0x281);
    if (fault==1) put32(rom,0x20,0); // unreferenced prefix
    if (fault==2) put32(rom,0x310,kBase+0x101); // one distinct leaf
    if (fault==3) { put32(rom,0x314,kBase+0x181);put32(rom,0x31C,kBase+0x181); }
    if (fault==4) put32(rom,0x300,kBase+0x101); // incomplete optional record
    if (fault==5) put16(rom,0x110,0xE000); // branch inside leaf
    if (fault==6) put16(rom,0x110,0x2400); // callee-saved register clobber
    if (fault==7) put32(rom,0x174,kBase+0x400); // asset, not RAM literal
    if (fault==8) put16(rom,0x1C0,0xB400); // partner did not save LR
    if (fault==9) put32(rom,0x310,kBase+0x140); // wrong mode tag
    if (fault==10) put16(rom,0x172,0x4700); // does not return through LR
    if (fault==11) put32(rom,0x274,0x03004020); // uncorroborated late record
    if (fault==12) put16(rom,0x280,0xB400); // late record lacks frame partner
    if (fault==14 || fault==15) {
        leaf(0x2C0,0x03004000);put32(rom,0x30C,kBase+0x2C1); // optional frameless updater
        if (fault==15) put16(rom,0x2F2,0x4700); // updater must also return through LR
    }
    gbarecomp::FunctionFinder finder(rom.data(),rom.size(),kBase);
    finder.set_speculative_literal_harvest(false);
    finder.set_aot_scan_range(kBase+0x100,kBase+0x500);
    if (fault==13) finder.add_data_range(kBase+0x100,kBase+0x134,"asset");
    finder.add_seed({kBase,gbarecomp::CpuMode::Thumb,"entry"});finder.run(64);
    const bool prefix = fault==0 || fault==11 || fault==12 || fault==14;
    for (unsigned addr : {0x100u,0x140u,0x180u,0x1C0u,0x200u})
        if (has_function(finder,kBase+addr,gbarecomp::CpuMode::Thumb)!=(prefix && !(addr==0x180 && fault==14))) return false;
    // The existing independent-setter rule still accepts an odd referenced
    // leaf sharing the corroborated RAM global, even when its partner is bad.
    if (has_function(finder,kBase+0x240,gbarecomp::CpuMode::Thumb)!=(fault==0 || fault==12 || fault==14)) return false;
    if (has_function(finder,kBase+0x280,gbarecomp::CpuMode::Thumb)!=(fault==0 || fault==14)) return false;
    if (has_function(finder,kBase+0x2C0,gbarecomp::CpuMode::Thumb)!=(fault==14)) return false;
    return finder.collisions().empty();
}

static bool shared_ram_store_callbacks(unsigned fault) {
    std::vector<uint8_t> image(0x1200,0);
    put16(image,0,0x4770);
    put32(image,0x20,fault==6 ? 0 : kBase+0xA00);
    put32(image,0xA00,kBase+0x101);put32(image,0xA04,kBase+0x181);
    // An authoritative referenced descriptor anchors both actual store roots.
    put16(image,0x100,0x4803);put16(image,0x102,0x6001);
    put16(image,0x104,0x4A03);put16(image,0x106,fault==5 ? 0x6811 : 0x6011);
    put16(image,0x108,0x4770);
    put32(image,0x110,0x03004000);put32(image,0x114,0x03005000);
    put16(image,0x180,0xB500);
    for(unsigned at=0x182;at<0x18A;at+=2)put16(image,at,0x2100);
    put16(image,0x18A,0xBD00);
    // The framed callback reads another global, but stores the anchored root.
    put32(image,0x24,kBase+(fault==4 ? 0x400 : 0x401));
    put16(image,0x400,0xB510);put16(image,0x402,0x4B07);
    put16(image,0x404,fault==7 ? 0x2500 : 0x4806);
    put16(image,0x406,0x6800);
    put16(image,0x408,fault==3 ? 0x681B : fault==12 ? 0x3304 : 0x2100);
    put16(image,0x40A,fault==2 ? 0x8818 : 0x8058);
    put16(image,0x40C,fault==8 ? 0xB081 : 0x2100);
    put16(image,0x40E,0xBC10);put16(image,0x410,0xBC01);put16(image,0x412,0x4700);
    put32(image,0x420,fault==1 ? 0x03007000 : fault==10 ? kBase+0xB00 : 0x03005000);
    // A leaf reads a camera global first, then writes a different shared root.
    put16(image,0x500,0x4A07);put16(image,0x502,0x6810);
    put16(image,0x504,0x4907);put16(image,0x506,0x8188);
    put16(image,0x508,fault==9 ? 0x4700 : 0x4770);
    put32(image,0x520,0x03006000);put32(image,0x524,0x03005000);
    // Only the accepted first field can give this exact paired no-op its type.
    put32(image,0xB00,kBase+0x501);put32(image,0xB04,kBase+(fault==13 ? 0x600 : 0x601));
    put16(image,0x600,0x4770);
    gbarecomp::FunctionFinder finder(image.data(),image.size(),kBase);
    finder.set_speculative_literal_harvest(false);
    finder.set_aot_scan_range(kBase+0x100,kBase+(fault==11 ? 0x400 : 0x900));
    finder.add_seed({kBase,gbarecomp::CpuMode::Thumb,"entry"});finder.run(128);
    const bool anchored=fault!=5 && fault!=6 && fault!=11;
    const bool frame=anchored && (fault==0 || fault==9 || fault==12 || fault==13);
    const bool leaf=anchored && fault!=9;
    return has_function(finder,kBase+0x400,gbarecomp::CpuMode::Thumb)==frame &&
        has_function(finder,kBase+0x500,gbarecomp::CpuMode::Thumb)==leaf &&
        has_function(finder,kBase+0x600,gbarecomp::CpuMode::Thumb)==(leaf && fault!=13) &&
        finder.collisions().empty();
}

static bool branch_callbacks_preserve_existing_budget() {
    // A bounded extension must not evict a real callback near the end of the
    // existing call-site budget. A full byte-domain workload and 256 extra
    // branch paths must not starve the final directly known selector argument.
    auto run=[](bool branches) {
        std::vector<uint8_t> image(0x6000,0);
        auto bl=[&](unsigned pc,unsigned target) {
            const uint32_t delta=target-(pc+4u);
            put16(image,pc,uint16_t(0xF000u|((delta>>12)&0x7FFu)));
            put16(image,pc+2,uint16_t(0xF800u|((delta>>1)&0x7FFu)));
        };
        if (branches) {
            for (unsigned i=0;i<256;++i) {
                const unsigned pc=0x100+6*i;
                put16(image,pc,0xB500);put16(image,pc+2,uint16_t(0x2700|i));
                put16(image,pc+4,uint16_t(0xE000|((0x800-(pc+8))/2)));
            }
            put16(image,0x800,0x1C38);bl(0x802,0x3000);
            put16(image,0x806,0xBC01);put16(image,0x808,0x4700);
            put16(image,0x3000,0x4770);
        }
        unsigned at=0x1000;
        for (unsigned i=0;i<256;++i) {
            for (unsigned r=0;r<4;++r) {
                put16(image,at,uint16_t(0x7820|(r<<6)|r));at+=2;
            }
            bl(at,0x4000+2*i);at+=4;put16(image,0x4000+2*i,0x4770);
        }
        put16(image,at,0x2001);at+=2;bl(at,0x5000);at+=4;put16(image,at,0x4770);
        put16(image,0x5000,0x0080);put16(image,0x5002,0x4903);
        put16(image,0x5004,0x5808);put16(image,0x5006,0x4903);
        put16(image,0x5008,0x6008);put16(image,0x500A,0x4770);
        put32(image,0x5010,kBase+0x5200);put32(image,0x5014,0x03001000);
        put32(image,0x5204,kBase+0x5101);
        put16(image,0x5100,0xB500);
        for (unsigned pc=0x5102;pc<0x510A;pc+=2)put16(image,pc,0x2000);
        put16(image,0x510A,0xBD00);
        gbarecomp::FunctionFinder finder(image.data(),image.size(),kBase);
        finder.set_speculative_literal_harvest(false);finder.set_aot_scan_range(kBase,kBase+image.size());
        if (branches) for(unsigned i=0;i<256;++i)
            finder.add_seed({kBase+0x100+6*i,gbarecomp::CpuMode::Thumb,"branch-predecessor"});
        finder.add_seed({kBase+0x1000,gbarecomp::CpuMode::Thumb,"legacy-calls"});finder.run(2048);
        return has_function(finder,kBase+0x5100,gbarecomp::CpuMode::Thumb) && finder.collisions().empty();
    };
    return run(false) && run(true);
}

static bool callback_constants_at_branch_joins(unsigned fault) {
    std::vector<uint8_t> image(0x1200,0);
    auto bl=[&](unsigned pc,unsigned target) {
        const uint32_t delta=target-(pc+4u);
        put16(image,pc,uint16_t(0xF000u|((delta>>12)&0x7FFu)));
        put16(image,pc+2,uint16_t(0xF800u|((delta>>1)&0x7FFu)));
    };
    put16(image,0,0xB590);put16(image,2,0x2800);put16(image,4,0xD00C);
    put16(image,6,fault==2 ? 0x1C07 : 0x270C);put16(image,8,0xE01A);
    put16(image,0x20,fault==2 ? 0x1C07 : 0x270E);put16(image,0x22,0xE00D);
    // Two distinct path-local R7 constants join before a bounded four-step loop.
    put16(image,0x40,fault==1 ? 0x1C07 : fault==5 ? 0xDF00 : 0x2400);
    put16(image,0x42,0x3401);put16(image,0x44,0x2C03);put16(image,0x46,0xD9FC);
    put16(image,0x48,0x1C39);bl(0x4A,0x400);
    put16(image,0x4E,0xBC90);put16(image,0x50,0xBC01);put16(image,0x52,0x4700);
    put16(image,0x400,0x0089);put16(image,0x402,0x4A04);
    put16(image,0x404,0x1851);put16(image,0x406,0x6808);
    put16(image,0x408,0x4A03);put16(image,0x40A,0x6010);put16(image,0x40C,0x4770);
    put32(image,0x414,kBase+0x800);put32(image,0x418,0x03001000);
    put32(image,0x830,kBase+(fault==3 ? 0xA00 : 0xA01));
    put32(image,0x838,kBase+(fault==3 ? 0xB00 : 0xB01));
    for(unsigned pc:{0xA00u,0xB00u}) {
        put16(image,pc,0xB500);
        for(unsigned at=pc+2;at<pc+10;at+=2)put16(image,at,0x2000);
        put16(image,pc+10,0xBD00);
    }
    gbarecomp::FunctionFinder finder(image.data(),image.size(),kBase);
    finder.set_speculative_literal_harvest(false);
    if(fault!=4)finder.set_aot_scan_range(kBase,kBase+(fault==6 ? 0xB00 : 0x1200));
    if(fault==7)finder.add_data_range(kBase+0x830,kBase+0x83C,"callback-data");
    finder.add_seed({kBase,gbarecomp::CpuMode::Thumb,"branch-join"});finder.run(128);
    const bool valid=fault==0 || fault==6 || fault==7;
    return has_function(finder,kBase+0xA00,gbarecomp::CpuMode::Thumb)==valid &&
        has_function(finder,kBase+0xB00,gbarecomp::CpuMode::Thumb)==(valid && fault!=6) &&
        finder.collisions().empty();
}

static bool masked_selector_at_branch_joins(unsigned fault) {
    std::vector<uint8_t> image(0x1400,0);
    put16(image,0,0xB510);put16(image,2,fault==10 ? 0x6819 : 0x6818);
    const unsigned mask=fault==1 ? 3u : fault==2 ? 255u : fault==3 ? 0u : 10u;
    put16(image,4,fault==4 ? 0x6819 : uint16_t((fault==10 ? 0x2000 : 0x2100)|mask));
    put16(image,6,0x4A06);put16(image,8,0xE01A);put32(image,0x20,fault==5 ? 0x02001000 : kBase+0x800);
    if(fault==11 || fault==12) {
        // Inline LCG: MUL operands are IR Rm/Rs, not data-processing Op2.
        put16(image,2,fault==12 ? 0x490A : 0x6819);
        put16(image,4,0x4807);put16(image,6,0x4348);put16(image,8,0x4907);
        put16(image,10,0x1840);put16(image,12,0x0C00);put16(image,14,0x210A);
        put16(image,16,0x4A03);put16(image,18,0xE015);
        put32(image,0x24,0x00196225);put32(image,0x28,0x3C6EF35F);put32(image,0x2C,1);
    }
    put16(image,0x40,0x4008);put16(image,0x42,0x0080);put16(image,0x44,0x1880);
    put16(image,0x46,0x6801);put16(image,0x48,0xE01A);
    put16(image,0x80,fault==6 ? 0x6819 : 0x1C20);
    put16(image,0x82,0xF000);put16(image,0x84,0xF9BD);
    put16(image,0x86,0xBC10);put16(image,0x88,0xBC01);put16(image,0x8A,0x4700);
    put16(image,0x400,0x0089);put16(image,0x402,0x4A04);put16(image,0x404,0x1851);
    put16(image,0x406,0x6808);put16(image,0x408,0x4A03);put16(image,0x40A,0x6010);put16(image,0x40C,0x4770);
    put32(image,0x414,kBase+0x900);put32(image,0x418,0x03001000);
    for(unsigned i=0;i<=10;++i)put32(image,0x800+i*4,48); // impossible mask values point at the fifth callback
    const unsigned indices[4]={0,fault==1 ? 1u : 2u,fault==1 ? 2u : 8u,fault==1 ? 3u : 10u};
    const unsigned states[5]={6,14,20,32,48};
    for(unsigned i=0;i<4;++i)put32(image,0x800+indices[i]*4,states[i]);
    for(unsigned i=0;i<5;++i) {
        const unsigned pc=0xA00+i*0x40;
        put32(image,0x900+states[i]*4,kBase+pc+(fault==8 ? 0u : 1u));
        put16(image,pc,0xB500);for(unsigned at=pc+2;at<pc+10;at+=2)put16(image,at,0x2100);
        put16(image,pc+10,0xBD00);
    }
    gbarecomp::FunctionFinder finder(image.data(),image.size(),kBase);
    finder.set_speculative_literal_harvest(false);
    if(fault!=7)finder.set_aot_scan_range(kBase,kBase+(fault==9 ? 0xAC0 : 0x1400));
    finder.add_seed({kBase,gbarecomp::CpuMode::Thumb,"masked-state-selector"});finder.run(128);
    const bool valid=fault==0 || fault==1 || fault==3 || fault==9 || fault==10 || fault==11 || fault==12;
    for(unsigned i=0;i<4;++i) {
        const bool expected=valid && (fault!=3 || i==0) && (fault!=9 || i<3) && (fault!=12 || i==2);
        if(has_function(finder,kBase+0xA00+i*0x40,gbarecomp::CpuMode::Thumb)!=expected)return false;
    }
    return !has_function(finder,kBase+0xB00,gbarecomp::CpuMode::Thumb) && finder.collisions().empty();
}

static bool callbacks_across_proved_calls(unsigned fault) {
    std::vector<std::uint8_t> image(0x2400,0);
    auto bl = [&](unsigned pc,unsigned target) {
        const int32_t delta=int32_t(target)-int32_t(pc+4u);
        put16(image,pc,uint16_t(0xF000u|((uint32_t(delta)>>12u)&0x7FFu)));
        put16(image,pc+2u,uint16_t(0xF800u|((uint32_t(delta)>>1u)&0x7FFu)));
    };
    put16(image,0,0x2103);bl(2,0x100);put16(image,6,0x4770);
    if(fault>=7) put16(image,0,fault==8 ? 0x8811 : fault==9 ? 0x5611 : 0x7811);
    if(fault==10) {put16(image,2,0x6819);bl(4,0x100);put16(image,8,0x4770);}
    put16(image,0x100,0xB510);put16(image,0x102,0x1C0C); // PUSH r4,lr; MOV r4,r1
    bl(0x104,0x500);
    put16(image,0x108,0xE0FA); // branch to +300, beyond old128-byte lookahead
    put16(image,0x300,0x1C21); // MOV r1,r4
    if(fault==4) put16(image,0x300,0x6811); // unknown load destroys the argument
    bl(0x302,0x600);
    put16(image,0x306,0xBC10);put16(image,0x308,0xBC01);put16(image,0x30A,0x4700);
    // The helper actually clobbers r4, then restores its saved incoming value.
    put16(image,0x500,0xB510);put16(image,0x502,0x2400);
    put16(image,0x504,0xBC10);put16(image,0x506,0xBC01);put16(image,0x508,0x4700);
    if(fault==1) { // does not preserve r4
        put16(image,0x500,0x2400);put16(image,0x502,0x4770);
    }
    if(fault==2) put16(image,0x500,0x4708); // unproved indirect transfer
    if(fault==3) { // explicitly overwrites the saved stack value
        put16(image,0x502,0x9000);
    }
    if(fault==5) { // frameless callee preserves the argument too
        put16(image,0x500,0x4770);
    }
    if(fault==6) { // recursive effect proof must not assume convergence/preservation
        bl(0x500,0x500);put16(image,0x504,0x4770);
    }
    if(fault==11) {put16(image,0x508,0xB001);put16(image,0x50A,0x4700);} // unbalanced SP
    if(fault==12 || fault==13) {
        put16(image,0x500,fault==12 ? 0xB510 : 0xB500);bl(0x502,0x5A0);
        put16(image,0x506,fault==12 ? 0xBC10 : 0xBC01);
        put16(image,0x508,fault==12 ? 0xBC01 : 0x4700);put16(image,0x50A,0x4700);
        put16(image,0x5A0,0x2400);put16(image,0x5A2,0x4770);
    }
    if(fault==14) put16(image,0x500,0xDF00); // never assume SWI effects
    if(fault==15) { // SP alias overwrites the saved r4 value
        put16(image,0x502,0x466A);put16(image,0x504,0x6010);
        put16(image,0x506,0xBC10);put16(image,0x508,0xBC01);put16(image,0x50A,0x4700);
    }
    if(fault>=16 && fault<=20) { // Thumb entry into an ARM library helper
        put16(image,0x500,0x4778);put16(image,0x502,0x46C0);
        put32(image,0x504,0xE92D4010); // PUSH r4,lr
        put32(image,0x508,fault==19 ? 0xE58D0000 : fault==20 ? 0xE8B10010 : 0xE3A04000);
        put32(image,0x50C,0xE8BD4010);put32(image,0x510,0xE12FFF1E);
        if(fault==17 || fault==18) {
            put32(image,0x504,fault==18 ? 0x03A04000 : 0xE3A04000);
            put32(image,0x508,0xE12FFF1E);
        }
    }
    if(fault==21) { // recursive calls with changing arguments need a depth bound
        put16(image,0x500,0x3101);bl(0x502,0x500);put16(image,0x506,0x4770);
    }
    put16(image,0x600,0x0088);put16(image,0x602,0x1840); // index*5
    put16(image,0x604,0x0080);put16(image,0x606,0x4A02); // index*20; LDR table
    put16(image,0x608,0x1880);put16(image,0x60A,0x6800);
    put16(image,0x60C,0x6018);put16(image,0x60E,0x4770); // callback store; BX LR
    put32(image,0x610,kBase+0x900);
    put32(image,0x93C,kBase+0x801);
    if(fault==9) {put32(image,0x93C,0);put32(image,0x8EC,kBase+0x801);} // signed -1 only
    put32(image,0x1D00,kBase+0x881); // index256 is outside either byte domain
    put16(image,0x800,0xB500);
    for(unsigned n=2;n<10;n+=2)put16(image,0x800+n,0x2000);
    put16(image,0x80A,0xBD00);
    put16(image,0x880,0xB500);
    for(unsigned n=2;n<10;n+=2)put16(image,0x880+n,0x2000);
    put16(image,0x88A,0xBD00);
    gbarecomp::FunctionFinder finder(image.data(),image.size(),kBase);
    finder.set_speculative_literal_harvest(false);finder.set_aot_scan_range(kBase,kBase+0x2400);
    finder.add_seed({kBase,gbarecomp::CpuMode::Thumb,"caller"});finder.run(128);
    return has_function(finder,kBase+0x800,gbarecomp::CpuMode::Thumb)==
        (fault==0 || fault==5 || fault==7 || fault==9 || fault==12 || fault==16 || fault==20) &&
        !has_function(finder,kBase+0x880,gbarecomp::CpuMode::Thumb);
}

static bool sparse_byte_word_callbacks(unsigned fault) {
    std::vector<uint8_t> image(0x1200,0);
    put16(image,0,0xB500);put16(image,2,0x4D0F);
    put16(image,4,fault==2 ? 0x56A1 : fault==3 ? 0x8821 : 0x7821);
    put16(image,6,fault==5 ? 0x0090 : 0x0088);put16(image,8,0x1940);
    put16(image,10,0x6800);put16(image,12,fault==1 ? 0xBC01 : 0x2800);
    put16(image,14,0xD001);put16(image,16,0xF000);put16(image,18,0xF826);
    put16(image,20,0xBD00);put16(image,0x60,fault==4 ? 0x4708 : 0x4700);
    put32(image,0x40,kBase+0x900);
    if(fault==6)put32(image,0x40,kBase+0x901); // no unaligned word-pointer proof
    put32(image,0x904,kBase+0x801);put32(image,0x91C,kBase+0x841);
    put32(image,0x8FC,kBase+0x881); // selected only by a signed byte index
    for(unsigned pc:{0x800u,0x840u,0x880u}) {
        put16(image,pc,0xB500);
        for(unsigned n=2;n<10;n+=2)put16(image,pc+n,0x2000);
        put16(image,pc+10,0xBD00);
    }
    gbarecomp::FunctionFinder finder(image.data(),image.size(),kBase);
    finder.set_speculative_literal_harvest(false);
    if(fault!=7)finder.set_aot_scan_range(kBase,kBase+(fault==8 ? 0x800u : 0x1200u));
    finder.add_seed({kBase,gbarecomp::CpuMode::Thumb,"byte-word-array"});finder.run(128);
    const bool valid=fault==0 || fault==2;
    return has_function(finder,kBase+0x800,gbarecomp::CpuMode::Thumb)==valid &&
        has_function(finder,kBase+0x840,gbarecomp::CpuMode::Thumb)==valid &&
        has_function(finder,kBase+0x880,gbarecomp::CpuMode::Thumb)==(fault==2);
}

static bool bios_irq_callback(unsigned fault) {
    std::vector<std::uint8_t> rom(0x100, 0);
    put32(rom, 0, 0xE59F1018); // ldr r1,[pc,#24] -> IRQ slot literal
    put32(rom, 4, 0xE28F0034); // adr r0,08000040; no frame prologue
    put32(rom, 8, 0xE5810000); // str r0,[r1]
    put32(rom, 12, 0xE12FFF1E); // bx lr
    put32(rom, 0x20, fault == 1 ? 0x03007FF8 : 0x03007FFC);
    for (unsigned off = 0x40; off < 0x5C; off += 4)
        put32(rom, off, 0xE1A00000); // mov r0,r0
    put32(rom, 0x40, 0xE3A03301); // mov r3,#04000000, like the retail IRQ
    put32(rom, 0x5C, 0xE12FFF1E);
    if (fault == 2) {
        put32(rom, 8, 0xE5920000); // unknown load clobbers the callback
        put32(rom, 12, 0xE5810000);
        put32(rom, 16, 0xE12FFF1E);
    }
    if (fault == 4) put32(rom, 4, 0xE28F0036); // misaligned ARM pointer
    if (fault == 5) put32(rom, 4, 0xE28F00F4); // beyond the image
    if (fault == 6) {
        put32(rom, 4, 0xE59F0010); // tagged Thumb callback from literal1C
        put32(rom, 0x1C, kBase + 0x41);
        for (unsigned off = 0x40; off < 0x4A; off += 2)
            put16(rom, off, 0x46C0); // nop, still no PUSH-LR
        put16(rom, 0x4A, 0x4770); // bx lr
    }
    if (fault == 7) put32(rom, 8, 0xE1A00000); // address-taking alone
    if (fault == 8) put32(rom, 8, 0xE5C10000); // byte store is not a vector
    if (fault == 9) put32(rom, 8, 0xE4810004); // unsupported writeback
    gbarecomp::FunctionFinder finder(rom.data(), rom.size(), kBase);
    finder.set_speculative_literal_harvest(false);
    if (fault == 3) finder.add_data_range(kBase + 0x40, kBase + 0x60, "data");
    finder.add_seed({kBase, gbarecomp::CpuMode::Arm, "entry"});
    finder.run(64);
    const auto mode = fault == 6 ? gbarecomp::CpuMode::Thumb : gbarecomp::CpuMode::Arm;
    return has_function(finder, kBase + 0x40, mode) == (fault == 0 || fault == 6);
}

static bool bounded_callback_copy(unsigned fault) {
    std::vector<std::uint8_t> rom(0x240, 0);
    const uint16_t code[] = {
        0x2200, 0x4C0F, 0x4B0F, // zero counter, RAM destination, ROM source
        0x0411, 0x1409, 0x0088, // signed16 counter, word offset
        0x1902, 0x18C0, 0x6800, // form both element addresses; load word
        0x6010, 0x3101, 0x0409, // store it; increment counter
        0x0C0A, 0x1409, 0x2904, // narrow16; signed bound4
        0xDDF2, 0x4770,         // BLE loop; BX LR
    };
    for (unsigned i = 0; i < sizeof(code)/sizeof(code[0]); ++i)
        put16(rom, 2*i, code[i]);
    put32(rom, 0x40, 0x03001000);
    put32(rom, 0x44, kBase + 0x100);
    put32(rom, 0x100, 0x03000441); // RAM callback must not truncate the table
    put32(rom, 0x104, kBase + 0x181);
    put32(rom, 0x108, kBase + 0x1A1);
    put32(rom, 0x10C, 0);          // null inside the proved copy extent
    put32(rom, 0x110, kBase + 0x1C1);
    put32(rom, 0x114, kBase + 0x1E1); // adjacent pointer is NOT copied
    for (unsigned off : {0x180u, 0x1A0u, 0x1C0u, 0x1E0u}) {
        put16(rom, off, off == 0x1C0 ? 0x2007 : 0xB500);
        for (unsigned i = 2; i < 10; i += 2) put16(rom, off+i, 0x46C0);
        put16(rom, off+10, off == 0x1C0 ? 0x4770 : 0xBD00);
    }
    if (fault == 1) put16(rom, 0, 0x1C3A); // unknown incoming counter
    if (fault == 2) put16(rom, 0x12, 0x2000); // loaded value clobbered, no copy
    if (fault == 3) put16(rom, 0x12, 0x6011); // store counter, not loaded word
    if (fault == 4) put16(rom, 0x12, 0x7010); // byte store
    if (fault == 5) put32(rom, 0x44, 0x02001000); // unknown RAM source
    if (fault == 6) put32(rom, 0x40, kBase + 0x140); // non-RAM destination
    if (fault == 7) put16(rom, 0x14, 0x3100); // loop does not terminate
    if (fault == 9) put16(rom, 0x1A0, 0x46C0); // no second prologue witness
    if (fault == 11) put16(rom, 0x10, 0x8800); // halfword load
    if (fault == 12) put16(rom, 0x1C, 0x2903); // leaf lies beyond exact copy count
    if (fault == 14) put16(rom, 0x14, 0x3102); // non-contiguous transfer
    if (fault == 15) put16(rom, 0x16, 0xE000); // branch inside body
    if (fault == 16) put32(rom, 0x44, kBase + 0x230); // copy exceeds image
    if (fault == 17) put32(rom, 0x44, kBase + 0x101); // unaligned source
    if (fault == 18) put32(rom, 0x40, 0x03007FF8); // destination crosses IRQ vector
    gbarecomp::FunctionFinder finder(rom.data(), rom.size(), kBase);
    finder.set_speculative_literal_harvest(false);
    if (fault != 13) finder.set_aot_scan_range(kBase+0x180,
        kBase+(fault == 10 ? 0x1C0 : 0x240));
    if (fault == 8) finder.add_data_range(kBase+0x180, kBase+0x1B0, "data");
    finder.add_seed({kBase, gbarecomp::CpuMode::Thumb, "copy_callbacks"});
    finder.run(64);
    const bool valid = fault == 0 || fault == 10 || fault == 12;
    return has_function(finder, kBase+0x180, gbarecomp::CpuMode::Thumb) == valid &&
           has_function(finder, kBase+0x1A0, gbarecomp::CpuMode::Thumb) == valid &&
           has_function(finder, kBase+0x1C0, gbarecomp::CpuMode::Thumb) == (fault == 0) &&
           !has_function(finder, kBase+0x1E0, gbarecomp::CpuMode::Thumb);
}

static bool executable_span_callback_record(unsigned fault) {
    std::vector<std::uint8_t> rom(0x240, 0);
    put16(rom, 0, 0x480F); // referenced pointer array, not the record directly
    put16(rom, 2, 0x6801); // load record pointer; no branch consuming it here
    put16(rom, 4, 0x4770);
    put32(rom, 0x40, kBase + 0x80);
    put32(rom, 0x80, fault == 5 ? 0 : kBase + 0x100);
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned off = 0x180 + 0x20*i;
        put32(rom, 0x100+4*i, kBase+off+1);
        put16(rom, off, 0xB500);
        for (unsigned n = 2; n < 10; n += 2) put16(rom, off+n, 0x46C0);
        put16(rom, off+10, 0xBD00);
    }
    if (fault == 1) put32(rom, 0x10C, 0x03000001); // non-ROM field
    if (fault == 2) put32(rom, 0x10C, kBase+0x181); // repeated prologue is not independent evidence
    if (fault == 3) put16(rom, 0x1C0, 0x46C0); // only three prologues
    if (fault == 6) put32(rom, 0x80, kBase+0x101); // unaligned object
    if (fault == 7 || fault == 8) {
        put32(rom, 0x108, 0); // ordinary two-entry data record
        put32(rom, 0x10C, 0);
    }
    gbarecomp::FunctionFinder finder(rom.data(), rom.size(), kBase);
    finder.set_speculative_literal_harvest(false);
    finder.set_aot_scan_range(kBase, kBase+(fault == 9 ? 0x1E0 : 0x240));
    if (fault == 4) finder.add_data_range(kBase+0x1C0, kBase+0x1CC, "not code");
    if (fault == 7 || fault == 8)
        finder.add_data_range(kBase+0x100, kBase+(fault == 7 ? 0x108 : 0x104), "record data");
    finder.add_seed({kBase, gbarecomp::CpuMode::Thumb, "nested_record"});
    finder.run(64);
    for (unsigned i = 0; i < 4; ++i) {
        const bool expected = fault == 0 || (fault == 7 && i < 2);
        if (has_function(finder, kBase+0x180+0x20*i, gbarecomp::CpuMode::Thumb) != expected)
            return false;
    }
    return finder.collisions().empty();
}

static void put_thumb_call(std::vector<uint8_t>& rom, unsigned at, unsigned target) {
    const uint32_t delta = uint32_t(int32_t(target)-int32_t(at+4));
    put16(rom, at, uint16_t(0xF000u | ((delta >> 12) & 0x7FFu)));
    put16(rom, at+2, uint16_t(0xF800u | ((delta >> 1) & 0x7FFu)));
}

static bool constant_argument_callbacks(unsigned fault) {
    std::vector<uint8_t> rom(0x500, 0);
    put16(rom, 0, 0xB500);
    put16(rom, 2, 0x2101); put_thumb_call(rom, 4, 0x80);
    put16(rom, 8, 0x2102); put_thumb_call(rom, 0xA, 0x80);
    put16(rom, 0xE, 0x2100); put_thumb_call(rom, 0x10, 0xC0);
    put16(rom, 0x14, 0xBD00);
    const uint16_t getter[] = {0xB510,0x4A2F,0x1C09,0x0048,0x1840,0x0080,
        0x3204,0x1880,0x6803,0x2B00,0xD003,0,0,0xE001,0x46C0,0x2001,0xBC10,0xBC02,0x4708};
    for (unsigned i=0;i<sizeof(getter)/sizeof(getter[0]);++i) put16(rom,0x80+2*i,getter[i]);
    put_thumb_call(rom, 0x96, 0x1E0);
    const uint16_t setter[] = {0xB510,0x1C04,0x4B1F,0x004A,0x1852,0x0092,
        0x18D2,0x6811,0x6021,0xBC10,0xBC01,0x4700};
    for (unsigned i=0;i<sizeof(setter)/sizeof(setter[0]);++i) put16(rom,0xC0+2*i,setter[i]);
    put32(rom,0x140,kBase+0x200); put32(rom,0x144,kBase+0x200);
    put16(rom,0x1E0,0x4718); // BX r3 veneer
    put16(rom,0x1E8,0xB500); put16(rom,0x1EA,0xBD00);
    const unsigned callbacks[] = {0x300,0,0,0x320,0x340,1,0x360,0x380,2};
    for (unsigned i=0;i<9;++i) put32(rom,0x200+4*i,
        callbacks[i]>=0x300 ? kBase+callbacks[i]+1 : callbacks[i]);
    const uint16_t leaf[] = {0x2204,0x1881,0x7809,0x0049,0x3010,0x1840,0x8800,0x4770};
    for (unsigned i=0;i<sizeof(leaf)/sizeof(leaf[0]);++i) put16(rom,0x300+2*i,leaf[i]);
    for (unsigned off : {0x320u,0x340u,0x360u,0x380u}) {
        put16(rom,off,0xB500);
        for (unsigned n=2;n<10;n+=2) put16(rom,off+n,0x2000);
        put16(rom,off+10,0xBD00);
    }
    if (fault==1) for (unsigned off : {2u,8u,0xEu}) put16(rom,off,0x1C39);
    if (fault==2) put16(rom,0x92,0x2300); // clobber loaded callback
    if (fault==3) put16(rom,0x92,0xBC08); // POP replaces callback
    if (fault==4) put16(rom,0x1E0,0x4770); // called helper is not a BX veneer
    if (fault==5) {put32(rom,0x210,0);put32(rom,0x21C,0);}
    if (fault==6) put16(rom,0x84,0x3101); // callee changes index, select actual new value
    if (fault==7) put32(rom,0x140,0x02001000); // mutable RAM reads stay unknown
    if (fault==8) put32(rom,0x210,0x09000001); // non-ROM callback
    if (fault==10) put16(rom,0xD0,0x7021); // partial pointer store
    if (fault==11) put16(rom,0x300,0x3401); // leaf clobbers callee-saved r4
    if (fault==12) put16(rom,0x30E,0xE7FE); // no return witness
    if (fault==13) put_thumb_call(rom,0x96,0x1E8); // opaque callee stops proof
    if (fault==14) {
        put32(rom,0x21C,kBase+0x400);
        put32(rom,0x400,0xE92D4000);
        for (unsigned n=4;n<20;n+=4) put32(rom,0x400+n,0xE1A00000);
        put32(rom,0x414,0xE8BD8000);
    }
    if (fault==16) {put16(rom,2,0x2100);put16(rom,8,0x2100);put16(rom,0xE,0x1C39);}
    if (fault==17) put16(rom,0x94,0xD103); // non-null path returns instead of calling
    if (fault==18) put16(rom,0xD0,0x2100); // loaded value never escapes
    if (fault==19) put32(rom,0x140,kBase+0x201); // no aligned static word read
    gbarecomp::FunctionFinder finder(rom.data(),rom.size(),kBase);
    finder.set_speculative_literal_harvest(false);
    if (fault!=15) finder.set_aot_scan_range(kBase,kBase+rom.size());
    if (fault==9) finder.add_data_range(kBase+0x340,kBase+0x34C,"not code");
    finder.add_seed({kBase,gbarecomp::CpuMode::Thumb,"constant_calls"});
    finder.run(128);
    const bool none = fault==1 || fault==15 || fault==16;
    const bool default_leaf = !none && fault!=10 && fault!=11 && fault!=12 && fault!=18;
    const bool pair_one = !none && !(fault>=2 && fault<=9) && fault!=13 && fault!=17 && fault!=19;
    const bool pair_two = !none && fault!=2 && fault!=3 && fault!=4 && fault!=5 &&
        fault!=7 && fault!=13 && fault!=17 && fault!=19;
    return has_function(finder,kBase+0x300,gbarecomp::CpuMode::Thumb)==default_leaf &&
        has_function(finder,kBase+0x320,gbarecomp::CpuMode::Thumb)==pair_one &&
        has_function(finder,kBase+0x340,gbarecomp::CpuMode::Thumb)==pair_one &&
        has_function(finder,kBase+0x360,gbarecomp::CpuMode::Thumb)==pair_two &&
        has_function(finder,kBase+0x380,gbarecomp::CpuMode::Thumb)==(pair_two && fault!=14) &&
        has_function(finder,kBase+0x400,gbarecomp::CpuMode::Arm)==(fault==14);
}

static bool stored_leaf_callback(unsigned fault) {
    std::vector<uint8_t> rom(0x100,0);
    put16(rom,0,0x4807); put16(rom,2,0x6020); put16(rom,4,0x4770);
    put32(rom,0x20,kBase+0x41);
    const uint16_t leaf[] = {0x2204,0x1881,0x7809,0x0049,0x3010,0x1840,0x8800,0x4770};
    for (unsigned i=0;i<sizeof(leaf)/sizeof(leaf[0]);++i) put16(rom,0x40+2*i,leaf[i]);
    if (fault==1) put16(rom,2,0x7020);
    if (fault==2) put16(rom,2,0x2000);
    if (fault==3) put16(rom,0x40,0x3401);
    if (fault==4) put16(rom,2,0x46C0);
    if (fault==5) {
        put16(rom,0x40,0x2002); put16(rom,0x42,0x2103); put16(rom,0x44,0x4348);
        for (unsigned off=0x46;off<0x64;off+=2) put16(rom,off,0x0000);
        put16(rom,0x64,0x4770);
    }
    if (fault==7) put32(rom,0x20,kBase+0x40);
    gbarecomp::FunctionFinder finder(rom.data(),rom.size(),kBase);
    finder.set_speculative_literal_harvest(false);
    finder.set_aot_scan_range(kBase,kBase+rom.size());
    if (fault==6) finder.add_data_range(kBase+0x40,kBase+0x50,"data");
    finder.add_seed({kBase,gbarecomp::CpuMode::Thumb,"publish_leaf"});
    finder.run(32);
    return has_function(finder,kBase+0x40,gbarecomp::CpuMode::Thumb)==(fault==0 || fault==5);
}

static bool literal_callback(unsigned fault) {
    std::vector<std::uint8_t> rom(0x100, 0);
    put16(rom, 0, 0x4800); // callback passed as an argument, no local BX consumer
    put16(rom, 2, 0x4770);
    put32(rom, 4, kBase + (fault == 1 ? 0x40 : 0x41));
    put16(rom, 0x40, fault == 2 ? 0x2000 : 0xB500);
    for (unsigned off = 0x42; off < 0x4A; off += 2) put16(rom, off, 0x2000);
    put16(rom, 0x4A, 0xBD00);
    gbarecomp::FunctionFinder finder(rom.data(), rom.size(), kBase);
    finder.set_speculative_literal_harvest(false);
    if (fault != 3) finder.set_aot_scan_range(kBase + 0x40, kBase + (fault == 5 ? 0x48 : 0x80));
    if (fault == 4) finder.add_data_range(kBase + 0x40, kBase + 0x4C, "data");
    finder.add_seed({kBase, gbarecomp::CpuMode::Thumb, "entry"});
    finder.run(64);
    return has_function(finder, kBase + 0x40, gbarecomp::CpuMode::Thumb) ==
        (fault == 0 || fault == 1 || fault == 3);
}

static bool copied_switch_source(bool adr, bool tagged) {
    std::vector<std::uint8_t> rom(0x200, 0);
    put16(rom, 0, adr ? 0xA03F : 0x4800); // referenced copy source at +0x100
    put16(rom, 2, 0x4770);
    put32(rom, 4, kBase + 0x100 + unsigned(tagged));
    put16(rom, 0x100, 0xB500); // push {lr}
    put16(rom, 0x102, 0x1C02); // mov r2,r0
    put16(rom, 0x104, 0x2801); // cmp r0,#1
    put16(rom, 0x106, 0xD81B); // bhi default
    put16(rom, 0x108, 0x0080); // lsl r0,r0,#2
    put16(rom, 0x10A, 0x4905); // literal names an absolute ROM table
    put16(rom, 0x10C, 0x1840);
    put16(rom, 0x10E, 0x6800);
    put16(rom, 0x110, 0x4687); // mov pc,r0
    put32(rom, 0x120, kBase + 0x124);
    put32(rom, 0x124, kBase + 0x180);
    put32(rom, 0x128, kBase + 0x190);
    for (unsigned pc : {0x140u, 0x180u, 0x190u}) {
        put16(rom, pc, 0x2001);
        put16(rom, pc + 2, 0xBD00);
    }
    gbarecomp::FunctionFinder finder(rom.data(), rom.size(), kBase);
    finder.set_speculative_literal_harvest(false);
    finder.add_seed({kBase, gbarecomp::CpuMode::Thumb, "entry"});
    finder.run(64);
    auto emitted = [&](uint32_t pc) {
        for (const auto& fn : finder.functions()) {
            if (pc >= fn.addr && pc < fn.end_addr) return true;
            for (const auto& span : fn.switch_blocks)
                if (pc >= span.start && pc < span.end) return true;
        }
        return false;
    };
    return emitted(kBase + 0x100) && emitted(kBase + 0x180) &&
           emitted(kBase + 0x190) && finder.stats().auto_jump_tables == 1;
}

static bool byte_record_callbacks(unsigned fault) {
    std::vector<std::uint8_t> rom(0x300,0);
    put16(rom,0,0xB500); put16(rom,2,0x4D0F);
    put16(rom,4,fault==1 ? 0x8821 : 0x7821); // byte index has 256 possible values
    put16(rom,6,fault==2 ? 0x0050 : 0x0048); // i*2 (or unrelated index)
    put16(rom,8,0x1840); put16(rom,10,0x00C0); // (i*2+i)*8 = i*24
    put16(rom,12,0x1D29); put16(rom,14,0x1840); // table + i*24 + field 4
    put16(rom,16,0x6800);
    put16(rom,18,fault==4 ? 0xBC01 : 0x2800); // compare preserves target; POP replaces it
    put16(rom,20,0xD00C); put16(rom,22,0xF000); put16(rom,24,0xF823);
    put16(rom,26,0xBD00); put16(rom,0x30,0xBD00);
    put32(rom,0x40,kBase+0x100);
    put16(rom,0x60,fault==3 ? 0x4708 : 0x4700);
    // Null first/intermediate slots do not terminate the finite-domain search.
    put32(rom,0x100+24+4,kBase+0x201);
    put32(rom,0x100+72+4,kBase+0x241);
    put32(rom,0x100+24+8,kBase+0x281); // unrelated field must not be read
    for (unsigned pc : {0x200u,0x240u,0x280u}) {
        put16(rom,pc,0xB500);
        for (unsigned n=2;n<10;n+=2) put16(rom,pc+n,0x2000);
        put16(rom,pc+10,0xBD00);
    }
    gbarecomp::FunctionFinder finder(rom.data(),rom.size(),kBase);
    finder.set_speculative_literal_harvest(false);
    finder.add_seed({kBase,gbarecomp::CpuMode::Thumb,"entry"});
    finder.run(64);
    return has_function(finder,kBase+0x200,gbarecomp::CpuMode::Thumb)==(fault==0) &&
        has_function(finder,kBase+0x240,gbarecomp::CpuMode::Thumb)==(fault==0) &&
        !has_function(finder,kBase+0x280,gbarecomp::CpuMode::Thumb);
}

static bool arm_backward_adr_table(bool copied,unsigned fault) {
    constexpr uint32_t ram=0x03000000u;
    const uint32_t live=copied ? ram : kBase;
    std::vector<uint8_t> image(0x200,0);
    put32(image,0x100,fault==1 ? 0xE2407088u : fault==4 ? 0x124F7088u : 0xE24F7088u);
    put32(image,0x104,fault==2 ? 0xE7976186u : 0xE7976106u);
    put32(image,0x108,fault==3 ? 0xE12FFF15u : 0xE12FFF16u);
    for (unsigned n=0;n<3;++n) {
        const auto offset=0x20u+n*0x20u;
        put32(image,0x80+n*4,live+offset);
        put32(image,offset,0xE3A00001u+n);
        put32(image,offset+4,0xE12FFF1Eu);
    }
    gbarecomp::FunctionFinder finder(image.data(),image.size(),kBase);
    finder.set_speculative_literal_harvest(false);
    if (copied) finder.add_code_copy(ram,kBase,uint32_t(image.size()),"ARM dispatcher and table");
    finder.add_seed({live+0x100,gbarecomp::CpuMode::Arm,"dispatcher"});
    finder.run(32);
    for (unsigned n=0;n<3;++n)
        if (has_function(finder,live+0x20u+n*0x20u,gbarecomp::CpuMode::Arm)!=(fault==0))
            return false;
    return finder.stats().auto_jump_tables==(fault==0 ? 1u : 0u);
}

static bool arm_sparse_preceding_table(bool copied,unsigned fault) {
    const uint32_t live=copied ? 0x03000000u : kBase;
    std::vector<uint8_t> image(0x200,0);
    put32(image,0x100,fault==1 ? 0xE1A00000u : 0xE92D4000u); // PUSH LR / NOP
    put32(image,0x104,0xE24F702Cu); // ADR r7,table at +E0
    put32(image,0x108,0xE7976106u); // LDR r6,[r7,r6,LSL#2]
    put32(image,0x10C,0xE12FFF16u); // BX r6
    for (unsigned n=0;n<3;++n) {
        const auto offset=0x20u+n*0x20u;
        put32(image,0xE0u+(n==2 ? 3u : n)*4u,live+offset);
        put32(image,offset,0xE3A00001u+n); put32(image,offset+4u,0xE12FFF1Eu);
    }
    if (fault==2) put32(image,0xFC,0x04000000u); // no complete pointer/null array
    if (fault==3) put32(image,0xFC,live+0x62u); // misaligned ARM target
    gbarecomp::FunctionFinder finder(image.data(),image.size(),kBase);
    finder.set_speculative_literal_harvest(false);
    if (copied) finder.add_code_copy(live,kBase,uint32_t(image.size()),"sparse ARM table");
    finder.add_seed({live+0x100,gbarecomp::CpuMode::Arm,"dispatcher"});
    finder.run(32);
    if (!has_function(finder,live+0x20,gbarecomp::CpuMode::Arm) ||
        !has_function(finder,live+0x40,gbarecomp::CpuMode::Arm) ||
        has_function(finder,live+0x60,gbarecomp::CpuMode::Arm)!=(fault==0)) return false;
    bool full_span=false;
    for (const auto& range : finder.data_ranges())
        full_span |= range.start==live+0xE0 && range.end==live+0x100;
    return full_span==(fault==0);
}

}  // namespace

// Actual GCC Thumb switch encodings, including the PC+4 value at an ADD pc
// whose address is 2 mod 4. Cases remain labels in their dispatcher host.
static bool thumb_switch(bool halfword, bool literal, bool manual,
                         unsigned fault = 0, bool bhi = false,
                         bool late_guard = false) {
    std::vector<std::uint8_t> rom(0x400, 0);
    put16(rom, 0, 0x2803);      // cmp r0,#3
    put16(rom, 2, 0xD21D);     // bcs default at +0x40
    if (bhi) {
        put16(rom, 0, 0x2802); // cmp r0,#2 (inclusive bound)
        put16(rom, 2, 0xD81D); // bhi default
    }
    put16(rom, 4, halfword ? 0x0041 : 0x46C0); // lsls r1,r0,#1 / nop
    put16(rom, 6, literal ? 0x4B16 : 0xA316);  // table at +0x60
    put16(rom, 8, 0x46C0);     // nop (ADD pc is 2 mod 4)
    put16(rom, 10, halfword ? 0x5A5B : 0x5C1B); // ldrh r3,[r3,r1] / ldrb
    put16(rom, 12, fault == 2 ? 0x009B : 0x005B); // lsl #2 / #1
    put16(rom, 14, fault == 3 ? 0x469F : 0x449F); // mov / add pc,r3
    if (literal) put32(rom, 0x60, kBase + 0x80);
    const unsigned table = literal ? 0x80 : 0x60;
    // Values exceed 255 in the halfword variant, proving full-width loads.
    const unsigned first = halfword ? 0x220 : 0xA0;
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned target = first + 4 * i;
        const unsigned value = (target - (fault == 10 ? 20 : 18)) / 2;
        if (halfword) put16(rom, table + 2 * i, value);
        else rom[table + i] = static_cast<std::uint8_t>(value);
        put16(rom, target, 0x2201 + i);  // movs r2,#case
        put16(rom, target + 2, 0x4770);  // bx lr
    }
    put16(rom, 0x40, 0x4770);
    if (fault == 1 || manual) put16(rom, 2, 0x46C0); // no guard
    if (fault == 4) { // invalid target in the table: reject atomically
        if (halfword) put16(rom, table, 0xFFFF);
        else rom[table] = static_cast<std::uint8_t>((table - 18) / 2);
    }
    if (fault == 5) put16(rom, 0, 0x2903); // guard compares wrong index
    if (fault == 6) put16(rom, 8, halfword ? 0x2100 : 0x2000); // clobber index
    if (fault == 7) { // a call between guard and table base invalidates the bound
        put16(rom, 4, 0xF000); // bl default
        put16(rom, 6, 0xF81C);
        put16(rom, 8, literal ? 0x4B15 : 0xA315); // reload table base after call
    }
    if (late_guard || fault == 8 || fault == 9) {
        put16(rom, 0, 0x0041); // lsls r1,r0,#1 before the guard
        put16(rom, 2, fault == 8 ? 0x2000 : fault == 9 ? 0xBC01 : 0x46C0);
        // A bound on a replacement r0 must not constrain the old r1.
        put16(rom, 4, 0x2803); // cmp r0,#3
        put16(rom, 6, 0xD21B); // bcs default
        put16(rom, 8, literal ? 0x4B15 : 0xA315);
    }
    if (fault == 10) {
        put16(rom, 0, 0x0041); // lsls r1,r0,#1
        put16(rom, 2, 0xF000); // a call can replace the unbounded source
        put16(rom, 4, 0xF81D);
        put16(rom, 6, 0x2803); // cmp r0,#3 after the call
        put16(rom, 8, 0xD21A); // bcs default
        put16(rom, 10, literal ? 0x4B15 : 0xA315);
        put16(rom, 12, 0x5A5B);
        put16(rom, 14, 0x005B);
        put16(rom, 16, 0x449F);
    }
    gbarecomp::FunctionFinder finder(rom.data(), rom.size(), kBase);
    finder.set_speculative_literal_harvest(false);
    finder.set_static_resume_all(true);
    if (manual) finder.add_thumb_relative_table(kBase + table,
        halfword ? 2 : 1, 3, kBase + 18);
    finder.add_seed({kBase, gbarecomp::CpuMode::Thumb, "switch_host"});
    finder.run(64);
    const auto* host = find_function(finder, kBase, gbarecomp::CpuMode::Thumb);
    if (!host || !finder.collisions().empty()) return false;
    if (fault) return host->switch_blocks.empty() && finder.auto_jump_tables().empty();
    if (finder.auto_jump_tables().size() != (manual ? 0u : 1u) ||
        host->switch_blocks.size() != 3) return false;
    for (unsigned i = 0; i < 3; ++i) {
        const auto target = kBase + first + 4 * i;
        if (has_function(finder, target, gbarecomp::CpuMode::Thumb) ||
            std::find(host->alias_entries.begin(), host->alias_entries.end(), target) ==
                host->alias_entries.end()) return false;
    }
    const auto body = gbarecomp::emit_function_body_str(
        *host, rom.data(), rom.size(), kBase, {});
    if (body.find("/* 08000060 ") != std::string::npos ||
        body.find("/* 08000080 ") != std::string::npos) return false;
    return true;
}

bool callback_table_transfer(bool thumb, unsigned fault = 0) {
    std::vector<std::uint8_t> rom(0x140, 0);
    put32(rom, 0x40, kBase + 0xA0);
    put32(rom, 0xA0, kBase + 0xC1);
    put32(rom, 0xA4, kBase + 0xE1);
    for (unsigned off : {0xC0u, 0xE0u}) {
        put16(rom, off, 0xB500);  // push {lr}
        for (unsigned i = 2; i < 10; i += 2) put16(rom, off+i, 0x2000);
        put16(rom, off+10, 0xBD00); // pop {pc}
    }
    if (thumb) {
        put16(rom, 0, 0x480F); // ldr r0, literal
        put16(rom, 2, 0x7811); // ldrb r1,[r2]
        put16(rom, 4, 0x0089); // lsl r1,#2
        put16(rom, 6, 0x1809); // add r1,r1,r0
        put16(rom, 8, 0x6808); // ldr r0,[r1]
        put16(rom, 10, 0xF000);
        put16(rom, 12, 0xF839); // bl 0x80 (bx r0 veneer)
        put16(rom, 14, 0x4770); // decoded partial BL target is not veneer
        put16(rom, 0x80, 0x4700);
        if (fault==1 || fault==2) {
            // The prefix's provisional target may coincidentally be BX r0;
            // the full BL target is an ordinary helper, not that veneer.
            put16(rom,14,fault==1 ? 0x4700 : 0x4770);
            put16(rom,0x80,0xB500);
            put16(rom,0x82,0xBD00);
        }
        if (fault==3) {
            put16(rom,8,0x6809);  // ldr r1,[r1]: asset pointer
            put16(rom,10,0xBC02); // pop {r1}: replace it with caller return PC
            put16(rom,12,0x4708); // bx r1 is a return, not the table consumer
        }
    } else {
        put32(rom, 0, 0xE59F1038); // ldr r1,literal
        put32(rom, 4, fault >= 7 ? 0xE0811000 : 0xE0811100); // index may arrive pre-scaled
        if (fault >= 9) put32(rom, 4, 0xE1A02000); // mov r2,r0: opaque byte offset
        put32(rom, 8, fault >= 9 ? 0xE7910002 : 0xE5910000); // indexed or folded word load
        const uint32_t between[] = {
            0xE92D4000, // stmdb sp!,{lr}: preserves loaded r0
            0xE3A00001, // mov r0,#1: clobbers target
            0xE8BD0001, // ldmia sp!,{r0}: clobbers target
            0xE5820000, // str r0,[r2]: reads r0, does not write it
            0xE3500000, // cmp r0,#0: reads r0
            0xEB00001B, // bl 0x80: unknown callee may clobber target
            0xE92D4000, // preserve target before non-MOV PC-write rejection
            0xE92D4000, // unknown index scale across an earlier block
            0xE92D4000, // same data load without a matching BX consumer
            0xE92D4000, // register-offset LDR with unknown earlier scale
            0xE92D4000, // register-offset data load with no branch consumer
        };
        put32(rom, 12, between[fault]);
        put32(rom, 16, 0xE28FE000); // add lr,pc,#0
        put32(rom, 20, fault == 6 ? 0xE08FF000 : (fault == 8 || fault == 10) ? 0xE12FFF1E : 0xE12FFF10);
        put32(rom, 0x80, 0xE12FFF1E);
    }
    gbarecomp::FunctionFinder finder(rom.data(), rom.size(), kBase);
    finder.set_speculative_literal_harvest(false);
    finder.add_seed({kBase, thumb ? gbarecomp::CpuMode::Thumb : gbarecomp::CpuMode::Arm, "entry"});
    finder.run(32);
    const bool expected = thumb ? fault==0 :
        fault == 0 || fault == 3 || fault == 4 || fault == 7 || fault == 9;
    return (finder.auto_jump_tables().size() == (expected ? 1u : 0u)) &&
           has_function(finder,kBase+0xC0,gbarecomp::CpuMode::Thumb) == expected &&
           has_function(finder,kBase+0xE0,gbarecomp::CpuMode::Thumb) == expected;
}

int main() {
    for (unsigned fault=0;fault<20;++fault) if (!constant_argument_callbacks(fault)) {
        std::fprintf(stderr,"Constant-argument callbacks failed: fault=%u\n",fault); return 1;
    }
    for (unsigned fault=0;fault<8;++fault) if (!stored_leaf_callback(fault)) {
        std::fprintf(stderr,"Stored leaf callback failed: fault=%u\n",fault); return 1;
    }
    for (unsigned fault = 0; fault < 10; ++fault) {
        if (!executable_span_callback_record(fault)) {
            std::fprintf(stderr, "Executable-span callback record failed: fault=%u\n", fault);
            return 1;
        }
    }
    for (unsigned fault = 0; fault < 19; ++fault) {
        if (!bounded_callback_copy(fault)) {
            std::fprintf(stderr, "Bounded callback copy discovery failed: fault=%u\n", fault);
            return 1;
        }
    }
    for (unsigned fault = 0; fault < 10; ++fault) {
        if (!bios_irq_callback(fault)) {
            std::fprintf(stderr, "BIOS IRQ callback discovery failed: fault=%u\n", fault);
            return 1;
        }
    }
    {
        std::vector<std::uint8_t> rom(4);
        put16(rom,0,0x2000);
        put16(rom,2,0x4770);
        gbarecomp::FunctionFinder finder(rom.data(),rom.size(),kBase);
        finder.add_seed({kBase,gbarecomp::CpuMode::Thumb,"entry"});
        finder.run(4);
        const auto* entry=find_function(finder,kBase,gbarecomp::CpuMode::Thumb);
        if (!entry || std::find(entry->alias_entries.begin(),entry->alias_entries.end(),kBase+2) == entry->alias_entries.end()) {
            std::fprintf(stderr,"Default finder missed interrupt/host-unwind resume boundary\n");
            return 1;
        }
    }
    for (unsigned fault=0;fault<=3;++fault) {
        if (!callback_table_transfer(true,fault)) {
            std::fprintf(stderr,"Thumb BL-pair callback confirmation failed: fault=%u\n",fault);
            return 1;
        }
    }
    for (unsigned fault = 0; fault < 11; ++fault) {
        if (!callback_table_transfer(false, fault)) {
            std::fprintf(stderr,"ARM callback target provenance failed: %u\n", fault);
            return 1;
        }
    }
    for (bool halfword : {false, true}) {
        for (bool literal : {false, true}) {
            for (unsigned fault = 0; fault <= 7; ++fault) {
                if (!thumb_switch(halfword, literal, false, fault)) {
                    std::fprintf(stderr, "Thumb switch failed: half=%d literal=%d fault=%u\n",
                                 halfword, literal, fault);
                    return 1;
                }
            }
            if (!thumb_switch(halfword, literal, true)) return 1;
            if (!thumb_switch(halfword, literal, false, 0, true)) return 1;
            if (halfword) {
                for (unsigned fault : {0u, 8u, 9u, 10u}) {
                    if (!thumb_switch(true, literal, false, fault, false, true)) {
                        std::fprintf(stderr, "Thumb halfword late guard failed: literal=%d fault=%u\n",
                                     literal, fault);
                        return 1;
                    }
                }
            }
        }
    }
    std::vector<std::uint8_t> rom(0x80, 0);

    // ldr r1,[pc,#8]  -> tracker sees the even ROM data pointer below
    // pop {r1}        -> r1 is overwritten from the runtime stack
    // bx r1           -> dynamic return; must not reuse the stale literal
    put16(rom, 0x00, 0x4902u);
    put16(rom, 0x02, 0xBC02u);
    put16(rom, 0x04, 0x4708u);
    put32(rom, 0x0C, kBase + 0x20u);

    // Make the false target valid-looking ARM code. Before the regression
    // fix, the stale constant caused this address to be emitted.
    put32(rom, 0x20, 0xE12FFF1Eu);  // bx lr

    gbarecomp::FunctionFinder finder(rom.data(), rom.size(), kBase);
    finder.set_speculative_literal_harvest(false);
    finder.add_seed({kBase, gbarecomp::CpuMode::Thumb, "pop_bx_return"});
    finder.run(32);

    if (!has_function(finder, kBase, gbarecomp::CpuMode::Thumb)) {
        std::fprintf(stderr, "entry function was not discovered\n");
        return 1;
    }
    if (has_function(finder, kBase + 0x20u, gbarecomp::CpuMode::Arm)) {
        std::fprintf(stderr,
                     "POP destination retained a stale constant and seeded "
                     "0x%08X as ARM code\n",
                     kBase + 0x20u);
        return 1;
    }

    // A stack-local helper may be copied again at an overlapping address.
    // The explicit source address must win over the first fixed mapping, and
    // CFG roots from the older placement must not truncate the newer body.
    std::vector<std::uint8_t> relocated_rom(0x80, 0);
    constexpr std::size_t kTemplate = 0x40;
    put16(relocated_rom, kTemplate + 0x00, 0x2800u); // cmp r0,#0
    put16(relocated_rom, kTemplate + 0x02, 0xD001u); // beq +8
    put16(relocated_rom, kTemplate + 0x04, 0x4770u); // bx lr
    put16(relocated_rom, kTemplate + 0x06, 0x46C0u); // nop
    put16(relocated_rom, kTemplate + 0x08, 0x4770u); // bx lr

    constexpr std::uint32_t kRam = 0x03000000u;
    gbarecomp::FunctionFinder relocated(
        relocated_rom.data(), relocated_rom.size(), kBase);
    relocated.set_speculative_literal_harvest(false);
    relocated.add_code_copy(kRam, kBase + kTemplate, 0x20, "first copy");
    relocated.add_seed({kRam, gbarecomp::CpuMode::Thumb, "first"});
    gbarecomp::FunctionSeed overlapping{
        kRam + 4, gbarecomp::CpuMode::Thumb, "overlapping"};
    overlapping.source_addr = kBase + kTemplate;
    relocated.add_seed(overlapping);
    relocated.run(32);

    const auto* overlap_fn = find_function(
        relocated, kRam + 4, gbarecomp::CpuMode::Thumb);
    if (!overlap_fn || overlap_fn->source_addr != kBase + kTemplate) {
        std::fprintf(stderr,
                     "overlapping relocation did not retain explicit ROM backing\n");
        return 1;
    }
    if (overlap_fn->end_addr <= kRam + 8) {
        std::fprintf(stderr,
                     "older placement incorrectly truncated overlapping CFG\n");
        return 1;
    }
    const auto* overlap_target = find_function(
        relocated, kRam + 12, gbarecomp::CpuMode::Thumb);
    if (!overlap_target ||
        overlap_target->source_addr != kBase + kTemplate + 8) {
        std::fprintf(stderr,
                     "relocation bias did not propagate to direct branch target\n");
        return 1;
    }

    // A bounded AOT sweep finds compiler-shaped prologues without runtime
    // evidence, then uses two strong entries in an even-address THUMB pointer
    // table to infer the table mode and recover a leaf function with no PUSH.
    std::vector<std::uint8_t> aot_rom(0x100, 0);
    put16(aot_rom, 0x00, 0x4802u);  // ldr r0,[pc,#8] -> table base
    put16(aot_rom, 0x02, 0x4770u);  // bx lr
    put32(aot_rom, 0x0C, kBase + 0xC0u);
    put16(aot_rom, 0x40, 0xB500u);  // push {lr}
    put16(aot_rom, 0x42, 0x46C0u);
    put16(aot_rom, 0x44, 0x46C0u);
    put16(aot_rom, 0x46, 0x46C0u);
    put16(aot_rom, 0x48, 0x46C0u);
    put16(aot_rom, 0x4A, 0xBD00u);  // pop {pc}
    put16(aot_rom, 0x50, 0xB500u);  // second strong table-mode vote
    put16(aot_rom, 0x52, 0x46C0u);
    put16(aot_rom, 0x54, 0x46C0u);
    put16(aot_rom, 0x56, 0x46C0u);
    put16(aot_rom, 0x58, 0x46C0u);
    put16(aot_rom, 0x5A, 0xBD00u);
    put16(aot_rom, 0x60, 0x2001u);  // movs r0,#1 (leaf, no prologue)
    put16(aot_rom, 0x62, 0x46C0u);
    put16(aot_rom, 0x64, 0x46C0u);
    put16(aot_rom, 0x66, 0x46C0u);
    put16(aot_rom, 0x68, 0x46C0u);
    put16(aot_rom, 0x6A, 0x4770u);  // bx lr
    put32(aot_rom, 0xC0, kBase + 0x40u);
    put32(aot_rom, 0xC4, kBase + 0x50u);
    put32(aot_rom, 0xC8, kBase + 0x60u);

    gbarecomp::FunctionFinder aot(aot_rom.data(), aot_rom.size(), kBase);
    aot.set_speculative_literal_harvest(false);
    aot.set_aot_scan_range(kBase + 0x40u, kBase + 0x80u);
    aot.add_seed({kBase, gbarecomp::CpuMode::Thumb, "entry"});
    aot.run(64);
    if (!has_function(aot, kBase + 0x40u, gbarecomp::CpuMode::Thumb) ||
        !has_function(aot, kBase + 0x50u, gbarecomp::CpuMode::Thumb) ||
        !has_function(aot, kBase + 0x60u, gbarecomp::CpuMode::Thumb)) {
        std::fprintf(stderr,
                     "bounded AOT scan missed a prologue or address-taken "
                     "THUMB leaf\n");
        return 1;
    }
    if (aot.stats().aot_prologue_seeds + aot.stats().aot_callback_seeds != 2 ||
        aot.stats().aot_pointer_tables != 1 ||
        aot.stats().aot_pointer_seeds != 1) {
        std::fprintf(stderr,
                     "bounded AOT scan stats disagree: prologues=%zu "
                     "tables=%zu leaves=%zu\n",
                     aot.stats().aot_prologue_seeds,
                     aot.stats().aot_pointer_tables,
                     aot.stats().aot_pointer_seeds);
        return 1;
    }

    // A copied ARM blob can contain internal callable helpers that are not
    // reachable from its declared entry. Scan the ROM backing of a known
    // code-copy range for stack-frame prologues, including leaf helpers that
    // save callee-saved registers without saving LR.
    std::vector<std::uint8_t> copied_rom(0x100, 0);
    constexpr std::size_t kCopiedTemplate = 0x40;
    put32(copied_rom, kCopiedTemplate + 0x00, 0xE12FFF1Eu); // bx lr
    put32(copied_rom, kCopiedTemplate + 0x1C, 0xE1A0C00Du); // mov ip,sp
    put32(copied_rom, kCopiedTemplate + 0x20, 0xE92D0030u); // push {r4,r5}
    put32(copied_rom, kCopiedTemplate + 0x24, 0xE1A00000u); // nop
    put32(copied_rom, kCopiedTemplate + 0x28, 0xE1A00000u);
    put32(copied_rom, kCopiedTemplate + 0x2C, 0xE1A00000u);
    put32(copied_rom, kCopiedTemplate + 0x30, 0xE1A00000u);
    put32(copied_rom, kCopiedTemplate + 0x34, 0xE8BD0030u); // pop {r4,r5}
    put32(copied_rom, kCopiedTemplate + 0x38, 0xE12FFF1Eu); // bx lr

    constexpr std::uint32_t kCopiedRam = 0x03001000u;
    gbarecomp::FunctionFinder copied(
        copied_rom.data(), copied_rom.size(), kBase);
    copied.set_speculative_literal_harvest(false);
    copied.set_static_resume_all(true);
    copied.add_code_copy(
        kCopiedRam, kBase + kCopiedTemplate, 0x40, "ARM helper blob");
    copied.add_seed(
        {kCopiedRam, gbarecomp::CpuMode::Arm, "copied_entry"});
    copied.run(32);
    const auto* copied_helper = find_function(
        copied, kCopiedRam + 0x1Cu, gbarecomp::CpuMode::Arm);
    bool copied_push_resume = false;
    if (copied_helper) {
        for (std::uint32_t pc : copied_helper->alias_entries) {
            if (pc == kCopiedRam + 0x20u) copied_push_resume = true;
        }
    }
    if (!copied_helper ||
        copied_helper->source_addr != kBase + kCopiedTemplate + 0x1Cu ||
        copied.stats().aot_code_copy_seeds != 1 ||
        !copied_push_resume) {
        std::fprintf(stderr,
                     "code-copy scan missed internal ARM helper: fn=%s "
                     "source=0x%08X seeds=%zu push_resume=%s\n",
                     copied_helper ? "yes" : "no",
                     copied_helper ? copied_helper->source_addr : 0u,
                     copied.stats().aot_code_copy_seeds,
                     copied_push_resume ? "yes" : "no");
        return 1;
    }

    for (unsigned fault = 0; fault <= 5; ++fault) {
        if (!literal_callback(fault)) {
            std::fprintf(stderr, "AOT callback literal failed: fault=%u\n", fault);
            return 1;
        }
    }
    for (bool adr : {false, true}) for (bool tagged : {false, true}) {
        if (!copied_switch_source(adr, tagged)) {
            std::fprintf(stderr, "Copied switch ROM targets missing: ADR=%u tag=%u\n", adr, tagged);
            return 1;
        }
    }
    for (unsigned fault=0;fault<5;++fault) if (!byte_record_callbacks(fault)) {
        std::fprintf(stderr,"Finite byte-index callback records failed: fault=%u\n",fault);
        return 1;
    }
    for (bool copied : {false,true}) for (unsigned fault=0;fault<5;++fault) {
        if (!arm_backward_adr_table(copied,fault)) {
            std::fprintf(stderr,"ARM backward ADR table failed: copied=%u fault=%u\n",copied,fault);
            return 1;
        }
    }
    for (bool copied : {false,true}) for (unsigned fault=0;fault<4;++fault) {
        if (!arm_sparse_preceding_table(copied,fault)) {
            std::fprintf(stderr,"Sparse preceding ARM table failed: copied=%u fault=%u\n",copied,fault);
            return 1;
        }
    }
    for (unsigned fault = 0; fault <= 19; ++fault) {
        if (!referenced_leaf_pair(fault)) {
            std::fprintf(stderr, "Referenced leaf callback pair failed: fault=%u\n", fault);
            return 1;
        }
    }
    for (unsigned fault=0;fault<=15;++fault) if (!referenced_callback_prefix(fault)) {
        std::fprintf(stderr,"Referenced callback prefix failed: fault=%u\n",fault);
        return 1;
    }
    for(unsigned fault=0;fault<=21;++fault) if(!callbacks_across_proved_calls(fault)) {
        std::fprintf(stderr,"Callbacks across proved calls failed: fault=%u\n",fault);
        return 1;
    }
    for(unsigned fault=0;fault<=8;++fault) if(!sparse_byte_word_callbacks(fault)) {
        std::fprintf(stderr,"Finite sparse word callbacks failed: fault=%u\n",fault);
        return 1;
    }
    for(unsigned fault=0;fault<=13;++fault) if(!shared_ram_store_callbacks(fault)) {
        std::fprintf(stderr,"Shared RAM-store callbacks failed: fault=%u\n",fault);
        return 1;
    }
    if (!branch_callbacks_preserve_existing_budget()) {
        std::fprintf(stderr,"Branch callback extension displaced existing call-site coverage\n");
        return 1;
    }
    for(unsigned fault=0;fault<=7;++fault) if(!callback_constants_at_branch_joins(fault)) {
        std::fprintf(stderr,"Callback branch-join constants failed: fault=%u\n",fault);
        return 1;
    }
    for(unsigned fault=0;fault<=12;++fault) if(!masked_selector_at_branch_joins(fault)) {
        std::fprintf(stderr,"Masked selector branch joins failed: fault=%u\n",fault);
        return 1;
    }
    for (unsigned fault=0; fault<8; ++fault) {
        std::vector<uint8_t> image(0x400,0);
        put16(image,0,0x480Fu); put16(image,2,0x4770u);
        put32(image,0x40,kBase+(fault==6 ? 0x260u : 0x200u));
        for (unsigned n=0;n<2;++n) {
            const unsigned pc=0x80+n*8;
            put16(image,pc,(n && fault!=4) ? 0x688Au : 0x680Au);
            put16(image,pc+2,fault==2 ? 0x1850u : 0x1851u);
            put16(image,pc+4,fault==5 ? 0x46C0u : 0xF000u);
            put16(image,pc+6,uint16_t(0xF800u+((0xC0u-(pc+8u))/2u)));
        }
        put16(image,0x90,0x4770u);
        put16(image,0xC0,fault==1 ? 0x4700u : 0x4708u);
        const uint32_t first=fault==7 ? 0x100u : 0x300u;
        const uint32_t second=first+0x20u;
        put32(image,0x200,(first+1u)-0x200u);
        put32(image,0x204,0x141u); // unconsumed slot: must stay undiscovered
        put32(image,0x208,fault==3 ? 0x80000001u : (second+1u)-0x200u);
        for (auto pc : {first,second,0x340u}) {
            put16(image,pc,0xB510u);
            for (unsigned k=2;k<10;k+=2) put16(image,pc+k,0x2001u);
            put16(image,pc+10,0xBD10u);
        }
        gbarecomp::FunctionFinder relative(image.data(),image.size(),kBase);
        relative.set_speculative_literal_harvest(false);
        relative.set_aot_scan_range(kBase+0x100u,kBase+0x400u);
        relative.add_seed({kBase,gbarecomp::CpuMode::Thumb,"table_reference"});
        relative.add_seed({kBase+0x80u,gbarecomp::CpuMode::Thumb,"relative_consumer"});
        relative.run(128);
        const bool expected=fault==0 || fault==7;
        if ((find_function(relative,kBase+first,gbarecomp::CpuMode::Thumb)!=nullptr)!=expected ||
            (find_function(relative,kBase+second,gbarecomp::CpuMode::Thumb)!=nullptr)!=expected ||
            find_function(relative,kBase+0x340u,gbarecomp::CpuMode::Thumb)) {
            std::fprintf(stderr,"Relative callback slots failed: fault=%u\n",fault);
            return 1;
        }
    }
    // In a large image, the caller's wrong bias can still point into ROM.
    // A different declared code copy must supply the callee's source.
    std::vector<uint8_t> cross_image(0x01000200u,0xFFu);
    put32(cross_image,0,0xEB3FFFFEu); // BL 03000000 at runtime 02000000
    put32(cross_image,4,0xE12FFF1Eu);
    put32(cross_image,0x100u,0xE3A00007u); put32(cross_image,0x104u,0xE12FFF1Eu);
    gbarecomp::FunctionFinder cross(cross_image.data(),cross_image.size(),kBase);
    cross.set_speculative_literal_harvest(false);
    cross.add_code_copy(0x02000000u,kBase,8u,"caller");
    cross.add_code_copy(0x03000000u,kBase+0x100u,8u,"callee");
    cross.add_code_copy(0x03000000u,kBase+0x180u,8u,"shadowed mapping");
    cross.add_seed({0x02000000u,gbarecomp::CpuMode::Arm,"caller"});
    cross.run(16);
    const auto* cross_callee=find_function(cross,0x03000000u,gbarecomp::CpuMode::Arm);
    if (!cross_callee || cross_callee->source_addr!=kBase+0x100u) {
        std::fprintf(stderr,"Cross-copy call inherited the caller's source bias\n");
        return 1;
    }
    std::printf("function_finder_tests: PASS\n");
    return 0;
}
