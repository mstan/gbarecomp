#include "host_capture.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace gbarecomp::capture {
namespace {

std::string g_out_dir, g_wav_path, g_script_path;
int g_w = 0, g_h = 0;
uint64_t g_from = 0, g_to = UINT64_MAX, g_every = 1, g_exit_at = UINT64_MAX;
bool g_ppm = false, g_requested = false, g_finished = false, g_failed = false;
uint64_t g_present = 0, g_written = 0, g_wav_frames = 0;
int g_last_w = 0, g_last_h = 0;
std::FILE* g_wav = nullptr;
bool g_wav_open_tried = false;
PngWriter g_png = nullptr;

// Linear resampler state (the mix rate is constant per game).
double g_pos = 0.0;
int16_t g_prev[2] = {0, 0};
std::vector<int16_t> g_out;

bool wav_header(std::FILE* f, uint32_t data_bytes) {
    uint8_t h[44];
    const uint32_t rate = 48000, byte_rate = 48000 * 4, riff = 36 + data_bytes;
    std::memcpy(h, "RIFF", 4);
    std::memcpy(h + 4, &riff, 4);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t v32 = 16;
    std::memcpy(h + 16, &v32, 4);
    uint16_t v16 = 1;
    std::memcpy(h + 20, &v16, 2);
    v16 = 2;
    std::memcpy(h + 22, &v16, 2);
    std::memcpy(h + 24, &rate, 4);
    std::memcpy(h + 28, &byte_rate, 4);
    v16 = 4;
    std::memcpy(h + 32, &v16, 2);
    v16 = 16;
    std::memcpy(h + 34, &v16, 2);
    std::memcpy(h + 36, "data", 4);
    std::memcpy(h + 40, &data_bytes, 4);
    return std::fwrite(h, 1, sizeof(h), f) == sizeof(h);
}

bool in_range(uint64_t f) {
    return f >= g_from && f <= g_to && ((f - g_from) % g_every) == 0;
}

uint16_t button_bit(const std::string& t) {
    static const struct {
        const char* n;
        uint16_t bit;
    } k[] = {
        {"A", 1u << 0},     {"B", 1u << 1},    {"SELECT", 1u << 2}, {"START", 1u << 3},
        {"RIGHT", 1u << 4}, {"LEFT", 1u << 5}, {"UP", 1u << 6},     {"DOWN", 1u << 7},
        {"R", 1u << 8},     {"L", 1u << 9},
    };
    for (const auto& e : k)
        if (t == e.n) return e.bit;
    return 0;
}

}  // namespace

bool parse_arg(int argc, char** argv, int* i) {
    const std::string a = argv[*i];
    const bool ours =
        a.rfind("--capture-", 0) == 0 || a == "--input-script" || a == "--exit-at-frame";
    if (!ours) return false;
    if (*i + 1 >= argc) {
        std::fprintf(stderr, "gbarecomp capture: %s needs a value\n", a.c_str());
        return false;
    }
    const char* v = argv[*i + 1];
    if (a == "--capture-out") {
        g_out_dir = v;
    } else if (a == "--capture-size") {
        int w = 0, h = 0;
        if (std::sscanf(v, "%dx%d", &w, &h) != 2 || w < 64 || h < 64 || w > 16384 || h > 16384) {
            std::fprintf(stderr, "gbarecomp capture: bad --capture-size '%s' (want WxH)\n", v);
            return false;
        }
        g_w = w;
        g_h = h;
    } else if (a == "--capture-from") {
        g_from = std::strtoull(v, nullptr, 10);
    } else if (a == "--capture-to") {
        g_to = std::strtoull(v, nullptr, 10);
    } else if (a == "--capture-every") {
        g_every = std::strtoull(v, nullptr, 10);
        if (g_every < 1) g_every = 1;
    } else if (a == "--capture-format") {
        g_ppm = std::strcmp(v, "ppm") == 0;
    } else if (a == "--capture-wav") {
        g_wav_path = v;
    } else if (a == "--input-script") {
        g_script_path = v;
    } else if (a == "--exit-at-frame") {
        g_exit_at = std::strtoull(v, nullptr, 10);
    } else {
        std::fprintf(stderr, "gbarecomp capture: unknown option %s\n", a.c_str());
        return false;
    }
    ++*i;
    g_requested = true;
    return true;
}

