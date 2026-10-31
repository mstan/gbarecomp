#include "gba_audio.h"
#include "snapshot.h"

#include <array>
#include <algorithm>
#include <cstdio>
#include <memory>

namespace {
int failures = 0;
void check(bool condition, const char* what) {
    if (!condition) { std::printf("FAIL: %s\n", what); ++failures; }
}

auto fresh_audio() {
    auto audio = std::make_unique<gba::GbaAudio>();
    audio->tick(0);  // Consume the reset-time event before measuring intervals.
    audio->discard_playback();
    return audio;
}

void test_rate_boundaries_and_partial_drains() {
    auto audio = fresh_audio();
    for (unsigned i = 0; i < 4; ++i) {
        audio->write_io16(0x088, 0x0200 | (i << 14));
        audio->tick(gba::GbaAudio::kSampleEventCycles);
    }
    std::array<int16_t, 32> pcm{};
    uint32_t rate = 123;
    check(audio->drain_sample_block(pcm.data(), 0, rate) == 0 && rate == 123,
          "zero capacity does not consume queued audio");
    for (unsigned i = 0; i < 4; ++i) {
        check(audio->drain_sample_block(pcm.data(), 1, rate) == 1,
              "partial drain returns one sample");
        check(rate == (32768u << i), "partial drain retains production rate");
        check(audio->drain_sample_block(pcm.data(), pcm.size(), rate) == (2u << i) - 1,
              "block stops before the next rate even with spare capacity");
        check(rate == (32768u << i), "queued rate is independent of current SOUNDBIAS");
    }
    check(audio->drain_sample_block(pcm.data(), pcm.size(), rate) == 0,
          "all rate segments consumed");
}

void test_restore_and_discard() {
    auto audio = fresh_audio();
    audio->write_io16(0x088, 0x8200);
    audio->tick(1024);
    gbarecomp::debug::SnapshotWriter writer;
    audio->serialize(writer);
    const auto& bytes = writer.buffer();
    auto restored = fresh_audio();
    gbarecomp::debug::SnapshotReader reader(bytes.data(), bytes.size());
    restored->deserialize(reader);
    check(reader.ok(), "audio snapshot restores");
    gbarecomp::debug::SnapshotWriter roundtrip;
    restored->serialize(roundtrip);
    check(roundtrip.buffer() == bytes, "playback metadata does not change snapshot format");

    std::array<int16_t, 32> legacy{}, tagged{};
    check(restored->drain_samples(legacy.data(), legacy.size()) == 8,
          "legacy drain preserves restored PCM without rate metadata");
    gbarecomp::debug::SnapshotReader again(bytes.data(), bytes.size());
    restored->deserialize(again);
    restored->tick(1024);
    uint32_t rate = 0;
    check(restored->drain_sample_block(tagged.data(), tagged.size(), rate) == 8,
          "playback skips unknown restored samples and returns fresh samples");
    check(rate == 131072, "fresh samples use restored SOUNDBIAS");
    check(tagged == legacy, "tagged and legacy drains preserve PCM");
    restored->tick(1024);
    const auto generated = restored->samples_generated();
    restored->discard_playback();
    check(restored->samples_generated() == generated, "discard preserves generation counter");
    check(restored->drain_samples(legacy.data(), legacy.size()) == 0,
          "discard also empties legacy consumer queue");
    restored->tick(1024);
    check(restored->drain_sample_block(tagged.data(), tagged.size(), rate) == 8,
          "production resumes after discard");
    restored->reset();
    check(restored->drain_sample_block(tagged.data(), tagged.size(), rate) == 0,
          "reset empties playback queue");
}

void test_pcm_and_ring_wrap() {
    auto tagged = fresh_audio();
    auto legacy = fresh_audio();
    for (auto* audio : {tagged.get(), legacy.get()}) {
        audio->write_io16(0x084, 0x0080);  // Master sound enable.
        audio->write_io16(0x080, 0x2277);  // Route SOUND2 to both speakers.
        audio->write_io16(0x068, 0xf080);  // Square wave, fixed envelope.
        audio->write_io16(0x06c, 0x87c0);  // Trigger.
    }
    std::array<int16_t, 16> a{}, b{};
    bool audible = false;
    // More than one ring's worth of samples, with a different rate at each
    // boundary. Compare independent consumers to catch PCM/tag cursor drift.
    for (unsigned event = 0; event < 2400; ++event) {
        const unsigned index = event % 4;
        for (auto* audio : {tagged.get(), legacy.get()}) {
            audio->write_io16(0x088, 0x0200 | (index << 14));
            audio->tick(1024);
        }
        uint32_t rate = 0;
        const auto n = tagged->drain_sample_block(a.data(), a.size(), rate);
        check(n == (2u << index), "sample count survives ring cursor wrap");
        check(rate == (32768u << index), "rate tags survive ring cursor wrap");
        check(legacy->drain_samples(b.data(), b.size()) == n && a == b,
              "tagged playback preserves legacy PCM across ring wrap");
        audible = audible || std::any_of(a.begin(), a.end(), [](int16_t s) { return s != 0; });
    }
    check(audible, "PCM comparison exercised actual sound rather than only silence");
}

void test_wave_and_noise_restore_continuation() {
    auto audio = fresh_audio();
    audio->write_io16(0x084,0x0080);
    audio->write_io16(0x080,0xcc77); // Wave and noise, both speakers.
    for (unsigned bank = 0; bank < 2; ++bank) {
        audio->write_io16(0x070,0x00a0 | (bank << 6));
        for (unsigned i = 0; i < 16; ++i)
            audio->write_io8(0x090+i,static_cast<uint8_t>(0x13u+i*11u+bank*7u));
    }
    audio->write_io16(0x070,0x00a0); // Play both banks from bank zero.
    audio->write_io16(0x072,0x2000);
    audio->write_io16(0x074,0x87b1);
    audio->write_io16(0x078,0xf200);
    audio->write_io16(0x07c,0x802b);
    audio->tick(31*1024+137); // Nonzero phase in both generators and APU clock.
    audio->discard_playback();
    gbarecomp::debug::SnapshotWriter snapshot;
    audio->serialize(snapshot);
    auto restored = fresh_audio();
    gbarecomp::debug::SnapshotReader reader(snapshot.buffer().data(),snapshot.size());
    restored->deserialize(reader);
    check(reader.ok() && reader.remaining()==0,"complete wave/noise state restores");
    bool wave=false,noise=false;
    for (unsigned event=0; event<128; ++event) {
        const auto start=audio->samples_generated();
        audio->tick(1024); restored->tick(1024);
        std::array<int16_t,32> original{},replayed{};
        const auto n=audio->drain_samples(original.data(),original.size());
        const auto m=restored->drain_samples(replayed.data(),replayed.size());
        check(n==m && original==replayed,"wave/noise PCM matches immediately after restore");
        std::array<gba::GbaAudio::CapSample,32> taps{};
        uint64_t first=0;
        const auto count=audio->query_capture(start,taps.size(),taps.data(),first);
        for (std::size_t i=0;i<count;++i) { wave|=taps[i].ch[2]!=0; noise|=taps[i].ch[3]!=0; }
    }
    check(wave && noise,"restore comparison exercised audible wave and noise channels");
}
// Compare complete audio state and audible PCM after real IO/timer barriers.
// The reference visits every sample deadline; the window arm receives the
// same elapsed time in one call, including intervals crossing several events.
void test_hardware_owned_windows() {
    constexpr std::array<uint32_t,12> gaps{
        1,63,127,511,512,513,1006,1023,1024,1025,2077,6173};
    bool audible=false;
    unsigned cases=0;
    for (unsigned rate=0;rate<4;++rate) for (unsigned phase : {0u,1u,511u,1023u}) {
        auto reference=fresh_audio(), window=fresh_audio();
        for (auto* a : {reference.get(),window.get()}) {
            a->write_io16(0x084,0x0080);
            a->write_io16(0x080,0xff77);
            a->write_io16(0x082,0x330e);
            a->write_io16(0x088,0x0200|(rate<<14));
            a->write_io16(0x062,0xf280);
            a->write_io16(0x064,0x84c0);
            a->write_io16(0x068,0xb140);
            a->write_io16(0x06c,0x86a0);
            a->write_io16(0x070,0x00a0);
            for (unsigned i=0;i<16;++i)
                a->write_io8(0x090+i,static_cast<uint8_t>(i*13+7));
            a->write_io16(0x072,0x2000);
            a->write_io16(0x074,0x87b1);
            a->write_io16(0x078,0xf200);
            a->write_io16(0x07c,0x802b);
            for (unsigned i=0;i<8;++i) {
                a->write_io32(0x0a0,0x817f0bf1u+i*0x01010101u);
                a->write_io32(0x0a4,0x3f01c780u+i*0x03030303u);
            }
            a->tick(phase);
        }
        for (unsigned event=0;event<96;++event) {
            const auto gap=gaps[(event+phase)%gaps.size()];
            auto remaining=gap;
            while (remaining) {
                const auto step=std::min(remaining,reference->cycles_until_next_sample());
                reference->tick(step);remaining-=step;
            }
            window->tick(gap);
            for (auto* a : {reference.get(),window.get()}) {
                a->timer_overflow(event&1);
                if (event%3==0) a->write_io32(0x0a0,0x815c27f0u+event);
                if (event%5==0) a->write_io32(0x0a4,0xd9317f02u-event);
                if (event%11==0) a->write_io16(0x088,0x0200|(((rate+event/11)&3)<<14));
                if (event%17==0) a->write_io16(0x064,0x8400|(event*7));
                if (event%19==0) a->write_io16(0x07c,0x8000|(event&0x7f));
            }
            gbarecomp::debug::SnapshotWriter a,b;
            reference->serialize(a);window->serialize(b);
            check(a.buffer()==b.buffer(),"complete audio state matches across owned windows and IO/FIFO changes");
            std::array<int16_t,256> x{},y{};
            const auto n=reference->drain_samples(x.data(),x.size());
            const auto m=window->drain_samples(y.data(),y.size());
            check(n==m && x==y,"owned windows preserve all audible PCM");
            audible|=std::any_of(x.begin(),x.end(),[](int16_t value){return value!=0;});
            if (event==47) {
                // Restore both at a nonzero clock/FIFO/oscillator phase, then
                // continue with the two different scheduling cadences.
                auto restored=fresh_audio();
                gbarecomp::debug::SnapshotReader reader(b.buffer().data(),b.size());
                restored->deserialize(reader);
                check(reader.ok() && reader.remaining()==0,"owned window state restores completely");
                std::array<int16_t,256> discard{};
                restored->drain_samples(discard.data(),discard.size());
                window=std::move(restored);
            }
            ++cases;
        }
    }
    check(audible,"owned-window comparison exercised audible PSG and FIFO samples");
    std::printf("audio owned windows: %u complete-state/PCM barriers\n",cases);
}

void test_shared_event_observability() {
    auto audio=fresh_audio();
#if GBARECOMP_TEST_DEVICE_AUDIO_WINDOWS
    check(audio->cycles_until_next_shared_event()==UINT32_MAX,
          "hardware-only samples stay inside their owned window");
#else
    check(audio->cycles_until_next_shared_event()==audio->cycles_until_next_sample(),
          "portable reference retains original shared sample deadlines");
#endif
    // An armed but not yet live Shadow still observes guest RAM. It must
    // retain its sample barrier even before its first successful driver hook.
    audio->configure_shadow({gba::Mp2kSig{0,0x03002601}},
        nullptr,0,nullptr,0,nullptr,0,true);
    check(audio->shadow_enabled(),"RAM-observing mixer actually armed");
    audio->tick(137);
    check(audio->cycles_until_next_shared_event()==audio->cycles_until_next_sample(),
          "armed RAM-observing mixer keeps its exact current-phase deadline");
    audio->set_shadow_enabled(false);
#if GBARECOMP_TEST_DEVICE_AUDIO_WINDOWS
    check(audio->cycles_until_next_shared_event()==UINT32_MAX,
          "hardware window ownership resumes after disabling Shadow");
#else
    check(audio->cycles_until_next_shared_event()==audio->cycles_until_next_sample(),
          "reference sample boundary is unchanged after disabling Shadow");
#endif
}
}  // namespace

int main() {
    test_rate_boundaries_and_partial_drains();
    test_restore_and_discard();
    test_pcm_and_ring_wrap();
    test_wave_and_noise_restore_continuation();
    test_hardware_owned_windows();
    test_shared_event_observability();
    if (failures) return 1;
    std::puts("audio playback: rate boundaries, restore and discard passed");
    return 0;
}
