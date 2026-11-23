// diagnostic_replay.cpp — see diagnostic_replay.h and docs/DIAGNOSTIC_REPLAY.md.

#include "diagnostic_replay.h"
#include "diagnostic_replay_setting.h"

#include "replay_writer.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <utility>
#include <system_error>
#include <thread>
#include <chrono>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#define diag_pid() _getpid()
#else
#include <unistd.h>
#define diag_pid() getpid()
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if defined(__has_include)
#if __has_include("engine_revision.h")
#include "engine_revision.h"  // generated per build (cmake/engine_revision.cmake)
#endif
#endif
#ifndef GBARECOMP_ENGINE_REVISION
#define GBARECOMP_ENGINE_REVISION "unknown"
#endif

namespace gbarecomp {

namespace fs = std::filesystem;
using debug::ReplayHash;
using debug::ReplayWriter;

namespace {

constexpr const char* kMagic = "GBARECOMP-REPLAY 1";

std::string segment_path(const std::string& stem, unsigned segment) {
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "-%06u.replay", segment);
    return stem + suffix;
}

bool probe_directory(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path probe = dir / (".probe-" + std::to_string(diag_pid()));
    {
        std::ofstream f(probe, std::ios::binary | std::ios::trunc);
        if (!f) return false;
    }
    fs::remove(probe, ec);
    return true;
}

std::string format_input(std::uint64_t frame, const DiagnosticFrameInput& in,
                         char site) {
    char line[192];
    std::snprintf(line, sizeof(line),
                  "I %llu %04x %d %d %lld %u %u %u %u %u %u %c\n",
                  static_cast<unsigned long long>(frame), in.keyinput, in.gyro,
                  in.solar, static_cast<long long>(in.rtc), in.width, in.height,
                  in.extra_left, in.extra_right, in.extra_top, in.extra_bottom, site);
    return line;
}

bool parse_input(const std::string& line, std::uint64_t& frame,
                 DiagnosticFrameInput& in, char& site) {
    unsigned long long f = 0;
    unsigned keys = 0, w = 0, h = 0, l = 0, r = 0, t = 0, b = 0;
    int gyro = 0, solar = 0;
    long long rtc = 0;
    site = 0;
    if (std::sscanf(line.c_str(), "I %llu %x %d %d %lld %u %u %u %u %u %u %c", &f,
                    &keys, &gyro, &solar, &rtc, &w, &h, &l, &r, &t, &b, &site) != 12 ||
        (site != 'h' && site != 'l') ||
        keys > 0x03FFu || solar < -1 || solar > 255 || w < 240 || h < 160 ||
        w > 0xFFFFu || h > 0xFFFFu || l + r + 240 != w || t + b + 160 != h)
        return false;
    frame = f;
    in.keyinput = static_cast<std::uint16_t>(keys);
    in.gyro = gyro;
    in.solar = solar;
    in.rtc = rtc;
    in.width = static_cast<std::uint16_t>(w);
    in.height = static_cast<std::uint16_t>(h);
    in.extra_left = static_cast<std::uint16_t>(l);
    in.extra_right = static_cast<std::uint16_t>(r);
    in.extra_top = static_cast<std::uint16_t>(t);
    in.extra_bottom = static_cast<std::uint16_t>(b);
    return true;
}

}  // namespace

struct DiagnosticReplay::Playback {
    std::string stem;
    unsigned segment = 0;
    std::FILE* text = nullptr;
    std::FILE* states = nullptr;
    bool first_checkpoint = true;  // nothing has synchronized the machine yet
    bool resync_allowed = true;    // a gap/start precedes the next checkpoint
    bool have_checkpoint = false;  // an S record awaits its I record
    char checkpoint_kind = 'C';
    std::uint64_t checkpoint_frame = 0;
    std::vector<std::uint8_t> checkpoint;
    std::uint64_t last_checkpoint_frame = 0;
    std::string timing;
    std::string heal_set;      // pending N/O keys for the next input record
    bool heal_exact = false;   // N seen: replace rather than extend
    bool heal_pending = false; // O seen
    unsigned last_checkpoint_segment = 0;

