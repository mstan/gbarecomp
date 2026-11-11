#include "runtime_context.h"
#include "snapshot.h"
#include "gba_bus.h"
#include "gba_ppu.h"
#include <memory>
#include <cstdio>
#include <array>
#include <algorithm>
#include <cstring>

int main() {
    auto bus = std::make_unique<gba::GbaBus>();
    auto ppu = std::make_unique<gba::GbaPpu>();
    gbarecomp::debug::SnapshotContext ctx;
    ctx.bus = bus.get(); ctx.ppu = ppu.get();
    ctx.rom_sha1 = "0123456789012345678901234567890123456789";
    runtime_init(nullptr);
    g_cpu = {};
    g_cpu.cpsr = 0x13;
    g_cpu.banked_spsr[ARM_BANK_SUPERVISOR] = 0x3f;
    gbarecomp::runtime_restore_swi_context({{0x08001002u, 0x08001000u}});
    std::string error;
    std::vector<uint8_t> bytes;
    if (!gbarecomp::debug::save_state_bytes(&bytes, ctx, &error)) return 1;
    gbarecomp::runtime_restore_swi_context({});
    if (!gbarecomp::debug::load_state_bytes(bytes.data(), bytes.size(), ctx, &error)) return 2;
    runtime_fp_reset();
    std::vector<gbarecomp::RuntimeSwiContinuation> restored;
    gbarecomp::runtime_capture_swi_context(restored);
    if (restored.size() != 1 || restored[0].continuation != 0x08001002u) return 3;
    // CPU0 is the first section. Its active SWI count follows the raw CPU
    // state and the empty call-return stack. Reject it before changing CPU.
    auto invalid = bytes;
    const std::size_t count = 52u + 8u + sizeof(ArmCpuState) + 4u;
    invalid[count] = 65;
    g_cpu.R[0] = 12345;
    if (gbarecomp::debug::load_state_bytes(invalid.data(), invalid.size(), ctx, &error) ||
        g_cpu.R[0] != 12345) return 4;
    runtime_exception_return(0x08001002u);
    if (g_cpu.R[15] != 0x08001002u) return 5;
    auto& audio=bus->audio();
    audio.tick(0);audio.discard_playback();
    audio.write_io16(0x084,0x80);audio.write_io16(0x080,0x2077);
    audio.write_io16(0x082,2);audio.write_io16(0x068,0xf080);audio.write_io16(0x06c,0x87c0);
    audio.tick(1024*64+137);
    if(!gbarecomp::debug::save_state_bytes(&bytes,ctx,&error))return 6;
    std::array<int16_t,512> first{},replayed{};
    const auto frames=audio.drain_stereo_samples(first.data(),256);
    if(!gbarecomp::debug::load_state_bytes(bytes.data(),bytes.size(),ctx,&error)||
       audio.drain_stereo_samples(replayed.data(),256)!=frames||first!=replayed)return 7;
    bool audible=false;
    for(size_t i=0;i<frames;++i){audible|=first[i*2]!=0;if(first[i*2+1]!=0)return 8;}
    if(!audible)return 9;
    // Rewind uses the same byte container. Old format, truncated audio and
    // impossible ring cursors must reject before even R0 changes.
    invalid=bytes;invalid[4]=5;g_cpu.R[0]=54321;
    if(gbarecomp::debug::load_state_bytes(invalid.data(),invalid.size(),ctx,&error)||g_cpu.R[0]!=54321)return 10;
    auto read32=[&](size_t p){uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(bytes[p+i])<<(i*8);return v;};
    size_t off=52,audio_off=0,audio_len=0;
    while(off+8<=bytes.size()){
        const auto len=read32(off+4);
        if(std::memcmp(bytes.data()+off,"AUD0",4)==0){audio_off=off+8;audio_len=len;break;}
        off+=8+len;
    }
    if(!audio_off||audio_len<100)return 11;
    invalid=bytes;
    // Tail of AUD0 is head/tail/counter, 16 stereo pending frames, index.
    const auto ring_head=audio_off+audio_len-(24+16*4+4);
    for(unsigned i=0;i<8;++i)invalid[ring_head+i]=0xff;
    if(gbarecomp::debug::load_state_bytes(invalid.data(),invalid.size(),ctx,&error)||g_cpu.R[0]!=54321)return 12;
    invalid=bytes;
    // A zeroed audio section is structurally framed but not a valid ring.
    std::fill(invalid.begin()+audio_off,invalid.begin()+audio_off+audio_len,0);
    if(gbarecomp::debug::load_state_bytes(invalid.data(),invalid.size(),ctx,&error)||g_cpu.R[0]!=54321)return 13;
    std::puts("SVC snapshot roundtrip and invalid-depth rejection passed");
}