bool requested() { return g_requested; }
void window_size(int* w, int* h) {
    if (w) *w = g_w;
    if (h) *h = g_h;
}
uint64_t frame_index() { return g_present; }
void set_png_writer(PngWriter writer) { g_png = writer; }

bool load_script(std::vector<InputEvent>* out) {
    if (g_script_path.empty()) return true;
    std::FILE* f = std::fopen(g_script_path.c_str(), "r");
    if (!f) {
        std::fprintf(stderr, "gbarecomp capture: cannot open %s\n", g_script_path.c_str());
        return false;
    }
    char line[512];
    int n = 0;
    uint64_t last = 0;
    while (std::fgets(line, sizeof line, f)) {
        ++n;
        char* h = std::strchr(line, '#');
        if (h) *h = 0;
        char* s = line;
        while (*s && std::isspace((unsigned char)*s)) ++s;
        if (!*s) continue;
        if (!std::isdigit((unsigned char)*s)) {
            std::fprintf(stderr, "gbarecomp capture: %s:%d: expected frame number\n",
                         g_script_path.c_str(), n);
            std::fclose(f);
            return false;
        }
        const uint64_t frame = std::strtoull(s, &s, 10);
        while (*s && std::isspace((unsigned char)*s)) ++s;
        if (std::isdigit((unsigned char)*s) && s[1] == ':') s += 2;  // "P:" prefix; GBA has one pad
        uint16_t word = 0x03FF;
        for (;;) {
            while (*s && (std::isspace((unsigned char)*s) || *s == '+' || *s == ',')) ++s;
            std::string tok;
            while (*s && !std::isspace((unsigned char)*s) && *s != '+' && *s != ',')
                tok += (char)std::toupper((unsigned char)*s++);
            if (tok.empty()) break;
            if (tok == "-" || tok == "NONE") continue;
            const uint16_t bit = button_bit(tok);
            if (!bit) {
                std::fprintf(stderr, "gbarecomp capture: %s:%d: unknown button '%s'\n",
                             g_script_path.c_str(), n, tok.c_str());
                std::fclose(f);
                return false;
            }
            word &= (uint16_t)~bit;
        }
        if (frame < last) {
            std::fprintf(stderr, "gbarecomp capture: %s:%d: frames must not decrease\n",
                         g_script_path.c_str(), n);
            std::fclose(f);
            return false;
        }
        last = frame;
        out->push_back({frame, word});
    }
    std::fclose(f);
    return true;
}

bool failed() { return g_failed; }

bool want_readback() {
    return g_requested && !g_failed && !g_out_dir.empty() && in_range(g_present);
}

void on_frame(const uint8_t* rgb, int w, int h) {
    const uint64_t f = g_present++;
    if (g_failed || g_out_dir.empty() || !in_range(f)) return;
    if (!rgb || w <= 0 || h <= 0) {
        std::fprintf(stderr, "gbarecomp capture: no presented pixels for frame %llu\n",
                     static_cast<unsigned long long>(f));
        g_failed = true;
        return;
    }
    if (g_written == 0) {
        std::error_code ec;
        std::filesystem::create_directories(g_out_dir, ec);
        if (ec) {
            std::fprintf(stderr, "gbarecomp capture: cannot create %s: %s\n",
                         g_out_dir.c_str(), ec.message().c_str());
            g_failed = true;
            return;
        }
    }
    char name[1100];
    const int length = std::snprintf(name, sizeof name, "%s/frame_%06llu.%s", g_out_dir.c_str(),
                                     (unsigned long long)f, g_ppm ? "ppm" : "png");
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof name) {
        std::fprintf(stderr, "gbarecomp capture: frame path too long in %s\n", g_out_dir.c_str());
        g_failed = true;
        return;
    }
    g_last_w = w;
    g_last_h = h;
    bool wrote = false;
    if (g_ppm) {
        std::FILE* fp = std::fopen(name, "wb");
        if (fp) {
            const std::size_t bytes = static_cast<std::size_t>(w) * h * 3;
            wrote = std::fprintf(fp, "P6\n%d %d\n255\n", w, h) > 0 &&
                    std::fwrite(rgb, 1, bytes, fp) == bytes;
            wrote = std::fclose(fp) == 0 && wrote;
        }
    } else if (g_png) {
        wrote = g_png(name, rgb, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    }
    if (wrote) {
        ++g_written;
    } else {
        std::fprintf(stderr, "gbarecomp capture: cannot write %s\n", name);
        g_failed = true;
    }
}