    ~Playback() { close(); }
    void close() {
        if (text) std::fclose(text);
        if (states) std::fclose(states);
        text = states = nullptr;
    }
    bool next_line(std::string& out) {
        out.clear();
        if (!text) return false;
        char buf[512];
        while (std::fgets(buf, sizeof(buf), text)) {
            out += buf;
            if (!out.empty() && out.back() == '\n') break;
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            out.pop_back();
        return !out.empty();
    }
};

DiagnosticReplay::DiagnosticReplay() = default;

DiagnosticReplay::~DiagnosticReplay() { shutdown(); }

// ── Recording ────────────────────────────────────────────────────────────

bool DiagnosticReplay::start_recording(const DiagnosticReplayConfig& config,
                                       DiagnosticReplayHost host) {
    config_ = config;
    host_ = std::move(host);
    writer_ = ReplayWriter::create();
    if (!writer_) {
        std::fprintf(stderr, "[gbarecomp:diagnostics] replay capture unavailable "
                             "on this host (no worker thread)\n");
        return false;
    }
    std::error_code ec;
    fs::path dir = config_.directory.empty() ? fs::path("diagnostics") / "replays"
                                             : fs::path(config_.directory);
    if (!probe_directory(dir)) {
        dir = fs::temp_directory_path(ec) / (config_.tag.empty() ? "gbarecomp" : config_.tag) /
              "replays";
        if (ec || !probe_directory(dir)) {
            std::fprintf(stderr, "[gbarecomp:diagnostics] replay capture disabled: "
                                 "no writable diagnostics directory\n");
            writer_.reset();
            return false;
        }
    }
    dir = fs::absolute(dir, ec);
    directory_ = dir.string();
    char stamp[32] = "session";
    const std::time_t now = std::time(nullptr);
    if (const std::tm* t = std::localtime(&now))
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", t);
    stem_ = (dir / ((config_.tag.empty() ? std::string("gbarecomp") : config_.tag) +
                    "-" + stamp + "-" + std::to_string(diag_pid())))
                .string();
    history_sizes_.assign(kSegments, 0);
    recording_ = true;
    return true;
}

void DiagnosticReplay::stop_recording(const char* why) {
    if (!recording_) return;
    std::fprintf(stderr, "[gbarecomp:diagnostics] replay capture stopped: %s\n",
                 why ? why : "unknown error");
    recording_ = false;
    segment_open_ = false;
    pending_end_ = false;
    if (writer_) writer_->end();
    if (host_.heal_release) host_.heal_release();
}

void DiagnosticReplay::note_writer_failure() {
    if (writer_ && writer_->status() == ReplayWriter::Status::Full) {
        // Recoverable: abandon this segment and resume with a fresh
        // checkpoint once the worker has drained half the queue.
        ++gaps_;
        if ((gaps_ & (gaps_ - 1)) == 0)
            std::fprintf(stderr,
                         "[gbarecomp:diagnostics] storage fell behind at frame %llu; "
                         "replay resumes with a new checkpoint (gaps=%llu)\n",
                         static_cast<unsigned long long>(host_.frame()),
                         static_cast<unsigned long long>(gaps_));
        gap_ = true;
        // The abandoned segment stays open in the worker; once the queue
        // drains it receives a "G" record so playback knows frames are
        // missing there (rather than a hang or crash).
        gap_marker_pending_ = gap_marker_pending_ || segment_open_;
        segment_open_ = false;
        pending_end_ = false;
        return;
    }
    stop_recording(writer_ ? writer_->error() : "no writer");
}

bool DiagnosticReplay::evict_oldest() {
    static const char* const kFixed[] = {"", ".state"};
    const std::string base = segment_path(stem_, oldest_segment_);
    for (const char* suffix : kFixed)
        if (!writer_->remove(base + suffix)) { note_writer_failure(); return false; }
    for (const auto& sidecar : config_.sidecars)
        if (!writer_->remove(base + sidecar.second)) { note_writer_failure(); return false; }
    for (const auto& sidecar : config_.inline_sidecars)
        if (!writer_->remove(base + sidecar.second)) { note_writer_failure(); return false; }
    history_bytes_ -= history_sizes_[oldest_segment_ % kSegments];
    history_sizes_[oldest_segment_ % kSegments] = 0;
    ++oldest_segment_;
    return true;
}

bool DiagnosticReplay::start_segment() {
    if (segment_open_ && !writer_->end()) { note_writer_failure(); return false; }
    segment_open_ = false;
    ++segment_;
    while (segment_ - oldest_segment_ >= kSegments)
        if (!evict_oldest()) return false;
    segment_state_bytes_ = 0;
    const std::string path = segment_path(stem_, segment_);
    std::string header = kMagic;
    header += "\nBUILD ";
    header += config_.engine_revision.empty() ? "unknown" : config_.engine_revision;
    header += ' ';
    header += config_.executable_hash.empty() ? "unknown" : config_.executable_hash;
    header += ' ';
    header += config_.title.empty() ? "unknown" : config_.title;
    header += "\nROM ";
    header += config_.rom_sha1.empty() ? "unknown" : config_.rom_sha1;
    header += "\nSEGMENT " + std::to_string(segment_) + " " +
              std::to_string(static_cast<long long>(std::time(nullptr))) + "\n";
    std::vector<ReplayWriter::Sidecar> sidecars;
    for (const auto& s : config_.sidecars) sidecars.push_back({s.first, path + s.second});
    std::vector<ReplayWriter::Sidecar> texts;
    for (const auto& s : config_.inline_sidecars) texts.push_back({s.first, path + s.second});
    if (!writer_->begin(path, header, sidecars, texts)) {
        note_writer_failure();
        return false;
    }
    segment_open_ = true;
    rows_ = 0;
    if (segment_ == 1)
        std::fprintf(stderr,
                     "[gbarecomp:diagnostics] recording %s (background writer; "
                     "up to one hour, %zu MiB checkpoint budget; attach every "
                     "file of this session; GBARECOMP_DIAGNOSTICS=0 opts out)\n",
                     path.c_str(), config_.history_bytes / (1024u * 1024u));
    return true;
}

bool DiagnosticReplay::write_checkpoint(char kind) {
    std::string e;
    if (!host_.save_state(snapshot_, e)) {
        stop_recording(("cannot serialize machine checkpoint: " + e).c_str());
        return false;
    }
    const std::size_t size = snapshot_.size();
    if (size >= ReplayWriter::kQueueBytes / 2) {
        stop_recording("machine checkpoint exceeds the replay queue");
        return false;
    }
    while (oldest_segment_ < segment_ && history_bytes_ + size > config_.history_bytes)
        if (!evict_oldest()) return false;
    if (!writer_->snapshot(snapshot_.data(), size, kind)) {
        note_writer_failure();
        return false;
    }
    if (!writer_->line("T " + host_.timing_capture() + "\n")) {
        note_writer_failure();
        return false;
    }
    segment_state_bytes_ += size;
    history_bytes_ += size;
    history_sizes_[segment_ % kSegments] += size;
    full_save_next_ = true;
    return true;
}

void DiagnosticReplay::frame_begin(char site) {
    if (!recording_ && !playing_) return;
    if (skip_begin_) { skip_begin_ = false; return; }
    if (playing_) {
        playback_begin(site);
        boundary_frame_ = host_.frame();
        have_boundary_ = true;
        return;
    }
    // Recording keeps the same boundary bookkeeping as playback.
    struct Mark {
        DiagnosticReplay& d;
        ~Mark() { d.boundary_frame_ = d.host_.frame(); d.have_boundary_ = true; }
    } mark{*this};
    if (!recording_) return;
    if (writer_->status() == ReplayWriter::Status::Invalid ||
        writer_->status() == ReplayWriter::Status::Io) {
        stop_recording(writer_->error());
        return;
    }
    const std::uint64_t frame = host_.frame();
    const std::uint64_t epoch = host_.state_epoch();
    if (gap_) {
        if (writer_->free_bytes() < ReplayWriter::kQueueBytes / 2) return;
        if (gap_marker_pending_) {
            if (!writer_->line("G\n")) return;
            gap_marker_pending_ = false;
        }
        gap_ = false;
    }
    // A restore (savestate, rewind, suspend resume) changes the epoch. Frame
    // numbers alone are not a discontinuity: one deferred VBlank yield can
    // legitimately cover two guest frames, identically on replay.
    const bool discontinuity = segment_open_ && epoch != epoch_;
    const std::size_t segment_limit =
        std::min(kSegmentStateLimit, config_.history_bytes);
    const bool new_segment =
        !segment_open_ || rows_ >= kSegmentFrames ||
        (discontinuity && segment_state_bytes_ + snapshot_.size() > segment_limit);
    // Native code finished compiling since the last boundary starts running
    // here, at a point playback reproduces ("O"); every segment also lists
    // the whole native set ("N") so it replays on its own.
    const std::string adopted = host_.heal_adopt ? host_.heal_adopt() : std::string();
    if (new_segment) {
        if (!start_segment()) return;
        if (!write_checkpoint(discontinuity ? 'R' : 'C')) return;
        if (host_.heal_active &&
            !writer_->line("N " + host_.heal_active() + "\n")) {
            note_writer_failure();
            return;
        }
    } else {
        if (discontinuity && !write_checkpoint('R')) return;
        if (!adopted.empty() && !writer_->line("O " + adopted + "\n")) {
            note_writer_failure();
            return;
        }
    }
    epoch_ = epoch;
    DiagnosticFrameInput in;
    host_.capture_input(in);
    if (!writer_->line(format_input(frame, in, site))) { note_writer_failure(); return; }
    pending_end_ = true;
    next_frame_ = frame + 1;
}

// Debug aid for investigating a divergence: GBARECOMP_DIAGNOSTICS_DUMP_FRAMES=
// <first>-<last> writes the full machine at each of those frame boundaries
// (before host input) as <stem>-frame-<n>.<record|replay>.state, so the live
// and replayed machines can be diffed section by section. Synchronous I/O:
// never enabled by default.
void DiagnosticReplay::dump_frame_state() {
    static const std::pair<std::uint64_t, std::uint64_t> range = [] {
        unsigned long long a = 1, b = 0;
        if (const char* e = std::getenv("GBARECOMP_DIAGNOSTICS_DUMP_FRAMES"))
            if (std::sscanf(e, "%llu-%llu", &a, &b) != 2) a = 1, b = 0;
        return std::make_pair(static_cast<std::uint64_t>(a), static_cast<std::uint64_t>(b));
    }();
    const std::uint64_t frame = host_.frame();
    if (frame < range.first || frame > range.second || stem_.empty()) return;
    std::vector<std::uint8_t> state;
    std::string e;
    if (!host_.save_state(state, e)) return;
    const std::string path = stem_ + "-frame-" + std::to_string(frame) +
                             (playing_ ? ".replay.state" : ".record.state");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(state.data()),
              static_cast<std::streamsize>(state.size()));
}

