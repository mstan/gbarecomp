// snapshot.cpp — save-state container orchestrator. See snapshot.h.
//
// Container layout (all little-endian):
//
//   magic   "GBAS"              4 bytes
//   version u32                 == kSnapshotVersion
//   rom_sha1 40 bytes           ASCII hex; the load-time gate
//   section_count u32
//   sections[]                  each: tag u32 | payload_len u32 | payload
//
// Each subsystem owns its payload via serialize()/deserialize(); this
// file only frames them and enforces the header gate. Sections are
// keyed by a FourCC tag and looked up by tag on load, so reordering or
// an unknown trailing section degrades gracefully (the version gate is
// the hard guarantee that layouts match).

#include "snapshot.h"

#include <fstream>
#include <unordered_map>
#include <vector>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime_arm.h"

namespace gbarecomp::debug {

namespace {

constexpr char kMagic[4] = {'G', 'B', 'A', 'S'};

// FourCC section tags.
constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}
constexpr uint32_t TAG_CPU   = fourcc('C', 'P', 'U', '0');
constexpr uint32_t TAG_BUS   = fourcc('B', 'U', 'S', '0');
constexpr uint32_t TAG_IO    = fourcc('I', 'O', '_', '0');
constexpr uint32_t TAG_AUDIO = fourcc('A', 'U', 'D', '0');
constexpr uint32_t TAG_SAVE  = fourcc('S', 'A', 'V', '0');
constexpr uint32_t TAG_PPU   = fourcc('P', 'P', 'U', '0');
constexpr uint32_t TAG_META  = fourcc('M', 'E', 'T', 'A');

constexpr std::size_t kRomSha1Len = 40;

// Copy up to 40 hex chars of `sha1` into `buf[40]`, zero-padding the
// remainder. Shared by the write path and the load-time gate compare.
void fill_rom_sha1(char* buf, const char* sha1) {
    std::size_t n = 0;
    if (sha1) {
        for (; n < kRomSha1Len && sha1[n]; ++n) buf[n] = sha1[n];
    }
    for (std::size_t i = n; i < kRomSha1Len; ++i) buf[i] = 0;
}

// Append a tagged, length-prefixed section to `out`. `fill` writes the
// payload into a scratch writer; we copy its bytes in framed.
template <typename Fill>
void add_section(SnapshotWriter& out, uint32_t tag, Fill&& fill) {
    SnapshotWriter payload;
    fill(payload);
    out.u32(tag);
    out.u32(static_cast<uint32_t>(payload.size()));
    out.bytes(payload.buffer().data(), payload.size());
}

void serialize_cpu(SnapshotWriter& w) {
    w.bytes(&g_cpu, sizeof(g_cpu));
    uint32_t depth = runtime_call_stack_depth();
    const uint32_t* data = runtime_call_stack_data();
    w.u32(depth);
    for (uint32_t i = 0; i < depth; ++i) w.u32(data[i]);
}

void deserialize_cpu(SnapshotReader& r) {
    r.bytes(&g_cpu, sizeof(g_cpu));
    uint32_t depth = r.u32();
    std::vector<uint32_t> entries(depth, 0);
    for (uint32_t i = 0; i < depth; ++i) entries[i] = r.u32();
    runtime_call_stack_restore(entries.data(), depth);
}

void serialize_meta(SnapshotWriter& w, const SnapshotContext& ctx) {
    w.u64(ctx.taken          ? *ctx.taken          : 0);
    w.u64(ctx.cycles_elapsed ? *ctx.cycles_elapsed : 0);
    w.u64(ctx.vblank_count   ? *ctx.vblank_count   : 0);
}

void deserialize_meta(SnapshotReader& r, const SnapshotContext& ctx) {
    uint64_t taken  = r.u64();
    uint64_t cycles = r.u64();
    uint64_t vblank = r.u64();
    if (ctx.taken)          *ctx.taken          = taken;
    if (ctx.cycles_elapsed) *ctx.cycles_elapsed = cycles;
    if (ctx.vblank_count)   *ctx.vblank_count   = vblank;
}

}  // namespace

bool save_state(const char* path, const SnapshotContext& ctx, std::string* err) {
    if (!ctx.bus || !ctx.ppu) {
        if (err) *err = "snapshot: bus/ppu not wired";
        return false;
    }

    char sha1[kRomSha1Len];
    fill_rom_sha1(sha1, ctx.rom_sha1);

    SnapshotWriter w;
    w.bytes(kMagic, 4);
    w.u32(kSnapshotVersion);
    w.bytes(sha1, kRomSha1Len);
    w.u32(7);  // section_count

    add_section(w, TAG_CPU,   [&](SnapshotWriter& s) { serialize_cpu(s); });
    add_section(w, TAG_BUS,   [&](SnapshotWriter& s) { ctx.bus->serialize(s); });
    add_section(w, TAG_IO,    [&](SnapshotWriter& s) { ctx.bus->io().serialize(s); });
    add_section(w, TAG_AUDIO, [&](SnapshotWriter& s) { ctx.bus->audio().serialize(s); });
    add_section(w, TAG_SAVE,  [&](SnapshotWriter& s) { ctx.bus->save().serialize(s); });
    add_section(w, TAG_PPU,   [&](SnapshotWriter& s) { ctx.ppu->serialize(s); });
    add_section(w, TAG_META,  [&](SnapshotWriter& s) { serialize_meta(s, ctx); });

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (err) *err = std::string("snapshot: cannot open for write: ") + path;
        return false;
    }
    f.write(reinterpret_cast<const char*>(w.buffer().data()),
            static_cast<std::streamsize>(w.size()));
    if (!f) {
        if (err) *err = std::string("snapshot: write failed: ") + path;
        return false;
    }
    return true;
}

