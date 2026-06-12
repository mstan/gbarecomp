// color_lut.cpp — see color_lut.h. Present-time only.
//
// Ported from JRickey/gba-recomp (crates/screen/src/{color,profile,lut}.rs),
// © Jrickey, MIT OR Apache-2.0, used with permission. See
// THIRD_PARTY_ATTRIBUTION.md.

#include "color_lut.h"

#include <cmath>

namespace gbarecomp::runtime {
namespace {

// ── CIE colorimetry (all f64; runs only at LUT-build time) ─────────
struct Xy { double x, y; };
struct Primaries { Xy red, green, blue, white; };
struct Mat3 { double m[3][3]; };

constexpr Xy kD65 = {0.3127, 0.3290};
constexpr Primaries kSrgb = {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}, kD65};
constexpr Primaries kDisplayP3 = {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, kD65};

// Measured reflective/frontlit gamut, and the near-sRGB backlit revision.
constexpr Primaries kPanelReflective = {
    {0.4925, 0.3100}, {0.3150, 0.4825}, {0.1625, 0.1925}, kD65};
constexpr Primaries kPanelBacklit = {
    {0.6191, 0.3454}, {0.3269, 0.6003}, {0.1436, 0.0893}, kD65};

bool xy_eq(Xy a, Xy b) { return a.x == b.x && a.y == b.y; }

void mat_apply(const Mat3& a, const double v[3], double out[3]) {
    for (int i = 0; i < 3; ++i)
        out[i] = a.m[i][0] * v[0] + a.m[i][1] * v[1] + a.m[i][2] * v[2];
}

Mat3 mat_mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

Mat3 mat_inverse(const Mat3& a) {
    const auto& m = a.m;
    auto cof = [&](int r, int c) {
        int r1 = (r + 1) % 3, r2 = (r + 2) % 3;
        int c1 = (c + 1) % 3, c2 = (c + 2) % 3;
        return m[r1][c1] * m[r2][c2] - m[r1][c2] * m[r2][c1];
    };
    double det = m[0][0] * cof(0, 0) + m[0][1] * cof(0, 1) + m[0][2] * cof(0, 2);
    Mat3 out{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out.m[i][j] = cof(j, i) / det;
    return out;
}

void xy_to_xyz(Xy c, double out[3]) {
    out[0] = c.x / c.y;
    out[1] = 1.0;
    out[2] = (1.0 - c.x - c.y) / c.y;
}

// Linear RGB → CIE XYZ for a set of primaries (white maps to Y=1).
Mat3 rgb_to_xyz(const Primaries& p) {
    double r[3], g[3], b[3], w[3];
    xy_to_xyz(p.red, r);
    xy_to_xyz(p.green, g);
    xy_to_xyz(p.blue, b);
    xy_to_xyz(p.white, w);
    Mat3 m = {{{r[0], g[0], b[0]}, {r[1], g[1], b[1]}, {r[2], g[2], b[2]}}};
    double s[3];
    mat_apply(mat_inverse(m), w, s);
    Mat3 out = m;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out.m[i][j] *= s[j];
    return out;
}

Mat3 bradford_adaptation(Xy from, Xy to) {
    constexpr Mat3 kBradford = {{{0.8951, 0.2664, -0.1614},
                                 {-0.7502, 1.7135, 0.0367},
                                 {0.0389, -0.0685, 1.0296}}};
    double f[3], t[3], src[3], dst[3];
    xy_to_xyz(from, f);
    xy_to_xyz(to, t);
    mat_apply(kBradford, f, src);
    mat_apply(kBradford, t, dst);
    Mat3 scale = {{{dst[0] / src[0], 0, 0},
                   {0, dst[1] / src[1], 0},
                   {0, 0, dst[2] / src[2]}}};
    return mat_mul(mat_mul(mat_inverse(kBradford), scale), kBradford);
}

Mat3 rgb_to_rgb(const Primaries& src, const Primaries& dst) {
    Mat3 to_xyz = rgb_to_xyz(src);
    Mat3 from_xyz = mat_inverse(rgb_to_xyz(dst));
    if (xy_eq(src.white, dst.white)) return mat_mul(from_xyz, to_xyz);
    return mat_mul(mat_mul(from_xyz, bradford_adaptation(src.white, dst.white)),
                   to_xyz);
}

double srgb_oetf(double v) {
    return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

// ── Panel optical model ────────────────────────────────────────────
struct PanelModel { Primaries primaries; double gamma, luminance, black_floor; };

double effective_gamma(double darken) {
    if (darken < 0.0) darken = 0.0;
    if (darken > 1.0) darken = 1.0;
    return 2.2 + 1.6 * darken;
}

double default_darken(ScreenKind s) {
    switch (s) {
        case ScreenKind::Unlit:    return 0.7;
        case ScreenKind::Frontlit: return 0.15;
        default:                   return 0.0;
    }
}

// Returns false for Raw/Classic (handled separately).
bool panel_model(ScreenKind s, double darken, PanelModel& out) {
    switch (s) {
        case ScreenKind::Unlit:
            out = {kPanelReflective, effective_gamma(darken), 0.91, 0.055};
            return true;
        case ScreenKind::Frontlit:
            out = {kPanelReflective, effective_gamma(darken), 0.91, 0.030};
            return true;
        case ScreenKind::Backlit:
            out = {kPanelBacklit, 2.2, 0.935, 0.002};
            return true;
        default:
            return false;
    }
}

uint8_t quantize(double v) {
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    return static_cast<uint8_t>(v * 255.0 + 0.5);
}

// The community-canonical gamma-4.0 model, reproduced exactly as published.
std::array<uint8_t, 3> classic_entry(uint16_t px) {
    double r = (px & 31) / 31.0;
    double g = ((px >> 5) & 31) / 31.0;
    double b = ((px >> 10) & 31) / 31.0;
    double lr = std::pow(r, 4.0), lg = std::pow(g, 4.0), lb = std::pow(b, 4.0);
    auto mix = [&](double a, double bq, double c) {
        double v = (a * lb + bq * lg + c * lr) / 255.0;
        double e = std::pow(v, 1.0 / 2.2) * (255.0 / 280.0);
        return e;
    };
    return {quantize(mix(0.0, 50.0, 255.0)), quantize(mix(30.0, 230.0, 10.0)),
            quantize(mix(220.0, 10.0, 50.0))};
}

const Primaries& target_primaries(DisplayTarget t) {
    return t == DisplayTarget::DisplayP3 ? kDisplayP3 : kSrgb;
}

}  // namespace

bool screen_kind_from_name(std::string_view name, ScreenKind& out) {
    if (name == "raw")      { out = ScreenKind::Raw;      return true; }
    if (name == "unlit")    { out = ScreenKind::Unlit;    return true; }
    if (name == "frontlit") { out = ScreenKind::Frontlit; return true; }
    if (name == "backlit")  { out = ScreenKind::Backlit;  return true; }
    if (name == "classic")  { out = ScreenKind::Classic;  return true; }
    return false;
}

ColorLut::ColorLut(const ColorSettings& settings) {
    table_ = std::make_unique<std::array<std::array<uint8_t, 3>, 32768>>();
    auto& table = *table_;

    if (settings.screen == ScreenKind::Raw) {
        passthrough_ = true;
        for (int px = 0; px < 32768; ++px) {
            uint8_t r = px & 31, g = (px >> 5) & 31, b = (px >> 10) & 31;
            table[px] = {static_cast<uint8_t>(r << 3 | r >> 2),
                         static_cast<uint8_t>(g << 3 | g >> 2),
                         static_cast<uint8_t>(b << 3 | b >> 2)};
        }
        return;
    }
    if (settings.screen == ScreenKind::Classic) {
        for (int px = 0; px < 32768; ++px)
            table[px] = classic_entry(static_cast<uint16_t>(px));
        return;
    }

    double darken = settings.darken < 0.0 ? default_darken(settings.screen)
                                          : settings.darken;
    PanelModel model{};
    panel_model(settings.screen, darken, model);
    Mat3 to_display = rgb_to_rgb(model.primaries, target_primaries(settings.target));

    for (int px = 0; px < 32768; ++px) {
        double c[3] = {(px & 31) / 31.0, ((px >> 5) & 31) / 31.0,
                       ((px >> 10) & 31) / 31.0};
        double lin[3];
        for (int i = 0; i < 3; ++i) {
            double v = std::pow(c[i], model.gamma) * model.luminance;
            lin[i] = v > 1.0 ? 1.0 : v;
        }
        double out[3];
        mat_apply(to_display, lin, out);
        for (int i = 0; i < 3; ++i) {
            double v = out[i] < 0.0 ? 0.0 : (out[i] > 1.0 ? 1.0 : out[i]);
            double lifted = model.black_floor + (1.0 - model.black_floor) * v;
            out[i] = srgb_oetf(lifted);
        }
        table[px] = {quantize(out[0]), quantize(out[1]), quantize(out[2])};
    }
}

void ColorLut::map_rgb888(const uint8_t* src, uint8_t* dst,
                          int width, int height) const {
    const auto& table = *table_;
    const int n = width * height;
    for (int i = 0; i < n; ++i) {
        const uint8_t* s = src + i * 3;
        // Recover the 5-bit hardware channels (top 5 bits) and re-pack as
        // BGR555 (red is the low 5 bits, matching the LUT index).
        uint32_t idx = (s[0] >> 3) | ((s[1] >> 3) << 5) | ((s[2] >> 3) << 10);
        const auto& e = table[idx];
        uint8_t* d = dst + i * 3;
        d[0] = e[0];
        d[1] = e[1];
        d[2] = e[2];
    }
}

}  // namespace gbarecomp::runtime