bool DiagnosticReplay::repeated_boundary() const {
    return (recording_ || playing_) && have_boundary_ &&
           host_.frame() == boundary_frame_;
}

void DiagnosticReplay::frame_end() {
    if (!recording_ && !playing_) return;
    // A boundary where no guest frame ran since the last one (e.g. the runner
    // noticing a restored frame number) is not a frame: skip the pair.
    skip_begin_ = have_boundary_ && host_.frame() == boundary_frame_;
    if (skip_begin_) return;
    dump_frame_state();
    if (playing_) { playback_end(); return; }
    if (!pending_end_) return;
    pending_end_ = false;
    if (!recording_ || !segment_open_) return;
    if (host_.state_epoch() != epoch_) {
        // A restore landed mid-frame (a host poll between boundaries): this
        // frame's result was replaced, so it has no fingerprint. The next
        // boundary records the restored machine as an R checkpoint.
        if (!writer_->line("X " + std::to_string(host_.frame()) + "\n"))
            note_writer_failure();
        else
            ++rows_;
        return;
    }
    std::vector<DiagnosticRegion> regions;
    host_.regions(regions);
    ReplayWriter::Part parts[ReplayWriter::kMaxParts];
    unsigned count = 0;
    for (const auto& r : regions) {
        if (count + 1 >= ReplayWriter::kMaxParts) break;
        parts[count++] = {r.data, r.size, false};
    }
    const std::uint64_t seq = host_.save_write_seq();
    const bool reuse = !full_save_next_ && seq == save_seq_;
    if (!reuse) host_.save_memory(save_bytes_);
    parts[count++] = {reuse ? nullptr : save_bytes_.data(),
                      reuse ? 0u : save_bytes_.size(), reuse};
    if (!writer_->frame(host_.frame(), parts, count)) {
        note_writer_failure();
        return;
    }
    save_seq_ = seq;
    full_save_next_ = false;
    ++rows_;
}

