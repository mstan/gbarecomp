// Public capture-contract probe. Optional explicit output supports exact comparison
// against an original engine; default execution writes no files.
#include "gba_audio.h"
#include "snapshot.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

using Audio = gba::GbaAudio;
using Writer = gbarecomp::debug::SnapshotWriter;
void check(bool condition, const char* what) {
    if (!condition) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}
auto capture(const Audio& audio, std::uint64_t start, std::size_t maximum) {
    std::vector<Audio::CapSample> samples(maximum);
    std::uint64_t first = 0;
    const auto count = audio.query_capture(start,maximum,samples.data(),first);
    Writer result;
    result.u64(first); result.u32(static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        result.u16(samples[i].mixed);
        for (auto value : samples[i].ch) result.u16(value);
        result.u16(samples[i].direct_a); result.u16(samples[i].direct_b);
    }
    return result.buffer(); // return an independent vector
}
void emit(Writer& transcript, const std::vector<std::uint8_t>& bytes) {
    transcript.u32(static_cast<std::uint32_t>(bytes.size()));
    transcript.bytes(bytes.data(),bytes.size());
}
void generate(Audio& audio, unsigned events) {
    for (unsigned i = 0; i < events; ++i) {
        audio.write_io16(0x088,0x0200 | (((i / 127) % 4) << 14));
        audio.tick(1024);
    }
}
int main(int argc, char** argv) {
    check(argc <= 2,"usage: gba_audio_capture_qualification [explicit-output-file]");
    Audio audio;
    audio.write_io16(0x084,0x0080);
    audio.write_io16(0x080,0x2277);
    audio.write_io16(0x068,0xf080);
    audio.write_io16(0x06c,0x87c0);
    generate(audio,1000);
    check(audio.samples_generated() > 4096,"capture did not cross a storage boundary");
    Writer transcript;
    const auto initial = capture(audio,0,8192);
    emit(transcript,initial);
    emit(transcript,capture(audio,4090,32));
    auto copy = audio; // copying must retain independent capture history
    Writer snapshot;
    audio.serialize(snapshot);
    Audio restored;
    gbarecomp::debug::SnapshotReader reader(snapshot.buffer().data(),snapshot.buffer().size());
    restored.deserialize(reader);
    check(reader.ok(),"current-build audio snapshot rejected");
    std::vector<Audio::CapSample> zeros(8192);
    std::uint64_t first = 0;
    const auto count = restored.query_capture(0,zeros.size(),zeros.data(),first);
    check(count == restored.samples_generated(),"fresh restore capture extent changed");
    for (std::size_t i = 0; i < count; ++i) {
        const auto& s = zeros[i];
        check(s.mixed == 0 && s.direct_a == 0 && s.direct_b == 0 &&
              std::all_of(std::begin(s.ch),std::end(s.ch),[](auto v){return v == 0;}),
              "fresh restored capture must retain original zero-filled semantics");
    }
    emit(transcript,capture(restored,0,8192));
    generate(restored,900);
    emit(transcript,capture(restored,0,16384));
    while (audio.samples_generated() < Audio::kCapRingSize + 8192) generate(audio,1000);
    emit(transcript,capture(audio,0,Audio::kCapRingSize));
    emit(transcript,capture(audio,audio.capture_oldest_index()+4088,48));
    check(capture(copy,0,8192) == initial,"copy aliased capture storage after ring wrap");
    emit(transcript,capture(copy,0,8192));
    audio.reset();
    emit(transcript,capture(audio,0,8192));
    audio.tick(1024);
    // Restore into a previously used device: capture history was never serialized
    // or cleared by deserialize, so existing values must remain observable.
    gbarecomp::debug::SnapshotReader again(snapshot.buffer().data(),snapshot.buffer().size());
    audio.deserialize(again);
    check(again.ok(),"reused-device restore rejected");
    emit(transcript,capture(audio,0,8192));
    Writer before, after;
    audio.serialize(before);
    const auto repeat = capture(audio,4090,32);
    check(capture(audio,4090,32) == repeat,"capture query consumed data");
    emit(transcript,repeat);
    emit(transcript,capture(audio,0,0));
    emit(transcript,capture(audio,audio.samples_generated()+17,32));
    audio.serialize(after);
    check(before.buffer() == after.buffer(),"capture query altered simulation/playback state");
    std::array<std::int16_t,16384> pcm{};
    const auto pcm_count = audio.drain_samples(pcm.data(),pcm.size());
    transcript.u32(static_cast<std::uint32_t>(pcm_count));
    for (std::size_t i = 0; i < pcm_count; ++i) transcript.u16(pcm[i]);
    check(std::any_of(pcm.begin(),pcm.begin()+pcm_count,[](auto v){return v != 0;}),
          "capture probe did not exercise audible synthesis");
    std::uint64_t digest = 14695981039346656037ull;
    for (auto byte : transcript.buffer()) digest = (digest ^ byte)*1099511628211ull;
    if (argc == 2) {
        std::ofstream file(argv[1],std::ios::binary);
        file.write(reinterpret_cast<const char*>(transcript.buffer().data()),transcript.buffer().size());
        check(bool(file),"cannot write explicit capture qualification output");
    }
    std::printf("capture qualification passed bytes=%zu digest=%016llx\n",
        transcript.buffer().size(),static_cast<unsigned long long>(digest));
}
