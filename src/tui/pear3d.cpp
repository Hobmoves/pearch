#include "tui/pear3d.h"

#include <algorithm>
#include <cmath>

#include "tui/glyphs.h"
#include "tui/theme.h"

namespace tui {

namespace {

struct V {
    float x, y, z;
};
inline V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V operator*(V a, float k) { return {a.x * k, a.y * k, a.z * k}; }
inline float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float len(V a) { return std::sqrt(dot(a, a)); }
inline V norm(V a) {
    float l = len(a);
    return l > 0 ? a * (1.0f / l) : a;
}
inline float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
inline float smoothstep(float a, float b, float x) {
    float t = clamp01((x - a) / (b - a));
    return t * t * (3 - 2 * t);
}

struct M3 {
    float m[3][3];
    V mul(V v) const {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
    V tmul(V v) const {  // transpose: inverse of a rotation
        return {m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z, m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z,
                m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z};
    }
};

M3 mul(const M3& a, const M3& b) {
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

M3 rot_x(float a) { float c = std::cos(a), s = std::sin(a); return {{{1, 0, 0}, {0, c, -s}, {0, s, c}}}; }
M3 rot_y(float a) { float c = std::cos(a), s = std::sin(a); return {{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}}; }
M3 rot_z(float a) { float c = std::cos(a), s = std::sin(a); return {{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}}; }

float smin(float a, float b, float k) {
    float h = clamp01(0.5f + 0.5f * (b - a) / k);
    return b + (a - b) * h - k * h * (1 - h);
}

float sd_capsule(V p, V a, V b, float r) {
    V pa = p - a, ba = b - a;
    float h = clamp01(dot(pa, ba) / dot(ba, ba));
    return len(pa - ba * h) - r;
}

float sd_ellipsoid(V p, V r) {
    float k0 = len({p.x / r.x, p.y / r.y, p.z / r.z});
    float k1 = len({p.x / (r.x * r.x), p.y / (r.y * r.y), p.z / (r.z * r.z)});
    return k1 > 0 ? k0 * (k0 - 1.0f) / k1 : -1.0f;
}

enum Mat { kBody, kStemMat, kLeafMat };

const M3 kLeafSpace = mul(rot_y(0.45f), rot_z(-0.55f));

float map(V p, Mat* mat) {
    float body = smin(len(p - V{0.0f, -0.36f, 0.0f}) - 0.80f, len(p - V{0.04f, 0.43f, 0.0f}) - 0.46f, 0.45f);
    float stem = sd_capsule(p, {0.03f, 0.80f, 0.0f}, {0.13f, 1.14f, 0.02f}, 0.048f);
    float leaf = sd_ellipsoid(kLeafSpace.mul(p - V{0.30f, 1.03f, 0.02f}), {0.27f, 0.065f, 0.13f});
    float d = body;
    Mat m = kBody;
    if (stem < d) { d = stem; m = kStemMat; }
    if (leaf < d) { d = leaf; m = kLeafMat; }
    if (mat) *mat = m;
    return d;
}

V normal_at(V p) {
    const float h = 0.0025f;
    const V k1{1, -1, -1}, k2{-1, -1, 1}, k3{-1, 1, -1}, k4{1, 1, 1};
    return norm(k1 * map(p + k1 * h, nullptr) + k2 * map(p + k2 * h, nullptr) +
                k3 * map(p + k3 * h, nullptr) + k4 * map(p + k4 * h, nullptr));
}

float hash3(V q) {
    float v = std::sin(q.x * 12.9898f + q.y * 78.233f + q.z * 37.719f) * 43758.5453f;
    return v - std::floor(v);
}

struct Sample {
    float lum = 0;
    uint32_t color = 0;
    bool hit = false;
    bool surface = false;  // the liquid line
    bool glass = false;    // the still-empty part
};

constexpr const char* kRamp = " .,-~:;=!*#$@";
constexpr int kRampMax = 12;

struct Scene {
    M3 rot;
    float fill_y;
    float t;
    float progress;
    bool color;
    V light = norm({-0.50f, 0.62f, -0.80f});
    V fill_light = norm({0.75f, 0.15f, -0.25f});
};

Sample shade(const Scene& sc, V o_world, V d_world) {
    Sample s;
    const V o = sc.rot.tmul(o_world);
    const V d = sc.rot.tmul(d_world);
    // Bounding sphere.
    const float R = 1.45f;
    float b = dot(o, d), c = dot(o, o) - R * R, disc = b * b - c;
    bool hit = false;
    V p{};
    Mat mat = kBody;
    if (disc > 0) {
        float sq = std::sqrt(disc);
        float t = std::max(0.0f, -b - sq), t1 = -b + sq;
        for (int i = 0; i < 64 && t < t1; ++i) {
            p = o + d * t;
            float dist = map(p, &mat);
            if (dist < 0.0018f) { hit = true; break; }
            t += std::max(dist * 0.92f, 0.0015f);
        }
    }
    if (!hit) {
        // Soft contact shadow on an imaginary floor.
        if (d_world.y < 0) {
            float tg = (-1.24f - o_world.y) / d_world.y;
            V g = o_world + d_world * tg;
            float r = std::sqrt(g.x * g.x + g.z * g.z * 1.4f) / 1.0f;
            if (r < 1.0f) {
                float k = (1 - r) * (1 - r);
                s.lum = 0.20f * k;
                s.color = mix(0x1C2230, mix(0x2B3326, theme::kLime, 0.35f), k * (0.4f + 0.6f * sc.progress));
            }
        }
        return s;
    }

    s.hit = true;
    const V n_obj = normal_at(p);
    const V n = sc.rot.mul(n_obj);
    const V pw = sc.rot.mul(p);
    const V view = d_world * -1.0f;

    float occ = 0, sca = 1;
    for (int i = 0; i < 3; ++i) {
        float h = 0.03f + 0.07f * static_cast<float>(i);
        occ += (h - map(p + n_obj * h, nullptr)) * sca;
        sca *= 0.75f;
    }
    const float ao = std::clamp(1.0f - 2.4f * occ, 0.3f, 1.0f);
    const float diff = std::max(0.0f, dot(n, sc.light));
    const float fillv = std::max(0.0f, dot(n, sc.fill_light));
    const V h = norm(sc.light + view);
    const float spec = std::pow(std::max(0.0f, dot(n, h)), 28.0f);
    const float rim = std::pow(1.0f - std::max(0.0f, dot(n, view)), 3.0f);
    float lum = clamp01(0.13f + 0.74f * diff * ao + 0.24f * fillv + 0.33f * spec + 0.26f * rim * ao);

    if (mat == kLeafMat) {
        s.lum = clamp01(lum * 1.05f);
        s.color = mix(scale(theme::kLeaf, 0.38f + 0.8f * lum), 0xF4FFE0, spec * 0.5f);
        return s;
    }
    if (mat == kStemMat) {
        s.lum = clamp01(lum * 0.9f);
        s.color = scale(theme::kStem, 0.35f + 0.85f * lum);
        return s;
    }

    const float wave = 0.035f * std::sin(5.0f * pw.x + sc.t * 2.6f) + 0.025f * std::sin(7.0f * pw.z - sc.t * 1.9f);
    const float level = sc.fill_y + wave;
    const float band = 0.038f;
    if (pw.y < level - band) {
        // Filled: ripe pear skin.
        uint32_t base = mix(0x9BDC3C, 0xFFD45E, smoothstep(-1.10f, 0.80f, p.y));
        float blush = smoothstep(0.35f, 0.95f, dot(n_obj, norm({0.8f, 0.1f, -0.6f}))) * 0.32f;
        base = mix(base, 0xE5764F, blush);
        V q{std::floor(p.x * 13), std::floor(p.y * 13), std::floor(p.z * 13)};
        if (hash3(q) > 0.93f) base = scale(base, 0.78f);  // speckles show the spin
        s.color = mix(scale(base, 0.46f + 0.70f * lum), 0xFFFBEA, spec * 0.60f);
        s.lum = sc.color ? lum : clamp01(0.40f + 0.60f * lum);
    } else if (pw.y < level + band) {
        s.surface = true;
        s.color = mix(0xFFE9A0, 0xFFFFFF, 0.25f + 0.5f * spec);
        s.lum = std::max(lum, 0.72f);
    } else {
        // Still empty: dim glass.
        s.color = mix(mix(0x252C3E, 0x56637C, lum), 0xB4C8F0, rim * 0.75f);
        s.lum = sc.color ? lum * 0.45f : lum * 0.42f;
        s.glass = true;
    }
    return s;
}

}  // namespace

void draw_pear(Canvas& c, int x0, int y0, int w, int h, const PearParams& prm) {
    if (w < 6 || h < 4) return;
    Scene sc;
    sc.t = static_cast<float>(prm.time);
    sc.progress = static_cast<float>(std::clamp(prm.progress, 0.0, 1.0));
    sc.color = prm.color;
    const float spin = sc.t * 0.85f;
    sc.rot = mul(mul(rot_z(0.07f * std::sin(sc.t * 0.8f)), rot_x(0.16f * std::sin(sc.t * 0.6f) - 0.06f)), rot_y(spin));
    sc.fill_y = sc.progress >= 1.0f ? 9.0f : -1.20f + sc.progress * 2.18f;

    // Square pixels: each cell is two samples tall; 2x2 supersampled.
    const float W = static_cast<float>(w), H = static_cast<float>(h * 2);
    const float unit = std::min(H * 0.5f, W * 0.5f / 0.74f);
    const float k = 1.30f / 4.4f;
    const V origin{0, 0, -4.4f};
    const float flash = static_cast<float>(prm.flash);

    struct Half {
        int hits = 0;
        float lum = 0;
        uint32_t color = 0;
        bool surface = false;
        bool glass = false;
    };
    auto sample_half = [&](int cx, int py) {
        Half hf;
        float r = 0, g = 0, b = 0;
        for (int sub = 0; sub < 2; ++sub) {
            float px = static_cast<float>(cx) + 0.25f + 0.5f * static_cast<float>(sub);
            float u = (px - W * 0.5f) / unit;
            float v = (H * 0.5f - (static_cast<float>(py) + 0.5f)) / unit;
            Sample s = shade(sc, origin, norm({u * k, v * k, 1.0f}));
            hf.lum += s.lum * 0.5f;
            if (s.hit) {
                ++hf.hits;
                hf.surface |= s.surface;
                hf.glass |= s.glass;
            }
            if (s.hit || s.lum > 0) {
                r += static_cast<float>((s.color >> 16) & 0xFF);
                g += static_cast<float>((s.color >> 8) & 0xFF);
                b += static_cast<float>(s.color & 0xFF);
            }
        }
        int n = std::max(1, hf.hits > 0 ? hf.hits : 2);
        auto to8 = [&](float v) { return static_cast<uint32_t>(std::clamp(v / static_cast<float>(n), 0.0f, 255.0f)); };
        hf.color = (to8(r) << 16) | (to8(g) << 8) | to8(b);
        return hf;
    };
    auto lift = [&](uint32_t col) { return flash > 0 ? mix(col, 0xFFF6D5, flash * 0.22f) : col; };

    for (int cy = 0; cy < h; ++cy) {
        for (int cx = 0; cx < w; ++cx) {
            Half top = sample_half(cx, cy * 2);
            Half bot = sample_half(cx, cy * 2 + 1);
            const bool th = top.hits > 0, bh = bot.hits > 0;
            const int X = x0 + cx, Y = y0 + cy;
            if (th && bh) {
                // Solid body: a darker shade fills the cell, a bold glyph on top
                // carries the light. Chunky, but still drawn in text.
                float lum = clamp01((top.lum + bot.lum) * 0.5f + flash * 0.08f);
                uint32_t col = lift(mix(top.color, bot.color, 0.5f));
                // A gentle curve keeps mid-tones varied instead of all '@'.
                int idx = std::clamp(static_cast<int>(std::pow(lum, 1.35f) * kRampMax + 0.5f), 1, kRampMax);
                char32_t ch = static_cast<char32_t>(kRamp[idx]);
                if (top.surface || bot.surface) ch = U'≈';
                if (prm.color) {
                    const bool glass = top.glass && bot.glass;
                    uint32_t glyph = glass ? mix(col, 0xC8D6F5, 0.18f) : mix(col, 0xFFFFFF, 0.16f + 0.30f * lum);
                    uint32_t fill = glass ? scale(col, 0.78f) : scale(col, 0.58f + 0.14f * lum);
                    if (top.surface || bot.surface) { glyph = 0xFFF8DC; fill = scale(col, 0.85f); }
                    c.put(X, Y, ch, Style{glyph, fill, kBold});
                } else {
                    static const char32_t solid[] = {U'░', U'░', U'▒', U'▓', U'█'};
                    c.put(X, Y, top.surface || bot.surface ? U'≈' : solid[std::clamp(static_cast<int>(lum * 4.0f + 0.5f), 0, 4)],
                          Style{});
                }
            } else if (th || bh) {
                // Silhouette edge: half blocks give a crisp outline.
                const Half& hf = th ? top : bot;
                uint32_t col = lift(scale(hf.color, 0.75f + 0.25f * static_cast<float>(hf.hits) * 0.5f));
                const Half& other = th ? bot : top;
                uint32_t under = other.lum > 0.02f ? other.color : kDefault;
                c.put(X, Y, th ? U'▀' : U'▄', prm.color ? Style{col, under, 0} : Style{});
            } else {
                float lum = (top.lum + bot.lum) * 0.5f;
                if (lum < 0.03f) continue;
                int idx = std::clamp(static_cast<int>(lum * kRampMax * 1.6f + 0.5f), 1, 4);
                c.put(X, Y, static_cast<char32_t>(kRamp[idx]),
                      prm.color ? fg(mix(top.color, bot.color, 0.5f)) : Style{kDefault, kDefault, kDim});
            }
        }
    }

    // Completion sparkles orbiting the pear.
    if (flash > 0.01f) {
        const char32_t twinkle[] = {U'·', U'+', glyphs().sparkle, U'+', U'·'};
        for (int i = 0; i < 12; ++i) {
            float a = static_cast<float>(i) * 0.5236f + sc.t * 0.35f;
            float r = 1.30f + 0.12f * std::sin(static_cast<float>(i) * 1.7f + sc.t * 2.1f);
            float wx = std::cos(a) * r * 0.95f, wy = std::sin(a) * r * 1.02f;
            float sx = wx / (4.4f * k) * unit + W * 0.5f;
            float sy = (H * 0.5f - wy / (4.4f * k) * unit) * 0.5f;
            int ix = static_cast<int>(sx), iy = static_cast<int>(sy);
            if (ix < 0 || iy < 0 || ix >= w || iy >= h) continue;
            if (c.at(x0 + ix, y0 + iy).style.bg != kDefault) continue;  // don't draw over the pear
            float ph = sc.t * 1.6f + static_cast<float>(i) * 0.37f;
            int tw = static_cast<int>((ph - std::floor(ph)) * 5.0f) % 5;
            c.put(x0 + ix, y0 + iy, twinkle[tw], prm.color ? fg(mix(0x3A3220, theme::kGold, flash), kBold) : Style{});
        }
    }
}

}  // namespace tui