void DiagnosticReplay::shutdown() {
    if (writer_) {
        // A clean exit ends the newest segment with "E", so playback can tell a
        // normal quit from a hang, crash or kill (no "E" record). Exit may wait
        // briefly for a slow disk; gameplay never does.
        if (recording_ && segment_) {
            auto put = [&](const std::string& text) {
                for (int i = 0; i < 500; ++i) {
                    if (writer_->line(text)) return true;
                    if (writer_->status() != ReplayWriter::Status::Full) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                return false;
            };
            bool ok = !gap_marker_pending_ || put("G\n");
            gap_marker_pending_ = false;
            if (ok) put("E " + std::to_string(host_.frame()) + "\n");
        }
        pending_end_ = false;
        if (segment_open_) writer_->end();
        segment_open_ = false;
        if (const char* why = writer_->shutdown()) {
            if (recording_)
                std::fprintf(stderr, "[gbarecomp:diagnostics] replay capture "
                                     "incomplete: %s\n", why);
        } else if (recording_ && segment_) {
            std::fprintf(stderr,
                         "[gbarecomp:diagnostics] replay saved: %s-*.replay "
                         "(segments %u..%u, gaps=%llu)\n",
                         stem_.c_str(), oldest_segment_, segment_,
                         static_cast<unsigned long long>(gaps_));
        }
        writer_.reset();
    }
    recording_ = false;
    if (playback_) playback_->close();
}

// ── Playback ─────────────────────────────────────────────────────────────

void DiagnosticReplay::fail_playback(const std::string& why) {
    if (failed_) return;
    failed_ = true;
    pending_end_ = false;
    std::fprintf(stderr, "[gbarecomp:diagnostics] replay FAILED at frame %llu: %s\n",
                 static_cast<unsigned long long>(host_.frame()), why.c_str());
    std::fflush(stderr);
    if (playback_) playback_->close();
}

bool DiagnosticReplay::start_playback(const std::string& path,
                                      const DiagnosticReplayConfig& config,
                                      DiagnosticReplayHost host) {
    config_ = config;
    host_ = std::move(host);
    playback_ = std::make_unique<Playback>();
    playing_ = true;
    static const std::regex kSegmentName(R"(^(.*)-([0-9]{6})\.replay$)");
    std::smatch m;
    const std::string full = fs::absolute(fs::path(path)).string();
    if (!std::regex_match(full, m, kSegmentName)) {
        fail_playback("replay path must name a <stem>-NNNNNN.replay segment");
        return false;
    }
    playback_->stem = m[1].str();
    stem_ = playback_->stem;
    const unsigned first = static_cast<unsigned>(std::stoul(m[2].str()));
    if (!playback_open_segment(first)) return false;
    std::fprintf(stderr, "[gbarecomp:diagnostics] replaying %s\n", full.c_str());
    return true;
}

bool DiagnosticReplay::playback_open_segment(unsigned segment) {
    Playback& p = *playback_;
    const std::string path = segment_path(p.stem, segment);
    std::error_code ec;
    if (!fs::exists(path, ec)) return false;
    p.close();
    p.text = std::fopen(path.c_str(), "rb");
    p.states = std::fopen((path + ".state").c_str(), "rb");
    if (!p.text || !p.states) {
        fail_playback("cannot open " + path + " and its .state checkpoint file");
        return false;
    }
    p.segment = segment;
    std::string magic, build, rom, seg;
    if (!p.next_line(magic) || magic != kMagic || !p.next_line(build) ||
        build.rfind("BUILD ", 0) != 0 || !p.next_line(rom) ||
        rom.rfind("ROM ", 0) != 0 || !p.next_line(seg) ||
        seg.rfind("SEGMENT ", 0) != 0) {
        fail_playback("invalid or truncated replay header in " + path);
        return false;
    }
    if (!config_.rom_sha1.empty() && rom.substr(4) != config_.rom_sha1) {
        fail_playback("recorded ROM " + rom.substr(4) + " differs from the loaded ROM " +
                      config_.rom_sha1);
        return false;
    }
    const std::string local = config_.engine_revision + " " + config_.executable_hash;
    if (build.substr(6).rfind(local, 0) != 0)
        std::fprintf(stderr,
                     "[gbarecomp:diagnostics] WARNING: recorded %s; replaying with "
                     "BUILD %s %s. A divergence may be a build difference.\n",
                     build.c_str(), config_.engine_revision.c_str(),
                     config_.executable_hash.c_str());
    return true;
}

bool DiagnosticReplay::playback_checkpoint(const std::string& line) {
    Playback& p = *playback_;
    unsigned long long offset = 0, size = 0, digest = 0;
    char kind = 0;
    if (std::sscanf(line.c_str(), "S %llu %llu %llx %c", &offset, &size, &digest,
                    &kind) != 4 ||
        (kind != 'C' && kind != 'R') || size == 0 || size > (64ull << 20)) {
        fail_playback("invalid checkpoint record: " + line);
        return false;
    }
    p.checkpoint.resize(static_cast<std::size_t>(size));
    if (std::fseek(p.states, static_cast<long>(offset), SEEK_SET) != 0 ||
        std::fread(p.checkpoint.data(), 1, p.checkpoint.size(), p.states) !=
            p.checkpoint.size() ||
        debug::replay_hash(p.checkpoint.data(), p.checkpoint.size()) != digest) {
        fail_playback("missing or damaged machine checkpoint in segment " +
                      std::to_string(p.segment));
        return false;
    }
    p.have_checkpoint = true;
    p.checkpoint_kind = kind;
    return true;
}

void DiagnosticReplay::playback_begin(char site) {
    if (finished_ || failed_) return;
    Playback& p = *playback_;
    std::string line;
    for (;;) {
        if (!p.next_line(line)) {
            if (p.text && std::ferror(p.text)) { fail_playback("replay read failed"); return; }
            if (!playback_open_segment(p.segment + 1)) {
                if (failed_) return;
                finished_ = true;
                p.close();
                std::fprintf(stderr,
                             "[gbarecomp:diagnostics] replay MATCHED: all %llu recorded "
                             "frames reproduced bit-identically (last segment %u)\n",
                             static_cast<unsigned long long>(matched_), p.segment);
                std::fflush(stderr);
                return;
            }
            continue;
        }
        if (line[0] == 'G') {  // frames lost to a storage gap follow
            p.resync_allowed = true;
            continue;
        }
        if (line[0] == 'E') {
            finished_ = true;
            p.close();
            std::fprintf(stderr,
                         "[gbarecomp:diagnostics] replay MATCHED: all %llu recorded "
                         "frames reproduced bit-identically; the session ended normally\n",
                         static_cast<unsigned long long>(matched_));
            std::fflush(stderr);
            return;
        }
        if (line[0] == 'S') {
            if (!playback_checkpoint(line)) return;
            p.timing.clear();
            continue;
        }
        if (line[0] == 'T') {
            p.timing = line.size() > 2 ? line.substr(2) : std::string();
            continue;
        }
        if (line[0] == 'N') {  // the complete native set at this boundary
            p.heal_set = line.size() > 2 ? line.substr(2) : std::string();
            p.heal_exact = true;
            continue;
        }
        if (line[0] == 'O') {  // native code adopted at this boundary
            p.heal_set += " " + (line.size() > 2 ? line.substr(2) : std::string());
            p.heal_pending = true;
            continue;
        }
        if (line[0] != 'I') { fail_playback("unexpected record: " + line); return; }
        std::uint64_t frame = 0;
        DiagnosticFrameInput in;
        char recorded_site = 0;
        if (!parse_input(line, frame, in, recorded_site)) {
            fail_playback("invalid input record: " + line);
            return;
        }
        if (!p.have_checkpoint && recorded_site != site) {
            fail_playback(std::string("FIRST DIVERGENCE: frame ") + std::to_string(frame) +
                          " reached at the " + (site == 'h' ? "present hook" : "runner loop") +
                          " but recorded at the " +
                          (recorded_site == 'h' ? "present hook" : "runner loop"));
            return;
        }
        // Inputs first: the recorder checkpointed after sampling them.
        host_.apply_input(in);
        if (p.have_checkpoint) {
            p.have_checkpoint = false;
            std::string e;
            const bool initial = p.first_checkpoint;
            p.first_checkpoint = false;
            // R: the live session restored this state here, so playback does
            // too. C: the live session merely continued; while playback is in
            // step (every fingerprint so far matched) there is nothing to do.
            // Checkpoint bytes are never compared: they also carry host-only
            // state (audio output ring, an idle RTC's wall clock).
            const bool load = p.checkpoint_kind == 'R' || p.resync_allowed;
            if (load) {
                if (!host_.load_state(p.checkpoint, e)) {
                    fail_playback("checkpoint does not load in this build: " + e);
                    return;
                }
                if (!p.timing.empty() && !host_.timing_restore(p.timing)) {
                    fail_playback("invalid scheduling record for the checkpoint");
                    return;
                }
                host_.apply_input(in);  // device inputs are host state
                // The restored machine resumes from its published PC with a
                // clean host stack, never inside this replay's call chain.
                host_.request_unwind();
                std::fprintf(stderr,
                             "[gbarecomp:diagnostics] replay %s checkpoint (segment %u, "
                             "frame %llu)\n",
                             p.checkpoint_kind == 'R' && !initial ? "restored recorded"
                                                                  : "synchronized to",
                             p.segment, static_cast<unsigned long long>(host_.frame()));
            }
            p.last_checkpoint_segment = p.segment;
            p.last_checkpoint_frame = host_.frame();
            p.resync_allowed = false;
        } else if (p.first_checkpoint) {
            fail_playback("segment " + std::to_string(p.segment) +
                          " does not begin with a machine checkpoint");
            return;
        }
        if ((p.heal_exact || p.heal_pending) && host_.heal_set_active) {
            // N replaces the native set; O adds to the current one.
            const std::string keys = p.heal_exact
                ? p.heal_set
                : (host_.heal_active ? host_.heal_active() : std::string()) + p.heal_set;
            std::string e;
            if (!host_.heal_set_active(keys, e))
                std::fprintf(stderr, "[gbarecomp:diagnostics] WARNING: %s; native-code "
                                     "coverage differs from the recording\n", e.c_str());
        }
        p.heal_set.clear();
        p.heal_exact = p.heal_pending = false;
        if (frame != host_.frame()) {
            fail_playback("FIRST DIVERGENCE: recorded input for frame " +
                          std::to_string(frame) + " but the machine reached frame " +
                          std::to_string(host_.frame()));
            return;
        }
        pending_end_ = true;
        return;
    }
}

void DiagnosticReplay::playback_end() {
    if (!pending_end_ || failed_ || finished_) return;
    pending_end_ = false;
    Playback& p = *playback_;
    std::string line;
    if (!p.next_line(line)) {
        std::error_code ec;
        if (fs::exists(segment_path(p.stem, p.segment + 1), ec)) {
            // The recorder abandoned this segment (storage gap). The next
            // segment begins with its own checkpoint.
            p.resync_allowed = true;
            return;
        }
        fail_playback("INCOMPLETE: the final recorded input has no fingerprint, so "
                      "the recorded frame never completed (hang, crash or kill). The "
                      "replay executed that input; this frame is where to look");
        return;
    }
    if (line[0] == 'X') return;  // recorded frame was replaced by a restore
    if (line[0] == 'G') {
        // The recorder lost this frame's fingerprint to a storage gap; the
        // next checkpoint resynchronizes.
        p.resync_allowed = true;
        return;
    }
    if (line[0] == 'E') {
        // The session quit normally before this input's frame ran.
        finished_ = true;
        p.close();
        std::fprintf(stderr,
                     "[gbarecomp:diagnostics] replay MATCHED: all %llu recorded frames "
                     "reproduced bit-identically; the session ended normally\n",
                     static_cast<unsigned long long>(matched_));
        std::fflush(stderr);
        return;
    }
    unsigned long long frame = 0;
    std::vector<std::uint64_t> expected;
    {
        const char* cursor = line.c_str();
        int used = 0;
        if (std::sscanf(cursor, "H %llu%n", &frame, &used) != 1) {
            fail_playback("invalid fingerprint record: " + line);
            return;
        }
        cursor += used;
        unsigned long long value = 0;
        while (std::sscanf(cursor, " %llx%n", &value, &used) == 1) {
            expected.push_back(value);
            cursor += used;
        }
    }
    std::vector<DiagnosticRegion> regions;
    host_.regions(regions);
    host_.save_memory(save_bytes_);
    regions.push_back({"save", save_bytes_.data(), save_bytes_.size()});
    if (expected.size() != regions.size()) {
        fail_playback("fingerprint layout differs from this build (" +
                      std::to_string(expected.size()) + " recorded regions, " +
                      std::to_string(regions.size()) + " here)");
        return;
    }
    bool same = frame == host_.frame();
    std::string detail, advisory;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const bool match =
            debug::replay_hash(regions[i].data, regions[i].size) == expected[i];
        if (regions[i].advisory) {
            if (!match) advisory += std::string(advisory.empty() ? "" : ",") + regions[i].name;
        } else {
            same = same && match;
        }
        detail += std::string(" ") + regions[i].name + (match ? "=match" : "=DIFFERS");
    }
    if (same) {
        ++matched_;
        if (!advisory.empty() && ++advisory_mismatches_ <= 3)
            std::fprintf(stderr,
                         "[gbarecomp:diagnostics] note: frame %llu matches; host "
                         "bookkeeping differs (%s), as expected after a resync\n",
                         frame, advisory.c_str());
        return;
    }
    std::fprintf(stderr,
                 "[gbarecomp:diagnostics] FIRST DIVERGENCE after recorded frame %llu "
                 "(machine frame %llu, %llu frames matched; last checkpoint segment "
                 "%u frame %llu):%s\n",
                 frame, static_cast<unsigned long long>(host_.frame()),
                 static_cast<unsigned long long>(matched_), p.last_checkpoint_segment,
                 static_cast<unsigned long long>(p.last_checkpoint_frame), detail.c_str());
    std::vector<std::uint8_t> state;
    std::string e;
    if (host_.save_state(state, e)) {
        std::ofstream out("replay-divergence.state", std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(state.data()),
                  static_cast<std::streamsize>(state.size()));
        if (out)
            std::fprintf(stderr, "[gbarecomp:diagnostics] saved the divergent machine "
                                 "to replay-divergence.state\n");
    }
    fail_playback("checkpoint mismatch");
}

