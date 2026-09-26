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
}  // namespace

int main() {
    test_rate_boundaries_and_partial_drains();
    test_restore_and_discard();
    test_pcm_and_ring_wrap();
    if (failures) return 1;
    std::puts("audio playback: rate boundaries, restore and discard passed");
    return 0;
}
