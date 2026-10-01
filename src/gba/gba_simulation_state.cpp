#include "gba_simulation_state.h"
#include "simulation_archive.h"
#include "gba_bus.h"
#include "gba_ppu.h"
#include <algorithm>

namespace gba {
class SimulationStateCodec {
    template<class A, class C> static void square(A& a, C& c) {
        a(c.active, c.duty, c.length, c.length_enabled, c.envelope_initial,
          c.envelope_increase, c.envelope_step, c.volume, c.frequency,
          c.waveform_cycles, c.waveform_phase, c.envelope_cycles, c.length_cycles);
    }
    template<class A, class F> static void fifo(A& a, F& f) {
        a(f.words, f.write, f.read, f.count, f.shift_word, f.bytes_remaining, f.samples);
        if (f.write >= 8 || f.read >= 8 || f.count > 8 || f.bytes_remaining > 4)
            throw std::invalid_argument("invalid audio FIFO state");
    }
    template<class A, class C> static void audio(A& a, C& s) {
        if (s.shadow_enabled_) throw std::invalid_argument("audio shadow is not qualified for multiplayer");
        square(a, s.ch1_); square(a, s.ch2_);
        a(s.ch1_.sweep_shift, s.ch1_.sweep_decrease, s.ch1_.sweep_time, s.ch1_.sweep_cycles);
        auto& w = s.ch3_;
        a(w.active,w.dac_on,w.two_banks,w.bank,w.length,w.length_enabled,w.volume_code,
          w.force_volume,w.frequency,w.wave_cycles,w.wave_pos,w.length_cycles);
        a(s.wave_ram_);
        auto& n = s.ch4_;
        a(n.active,n.length,n.length_enabled,n.envelope_initial,n.envelope_increase,
          n.envelope_step,n.volume,n.divisor_code,n.width_7bit,n.shift,n.lfsr,
          n.noise_cycles,n.envelope_cycles,n.length_cycles);
        a(s.master_enable_,s.volume_l_,s.volume_r_,s.ch1_left_enable_,s.ch1_right_enable_,
          s.ch2_left_enable_,s.ch2_right_enable_,s.ch3_left_enable_,s.ch3_right_enable_,
          s.ch4_left_enable_,s.ch4_right_enable_,s.dmg_volume_ratio_);
        fifo(a,s.fifo_a_); fifo(a,s.fifo_b_);
        a(s.direct_a_left_,s.direct_a_right_,s.direct_a_timer1_,s.direct_a_full_volume_,
          s.direct_b_left_,s.direct_b_right_,s.direct_b_timer1_,s.direct_b_full_volume_,
          s.soundbias_,s.cycles_per_sample_,s.cycle_accumulator_,s.current_samples_,s.sample_index_);
        if (w.bank > 1 || w.wave_pos > 63 || w.frequency > 2047 ||
            n.divisor_code > 7 || n.shift > 15 || s.ch1_.duty > 3 || s.ch2_.duty > 3 ||
            s.ch1_.frequency > 2047 || s.ch2_.frequency > 2047 || s.ch1_.waveform_phase > 7 ||
            s.ch2_.waveform_phase > 7 || s.sample_index_ > GbaAudio::kMaxSamplesPerEvent ||
            (s.cycles_per_sample_ != 64 && s.cycles_per_sample_ != 128 &&
             s.cycles_per_sample_ != 256 && s.cycles_per_sample_ != 512) || s.cycle_accumulator_ > 1024)
            throw std::invalid_argument("invalid audio channel state");
        // ring_, cap_ring_, trace_, and samples_generated_ are host output or
        // diagnostics; replay advances the mixer without replaying those queues.
    }
    template<class A, class C> static void save(A& a, C& s) {
        a(s.sram_enabled_,s.sram_size_); a.vector(s.sram_data_,128*1024);
        a(s.eeprom_enabled_,s.dirty_,s.eeprom_size_,s.eeprom_addr_bits_,s.eeprom_block_mask_,s.eeprom_);
        a.vector(s.command_bits_,128);
        a(s.read_active_,s.read_byte_offset_,s.read_bit_index_,s.flash_enabled_,s.flash_size_,
          s.flash_banks_,s.flash_maker_,s.flash_device_,s.flash_state_,s.flash_id_mode_,s.flash_bank_);
        a.vector(s.flash_data_,128*1024);
        if (s.sram_size_ != s.sram_data_.size() || s.flash_size_ != s.flash_data_.size() ||
            s.eeprom_size_ > s.eeprom_.size() || s.eeprom_addr_bits_ > 14 ||
            s.read_byte_offset_ > s.eeprom_.size() || s.read_bit_index_ > 68 ||
            s.flash_banks_ > 2 || s.flash_bank_ >= s.flash_banks_ ||
            static_cast<unsigned>(s.flash_state_) > 7)
            throw std::invalid_argument("invalid cartridge save state");
    }
    template<class A, class C> static void rtc(A& a, C& s) {
        a(s.active_,s.sck_,s.cs_,s.sio_out_,s.phase_,s.cmd_acc_,s.nbits_,s.byte_idx_,s.buflen_,
          s.buffer_,s.reg_,s.lsb_first_,s.control_,s.offset_,s.boot_seeded_,s.boot_seconds_,
          s.emulated_clock_,s.emulated_cycles_);
        if (!s.emulated_clock_ || s.byte_idx_ > 7 || s.buflen_ > 7 || s.nbits_ > 8 ||
            static_cast<unsigned>(s.phase_) > 3)
            throw std::invalid_argument("invalid emulated RTC state");
    }
public:
    template<class A, class B, class P> static void visit(A& a, B& b, P& p) {
        if (b.gyro_.active() || b.solar_.active() || b.matrix_.active())
            throw std::invalid_argument("cartridge peripheral is not qualified for multiplayer");
        if (p.view_expanded()) throw std::invalid_argument("expanded view is not qualified for multiplayer");
        a.blob(b.ewram_,b.ewram_.size()); a.blob(b.iwram_,b.iwram_.size());
        a.blob(b.pal_,b.pal_.size()); a.blob(b.vram_,b.vram_.size()); a.blob(b.oam_,b.oam_.size());
        a(b.last_fetched_,b.bios_access_enabled_,b.bios_prefetch_,b.open_bus_pc_,b.open_bus_thumb_);
        // Wait states: WAITCNT is in the IO page below; memory control, the
        // accepted EWRAM wait and the prefetch-buffer position are bus state.
        a(b.memctl_,b.waits_->n16[0x2],b.waits_->last_prefetched_pc);
        if (b.waits_->n16[0x2] == 0 || b.waits_->n16[0x2] > 15)
            throw std::invalid_argument("invalid EWRAM wait state");
        auto& i = b.io_dispatch_;
        a.blob(i.io_,i.io_.size());
        a(i.halted_,i.host_keyinput_,i.synth_keyinput_,i.timer_reload_,i.timer_counter_,
          i.timer_control_,i.timer_accum_,i.dma_next_source_,i.dma_next_dest_,
          i.sio_transfer_active_,i.sio_cycles_remaining_,i.dma_steal_cycles_);
        audio(a,b.audio_); save(a,b.save_); rtc(a,b.rtc_);
        auto& g = b.gpio_;
        a(g.data_,g.driven_,g.direction_,g.read_enable_);
        if ((g.data_ | g.driven_ | g.direction_) > 15) throw std::invalid_argument("invalid GPIO state");
        a(p.scanline_,p.dot_in_scanline_,p.cycle_in_dot_,p.vcount_,p.frame_count_,p.has_latched_fb_);
        for (auto& line : p.affine_line_)
            a(line.x,line.y,line.valid_x,line.valid_y,line.reload_x,line.reload_y);
        if (p.scanline_ >= GbaPpu::kLinesTotal || p.dot_in_scanline_ >= GbaPpu::kDotsPerScanline ||
            p.cycle_in_dot_ >= GbaPpu::kCyclesPerDot || p.vcount_ >= GbaPpu::kLinesTotal)
            throw std::invalid_argument("invalid PPU phase");
        a.blob(p.work_fb_,GbaPpu::kFramebufferBytes);
        a.blob(p.latched_fb_,GbaPpu::kFramebufferBytes);
    }
};
std::vector<std::uint8_t> save_device_state(const GbaBus& bus, const GbaPpu& ppu) {
    SimulationArchive<false> archive;
    SimulationStateCodec::visit(archive,bus,ppu);
    return archive.take();
}
bool load_device_state(GbaBus& bus, GbaPpu& ppu, std::span<const std::uint8_t> bytes) {
    try {
        SimulationArchive<true> archive(bytes);
        SimulationStateCodec::visit(archive,bus,ppu);
        if (archive.remaining() != 0) return false;
        bus.refresh_waitstates();
        return true;
    } catch (const std::invalid_argument&) { return false; }
}
} // namespace gba