void on_audio(const int16_t* stereo, std::size_t frames, uint32_t rate) {
    if (!g_requested || g_failed || g_wav_path.empty() || !stereo || !frames || rate == 0) return;
    // The audio block follows the present, so the counter is one past the
    // frame that produced it. An open-ended range has no upper check.
    if (g_present < g_from || (g_to != UINT64_MAX && g_present > g_to + 1))
        return;
    if (!g_wav_open_tried) {
        g_wav_open_tried = true;
        g_wav = std::fopen(g_wav_path.c_str(), "wb");
        if (!g_wav || !wav_header(g_wav, 0)) {
            std::fprintf(stderr, "gbarecomp capture: cannot start WAV %s\n", g_wav_path.c_str());
            g_failed = true;
            return;
        }
    }
    if (!g_wav) return;
    const double step = (double)rate / 48000.0;
    g_out.clear();
    const std::size_t n = frames;
    // g_pos is the output sample position in source frames from the start of
    // this block; g_prev bridges the block boundary.
    while (g_pos < (double)n) {
        const std::size_t i1 = (std::size_t)g_pos;
        const double fr = g_pos - (double)i1;
        for (int c = 0; c < 2; ++c) {
            const double s0 = (i1 == 0) ? g_prev[c] : stereo[(i1 - 1) * 2 + c];
            const double s1 = stereo[i1 * 2 + c];
            g_out.push_back((int16_t)(s0 + (s1 - s0) * fr));
        }
        g_pos += step;
    }
    g_pos -= (double)n;
    g_prev[0] = stereo[(n - 1) * 2];
    g_prev[1] = stereo[(n - 1) * 2 + 1];
    const std::size_t pending = g_out.size() / 2;
    const std::size_t written = std::fwrite(g_out.data(), 4, pending, g_wav);
    g_wav_frames += written;
    if (written != pending) {
        std::fprintf(stderr, "gbarecomp capture: WAV sample write failed\n");
        g_failed = true;
    }
}

bool should_exit() {
    if (!g_requested) return false;
    const uint64_t end = g_exit_at < g_to ? g_exit_at : g_to;
    return g_failed || (end != UINT64_MAX && g_present > end);
}

void finish() {
    if (!g_requested || g_finished) return;
    g_finished = true;
    if (g_wav) {
        const bool header_ok = std::fseek(g_wav, 0, SEEK_SET) == 0 &&
            wav_header(g_wav, static_cast<uint32_t>(g_wav_frames * 4));
        if (std::fclose(g_wav) != 0 || !header_ok) {
            std::fprintf(stderr, "gbarecomp capture: WAV finalization failed\n");
            g_failed = true;
        }
        g_wav = nullptr;
    }
    std::printf(
        "gbarecomp capture: done frames_written=%llu audio_samples=%llu present_size=%dx%d "
        "presents=%llu\n",
        (unsigned long long)g_written, (unsigned long long)g_wav_frames, g_last_w, g_last_h,
        (unsigned long long)g_present);
    std::fflush(stdout);
}

}  // namespace gbarecomp::capture