bool load_state(const char* path, const SnapshotContext& ctx, std::string* err) {
    if (!ctx.bus || !ctx.ppu) {
        if (err) *err = "snapshot: bus/ppu not wired";
        return false;
    }

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        if (err) *err = std::string("snapshot: cannot open for read: ") + path;
        return false;
    }
    std::streamoff size = f.tellg();
    if (size <= 0) {
        if (err) *err = std::string("snapshot: empty/unreadable: ") + path;
        return false;
    }
    std::vector<uint8_t> blob(static_cast<std::size_t>(size));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(blob.data()),
           static_cast<std::streamsize>(blob.size()));
    if (!f) {
        if (err) *err = std::string("snapshot: read failed: ") + path;
        return false;
    }

    SnapshotReader head(blob.data(), blob.size());
    char magic[4];
    head.bytes(magic, 4);
    if (std::memcmp(magic, kMagic, 4) != 0) {
        if (err) *err = "snapshot: bad magic (not a GBAS file)";
        return false;
    }
    uint32_t version = head.u32();
    if (version != kSnapshotVersion) {
        if (err) {
            *err = "snapshot: version mismatch (file v" +
                   std::to_string(version) + ", runtime v" +
                   std::to_string(kSnapshotVersion) + ")";
        }
        return false;
    }
    char file_sha1[kRomSha1Len];
    head.bytes(file_sha1, kRomSha1Len);
    if (ctx.rom_sha1 && *ctx.rom_sha1) {
        char want[kRomSha1Len];
        fill_rom_sha1(want, ctx.rom_sha1);
        if (std::memcmp(file_sha1, want, kRomSha1Len) != 0) {
            if (err) *err = "snapshot: ROM SHA-1 mismatch (state was saved "
                            "against a different ROM)";
            return false;
        }
    }
    uint32_t section_count = head.u32();
    if (!head.ok()) {
        if (err) *err = "snapshot: truncated header";
        return false;
    }

    // Index sections by tag → (offset, len) within blob.
    struct Span { std::size_t off; std::size_t len; };
    std::unordered_map<uint32_t, Span> sections;
    std::size_t cursor = blob.size() - head.remaining();
    for (uint32_t i = 0; i < section_count; ++i) {
        SnapshotReader sr(blob.data() + cursor, blob.size() - cursor);
        uint32_t tag = sr.u32();
        uint32_t len = sr.u32();
        if (!sr.ok()) {
            if (err) *err = "snapshot: truncated section header";
            return false;
        }
        std::size_t payload_off = cursor + 8;
        if (payload_off + len > blob.size()) {
            if (err) *err = "snapshot: section overruns file";
            return false;
        }
        sections[tag] = Span{payload_off, len};
        cursor = payload_off + len;
    }

    // All gameplay-relevant sections must be present. META is advisory.
    const uint32_t required[] = {TAG_CPU, TAG_BUS, TAG_IO,
                                 TAG_AUDIO, TAG_SAVE, TAG_PPU};
    for (uint32_t tag : required) {
        if (sections.find(tag) == sections.end()) {
            if (err) *err = "snapshot: missing required section";
            return false;
        }
    }

    auto reader_for = [&](uint32_t tag) -> SnapshotReader {
        auto it = sections.find(tag);
        const uint8_t* base = (it == sections.end())
            ? nullptr : blob.data() + it->second.off;
        std::size_t len = (it == sections.end()) ? 0 : it->second.len;
        return SnapshotReader(base, len);
    };

    { SnapshotReader r = reader_for(TAG_CPU);   deserialize_cpu(r); }
    { SnapshotReader r = reader_for(TAG_BUS);   ctx.bus->deserialize(r); }
    { SnapshotReader r = reader_for(TAG_IO);    ctx.bus->io().deserialize(r); }
    { SnapshotReader r = reader_for(TAG_AUDIO); ctx.bus->audio().deserialize(r); }
    { SnapshotReader r = reader_for(TAG_SAVE);  ctx.bus->save().deserialize(r); }
    { SnapshotReader r = reader_for(TAG_PPU);   ctx.ppu->deserialize(r); }
    if (sections.find(TAG_META) != sections.end()) {
        SnapshotReader r = reader_for(TAG_META);
        deserialize_meta(r, ctx);
    }

    return true;
}

}  // namespace gbarecomp::debug
