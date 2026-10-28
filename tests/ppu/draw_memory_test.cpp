#include "gba_draw_memory.h"
#include "gba_native_draw_frame.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

int main() {
    using Memory = gba::GbaDrawMemory;
    auto journal = std::make_unique<Memory>();
    std::vector<std::uint8_t> live(Memory::kVramBytes), replica(live.size());
    std::uint64_t sequence = 0;
    auto initial = journal->publish(live);
    CHECK(initial.reset && initial.payload.size() == live.size());
    CHECK(Memory::apply(initial, replica, sequence) && replica == live);
    CHECK(!journal->mark(live.size(), 1));
    CHECK(!journal->mark(std::size_t(-1), 1));
    CHECK(!journal->mark(1, std::size_t(-1)));
    CHECK(journal->mark(live.size(), 0));
    CHECK(journal->mark(0, live.size()));
    auto unchanged = journal->publish(live);
    CHECK(unchanged.ranges.empty() && unchanged.payload.empty());
    CHECK(Memory::apply(unchanged, replica, sequence));
    bool invalid_size = false;
    try { journal->publish(std::span(live).first(1)); }
    catch (const std::invalid_argument&) { invalid_size = true; }
    CHECK(invalid_size);

    std::mt19937 random(0x4a4f4253);
    std::vector<Memory::Delta> owned;
    std::vector<std::vector<std::uint8_t>> expected;
    unsigned transactions = 0;
    for (unsigned i = 0; i < 6000; ++i) {
        const auto offset = random() % live.size();
        const auto size = std::min<std::size_t>(1 + random() % 517, live.size() - offset);
        for (std::size_t j = 0; j < size; ++j) live[offset+j] = std::uint8_t(random());
        CHECK(journal->mark(offset, size));
        if (i % 17 == 0 || i == 5999) {
            auto delta = journal->publish(live);
            CHECK(Memory::apply(delta, replica, sequence));
            CHECK(replica == live && sequence == journal->sequence());
            owned.push_back(std::move(delta)); expected.push_back(live); ++transactions;
        }
    }
    // Replay old immutable transactions after thousands of later bus writes.
    replica.assign(replica.size(), 0); sequence = 777;
    CHECK(Memory::apply(initial, replica, sequence));
    for (std::size_t i = 0; i < owned.size(); ++i) {
        CHECK(Memory::apply(owned[i], replica, sequence));
        CHECK(replica == expected[i]);
    }
    journal->invalidate();
    live[91] ^= 0xa5; // Restore/context-loss need not call tracked stores.
    auto reset = journal->publish(live);
    CHECK(reset.reset);
    sequence = 123456;
    CHECK(Memory::apply(reset, replica, sequence) && replica == live);

    // Every malformed packet must decline before either bytes or sequence change.
    auto reject = [&](const Memory::Delta& delta) {
        const auto old = replica; const auto old_sequence = sequence;
        CHECK(!Memory::apply(delta, replica, sequence));
        CHECK(replica == old && sequence == old_sequence);
    };
    Memory::Delta valid;
    valid.before = sequence; valid.after = sequence + 1;
    valid.ranges = {{0, 2, 0}, {10, 1, 2}}; valid.payload = {1, 2, 3};
    auto bad = valid; ++bad.before; reject(bad);
    bad = valid; ++bad.after; reject(bad);
    bad = valid; bad.ranges[1].offset = 1; reject(bad);
    bad = valid; bad.ranges[1].offset = std::uint32_t(live.size()); reject(bad);
    bad = valid; bad.ranges[1].size = 0; reject(bad);
    bad = valid; bad.ranges[1].payload_offset = 0; reject(bad);
    bad = valid; bad.ranges[1].size = 3; reject(bad);
    bad = valid; bad.payload.push_back(4); reject(bad);
    bad = valid; bad.reset = true; reject(bad);
    bad = valid; bad.before = UINT64_MAX; bad.after = 0; reject(bad);
    bad = valid; bad.ranges.clear(); reject(bad);
    CHECK(Memory::apply(valid, replica, sequence));
    CHECK(replica[0] == 1 && replica[1] == 2 && replica[10] == 3);
    std::printf("draw memory: 6000 writes, %u owned transactions, reset/replay and atomic rejection passed\n", transactions);

    // Retain every actual raster observation, rather than rebuilding an old
    // frame from memory that VBlank or a later HBlank has already changed.
    gba::GbaNativeDrawBuilder builder;
    std::array<std::uint8_t,0x60> io{};
    std::array<std::uint8_t,1024> oam{}, palette{};
    CHECK(!builder.capture(1,0,io,oam,palette,live,*journal));
    CHECK(!builder.capture(0,0,std::span(io).first(1),oam,palette,live,*journal));
    CHECK(!builder.finish());
    expected.clear();
    for (unsigned y=0;y<160;++y) {
        const auto offset=(y*997u)%(live.size()-4);
        for (unsigned j=0;j<4;++j) live[offset+j]=std::uint8_t(y+j+13);
        CHECK(journal->mark(offset,4));
        io.fill(std::uint8_t(y)); oam.fill(std::uint8_t(y^0x95)); palette.fill(std::uint8_t(y^0x53));
        CHECK(builder.capture(y,std::uint16_t(y+100),io,oam,palette,live,*journal));
        expected.push_back(live);
        if (y<159) CHECK(!builder.finish());
    }
    auto frame=builder.finish();CHECK(frame && frame->lines().size()==160);
    CHECK(frame->payload_bytes()<=Memory::kVramBytes+160*64);
    std::fill(live.begin(),live.end(),0xee); io.fill(0xee); oam.fill(0xee); palette.fill(0xee);
    sequence=888;
    for (unsigned y=0;y<160;++y) {
        const auto& line=frame->lines()[y];
        CHECK(Memory::apply(line.memory,replica,sequence)); CHECK(replica==expected[y]);
        CHECK(line.dispcnt==y+100);
        CHECK(std::all_of(line.io.begin(),line.io.end(),[&](auto value){return value==y;}));
        CHECK(std::all_of(line.oam.begin(),line.oam.end(),[&](auto value){return value==(y^0x95);}));
        CHECK(std::all_of(line.palette.begin(),line.palette.end(),[&](auto value){return value==(y^0x53);}));
    }
    CHECK(!builder.capture(1,0,io,oam,palette,live,*journal));
    CHECK(builder.capture(0,0,io,oam,palette,live,*journal));builder.reset();CHECK(!builder.finish());
    CHECK(frame->lines()[0].io[0]==0 && frame->lines()[159].palette[0]==(159^0x53));
    std::puts("draw frame:160 historical rows and independent VRAM/IO/OAM/palette inputs survive later writes and builder reset");
}