// ── Identity / settings helpers ──────────────────────────────────────────

namespace {
fs::path executable_path() {
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    return fs::path(buf);
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    return fs::path(buf.c_str());
#elif defined(__linux__) || defined(__ANDROID__)
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path{} : p;
#else
    return {};
#endif
}
}  // namespace

std::string diagnostic_executable_hash() {
    static const std::string cached = [] {
        const fs::path path = executable_path();
        std::FILE* f = path.empty() ? nullptr : std::fopen(path.string().c_str(), "rb");
        if (!f) return std::string("unknown");
        ReplayHash h;
        std::vector<char> buf(1u << 20);
        std::size_t n;
        while ((n = std::fread(buf.data(), 1, buf.size(), f)) != 0) h.update(buf.data(), n);
        const bool ok = !std::ferror(f);
        std::fclose(f);
        if (!ok) return std::string("unknown");
        char hex[17];
        std::snprintf(hex, sizeof(hex), "%016llx",
                      static_cast<unsigned long long>(h.finish()));
        return std::string(hex);
    }();
    return cached;
}

const char* diagnostic_engine_revision() { return GBARECOMP_ENGINE_REVISION; }

bool diagnostic_replay_setting(const std::string& config_ini, bool fallback) {
    return diagnostic_replay_saved_setting(config_ini).value_or(fallback);
}

}  // namespace gbarecomp
