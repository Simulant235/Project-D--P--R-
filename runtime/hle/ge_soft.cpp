// Software-Renderer der GE (T21), wird pro Befehl aus devices_placeholder.cpp aufgerufen, zeichnet ins VRAM.
// Fehlt noch: Beleuchtung (nur grob), Nebel, Stencil, Logikops, Near-Clipping (Dreiecke hinter der Kamera fliegen raus), Mipmaps, bilinear.
// Befehle und Formate nach PPSSPP (GPU/ge_constants.h, GPU/Software/*).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "../src/rt.h"
#include "ge_soft.h"

using namespace rt;

namespace {

uint32_t R[256];                                   // letzte Argumente aller Befehle
float world[12], view[12], proj[16], bones[8 * 12], tgen[12];
int world_i, view_i, proj_i, bone_i, tgen_i;
uint32_t vaddr, iaddr;
uint8_t clut[1024];

float f24(uint32_t a) { uint32_t b = a << 8; float f; memcpy(&f, &b, 4); return f; }

struct V { float x, y, z, w, u, v; float r, g, b, a; };   // Screen-Koordinaten (Pixel), 1/w für Perspektive

int comp_size(int type, int kind) {   // kind: 0 = u8/s8
    (void)kind;
    return type == 1 ? 1 : type == 2 ? 2 : type == 3 ? 4 : 0;
}

struct Fmt { int size, w_off, tc_off, c_off, n_off, p_off, wn, wt, tct, ct, nt, pt, it; bool through; };

Fmt decode_fmt(uint32_t vt) {
    Fmt f = {};
    f.tct = vt & 3; f.ct = (vt >> 2) & 7; f.nt = (vt >> 5) & 3; f.pt = (vt >> 7) & 3; f.wt = (vt >> 9) & 3;
    f.it = (vt >> 11) & 3; f.wn = (int)((vt >> 14) & 7) + 1; f.through = (vt >> 23) & 1;
    int off = 0, maxal = 1;
    auto place = [&](int sz, int count) { if (!sz) return -1; off = (off + sz - 1) & ~(sz - 1); int o = off; off += sz * count; maxal = std::max(maxal, sz); return o; };
    f.w_off = f.wt ? place(comp_size(f.wt, 0), f.wn) : -1;
    f.tc_off = f.tct ? place(comp_size(f.tct, 0), 2) : -1;
    f.c_off = f.ct >= 4 ? place(f.ct == 7 ? 4 : 2, 1) : -1;
    f.n_off = f.nt ? place(comp_size(f.nt, 0), 3) : -1;
    f.p_off = place(comp_size(f.pt ? f.pt : 3, 0), 3);
    f.size = (off + maxal - 1) & ~(maxal - 1);
    return f;
}

float rd(const uint8_t* p, int type, int i, bool norm, bool sign) {
    switch (type) {
    case 1: return sign ? (float)(int8_t)p[i] / (norm ? 128.f : 1.f) : (float)p[i] / (norm ? 128.f : 1.f);
    case 2: { int16_t s; uint16_t u; memcpy(&s, p + i * 2, 2); memcpy(&u, p + i * 2, 2);
              return sign ? (float)s / (norm ? 32768.f : 1.f) : (float)u / (norm ? 32768.f : 1.f); }
    case 3: { float f; memcpy(&f, p + i * 4, 4); return f; }
    }
    return 0;
}

void unpack16(uint32_t c, int fmt, float* o) {
    switch (fmt) {
    case 0: o[0] = (c & 31) * 255.f / 31; o[1] = ((c >> 5) & 63) * 255.f / 63; o[2] = ((c >> 11) & 31) * 255.f / 31; o[3] = 255; break;
    case 1: o[0] = (c & 31) * 255.f / 31; o[1] = ((c >> 5) & 31) * 255.f / 31; o[2] = ((c >> 10) & 31) * 255.f / 31; o[3] = (c >> 15) ? 255.f : 0.f; break;
    case 2: o[0] = (c & 15) * 17.f; o[1] = ((c >> 4) & 15) * 17.f; o[2] = ((c >> 8) & 15) * 17.f; o[3] = ((c >> 12) & 15) * 17.f; break;
    default: o[0] = (float)(c & 255); o[1] = (float)((c >> 8) & 255); o[2] = (float)((c >> 16) & 255); o[3] = (float)(c >> 24); break;
    }
}

uint32_t col_word(float r, float g, float b, float a) {
    auto q = [](float v) { return (uint32_t)std::clamp(v, 0.f, 255.f); };
    return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
}

void mat43(const float* m, const float* i, float* o) {
    for (int k = 0; k < 3; k++) o[k] = i[0] * m[k] + i[1] * m[3 + k] + i[2] * m[6 + k] + m[9 + k];
}

// Vertex lesen und auf den Schirm bringen; false = hinter der Kamera
bool load_vertex(uint8_t* mem, const Fmt& f, uint32_t addr, V& out) {
    if (!mem_valid(addr, (uint32_t)f.size)) return false;
    const uint8_t* p = mem + addr;
    float pos[3];
    for (int i = 0; i < 3; i++) pos[i] = rd(p + f.p_off, f.pt ? f.pt : 3, i, !f.through, true);
    float c[4];
    if (f.c_off >= 0) {
        if (f.ct == 7) { uint32_t w; memcpy(&w, p + f.c_off, 4); unpack16(w, 3, c); }
        else { uint16_t w; memcpy(&w, p + f.c_off, 2); unpack16(w, f.ct - 4, c); }
    } else {
        unpack16(R[0x55], 3, c);
        c[3] = (float)(R[0x58] & 255);
        if (R[0x17] & 1) {   // Licht grob: Emission + Diffus
            float d[4], e[4];
            unpack16(R[0x56], 3, d);
            unpack16(R[0x54], 3, e);
            for (int k = 0; k < 3; k++) c[k] = std::min(255.f, e[k] + d[k] * 0.8f + c[k] * 0.2f);
        }
    }
    out.r = c[0]; out.g = c[1]; out.b = c[2]; out.a = c[3];
    float u = 0, v = 0;
    if (f.tc_off >= 0) {
        u = rd(p + f.tc_off, f.tct, 0, !f.through, false);
        v = rd(p + f.tc_off, f.tct, 1, !f.through, false);
    }
    if (f.through) {
        out.x = pos[0]; out.y = pos[1]; out.z = pos[2]; out.w = 1; out.u = u; out.v = v;
        return true;
    }
    float sk[3] = {pos[0], pos[1], pos[2]};
    if (f.w_off >= 0) {
        sk[0] = sk[1] = sk[2] = 0;
        for (int i = 0; i < f.wn; i++) {
            const float wgt = rd(p + f.w_off, f.wt, i, true, false);
            if (wgt == 0) continue;
            float t[3];
            mat43(bones + i * 12, pos, t);
            for (int k = 0; k < 3; k++) sk[k] += t[k] * wgt;
        }
    }
    float wp[3], vp[3];
    mat43(world, sk, wp);
    mat43(view, wp, vp);
    float cl[4];
    for (int k = 0; k < 4; k++) cl[k] = vp[0] * proj[k] + vp[1] * proj[4 + k] + vp[2] * proj[8 + k] + proj[12 + k];
    if (cl[3] <= 1e-5f) return false;
    const float iw = 1.f / cl[3];
    out.x = f24(R[0x42]) * cl[0] * iw + f24(R[0x45]) - (float)(R[0x4C] & 0xFFFF) / 16.f;
    out.y = f24(R[0x43]) * cl[1] * iw + f24(R[0x46]) - (float)(R[0x4D] & 0xFFFF) / 16.f;
    out.z = f24(R[0x44]) * cl[2] * iw + f24(R[0x47]);
    out.w = iw;
    out.u = u * f24(R[0x48]) + f24(R[0x4A]);
    out.v = v * f24(R[0x49]) + f24(R[0x4B]);
    return true;
}

// ---- Pixel
struct Tex { uint32_t addr, bufw; int w, h, fmt; bool swz, clampu, clampv; };

uint32_t tex_byte_addr(const Tex& t, int x, int y, int bits) {
    if (!t.swz) return t.addr + (uint32_t)((y * (int)t.bufw + x) * bits / 8);
    const int bx = x * bits / 8, rowb = (int)t.bufw * bits / 8;
    return t.addr + (uint32_t)(((y / 8) * (rowb / 16) + bx / 16) * 128 + (y % 8) * 16 + (bx % 16));
}

void clut_lookup(uint32_t idx, float* o) {
    const uint32_t cf = R[0xC5];
    idx = ((idx >> ((cf >> 2) & 31)) & ((cf >> 8) & 255)) | (((cf >> 16) & 31) << 4);
    if ((cf & 3) == 3) { uint32_t c; memcpy(&c, clut + (idx & 255) * 4, 4); unpack16(c, 3, o); }
    else { uint16_t c; memcpy(&c, clut + (idx & 511) * 2, 2); unpack16(c, cf & 3, o); }
}

void sample(uint8_t* mem, const Tex& t, float fu, float fv, float* o) {
    int x = (int)std::floor(fu), y = (int)std::floor(fv);
    x = t.clampu ? std::clamp(x, 0, t.w - 1) : (x & (t.w - 1));
    y = t.clampv ? std::clamp(y, 0, t.h - 1) : (y & (t.h - 1));
    switch (t.fmt) {
    case 3: { const uint32_t a = tex_byte_addr(t, x, y, 32); uint32_t c = 0; memcpy(&c, mem + a, 4); unpack16(c, 3, o); break; }
    case 0: case 1: case 2: { const uint32_t a = tex_byte_addr(t, x, y, 16); uint16_t c = 0; memcpy(&c, mem + a, 2); unpack16(c, t.fmt, o); break; }
    case 4: { const uint32_t a = tex_byte_addr(t, x, y, 4); const uint8_t b = mem[a]; clut_lookup((x & 1) ? b >> 4 : b & 15, o); break; }
    case 5: { const uint32_t a = tex_byte_addr(t, x, y, 8); clut_lookup(mem[a], o); break; }
    case 6: { const uint32_t a = tex_byte_addr(t, x, y, 16); uint16_t c = 0; memcpy(&c, mem + a, 2); clut_lookup(c, o); break; }
    default: { const uint32_t a = tex_byte_addr(t, x, y, 32); uint32_t c = 0; memcpy(&c, mem + a, 4); clut_lookup(c, o); break; }
    }
}

bool cmp(int fn, float a, float b) {
    switch (fn) { case 0: return false; case 1: return true; case 2: return a == b; case 3: return a != b;
                  case 4: return a < b; case 5: return a <= b; case 6: return a > b; default: return a >= b; }
}

struct Ctx {
    uint8_t* mem;
    uint32_t fb, fbw, zb, zbw; int fbfmt;
    bool clear, tex_on, blend, atest, ztest, zwrite;
    int sx1, sy1, sx2, sy2;
    Tex tex;
};

void blend_factor(int f, bool src, const float* s, const float* d, float* o) {
    float k[3];
    const uint32_t fix = src ? R[0xE0] : R[0xE1];
    switch (f) {
    case 0: for (int i = 0; i < 3; i++) k[i] = (src ? d[i] : s[i]) / 255.f; break;
    case 1: for (int i = 0; i < 3; i++) k[i] = 1 - (src ? d[i] : s[i]) / 255.f; break;
    case 2: k[0] = k[1] = k[2] = s[3] / 255.f; break;
    case 3: k[0] = k[1] = k[2] = 1 - s[3] / 255.f; break;
    case 4: k[0] = k[1] = k[2] = d[3] / 255.f; break;
    case 5: k[0] = k[1] = k[2] = 1 - d[3] / 255.f; break;
    case 6: k[0] = k[1] = k[2] = 2 * s[3] / 255.f; break;
    case 7: k[0] = k[1] = k[2] = 2 * (1 - s[3] / 255.f); break;
    case 8: k[0] = k[1] = k[2] = 2 * d[3] / 255.f; break;
    case 9: k[0] = k[1] = k[2] = 2 * (1 - d[3] / 255.f); break;
    default: k[0] = (fix & 255) / 255.f; k[1] = ((fix >> 8) & 255) / 255.f; k[2] = ((fix >> 16) & 255) / 255.f; break;
    }
    for (int i = 0; i < 3; i++) o[i] = k[i];
}

void plot(Ctx& c, int x, int y, float z, float* col, float u, float v) {
    uint8_t* mem = c.mem;
    const int bpp = c.fbfmt == 3 ? 4 : 2;
    const uint32_t pa = c.fb + (uint32_t)(y * (int)c.fbw + x) * (uint32_t)bpp;
    const uint32_t za = c.zb + (uint32_t)(y * (int)c.zbw + x) * 2;
    const uint16_t zi = (uint16_t)std::clamp(z, 0.f, 65535.f);
    if (c.clear) {
        const uint32_t m = R[0xD3];
        if (m & 0x400) psp_w16(mem, za, zi);
        if (bpp == 4) {
            uint32_t old = psp_r32(mem, pa), nw = col_word(col[0], col[1], col[2], col[3]);
            if (!(m & 0x100)) nw = (nw & 0xFF000000u) | (old & 0x00FFFFFFu);
            if (!(m & 0x200)) nw = (nw & 0x00FFFFFFu) | (old & 0xFF000000u);
            psp_w32(mem, pa, nw);
        }
        return;
    }
    if (c.ztest) {
        const uint16_t old = psp_r16(mem, za);
        if (!cmp(R[0xDE] & 7, (float)zi, (float)old)) return;
    }
    float s[4] = {col[0], col[1], col[2], col[3]};
    if (c.tex_on) {
        float t[4];
        sample(mem, c.tex, u, v, t);
        const uint32_t tf = R[0xC9];
        const bool ta = (tf >> 8) & 1;
        switch (tf & 7) {
        case 0: for (int i = 0; i < 3; i++) s[i] = s[i] * t[i] / 255.f; if (ta) s[3] = s[3] * t[3] / 255.f; break;
        case 1: for (int i = 0; i < 3; i++) s[i] = ta ? s[i] + (t[i] - s[i]) * t[3] / 255.f : t[i]; break;
        case 2: { float e[4]; unpack16(R[0xCA], 3, e); for (int i = 0; i < 3; i++) s[i] = s[i] * (1 - t[i] / 255.f) + e[i] * t[i] / 255.f; if (ta) s[3] = s[3] * t[3] / 255.f; break; }
        case 3: for (int i = 0; i < 3; i++) s[i] = t[i]; if (ta) s[3] = t[3]; break;
        default: for (int i = 0; i < 3; i++) s[i] = std::min(255.f, s[i] + t[i]); if (ta) s[3] = s[3] * t[3] / 255.f; break;
        }
        if ((tf >> 16) & 1) for (int i = 0; i < 3; i++) s[i] = std::min(255.f, s[i] * 2);
    }
    if (c.atest) {
        const uint32_t at = R[0xDB];
        const uint32_t mask = (at >> 16) & 255;
        if (!cmp(at & 7, (float)((uint32_t)s[3] & mask), (float)(((at >> 8) & 255) & mask))) return;
    }
    if (bpp != 4) return;   // andere FB-Formate fehlen
    float d[4];
    unpack16(psp_r32(mem, pa), 3, d);
    if (c.blend) {
        const uint32_t bm = R[0xDF];
        float a[3], b[3];
        blend_factor(bm & 15, true, s, d, a);
        blend_factor((bm >> 4) & 15, false, s, d, b);
        for (int i = 0; i < 3; i++) {
            const float x1 = s[i] * a[i], x2 = d[i] * b[i];
            switch ((bm >> 8) & 7) {
            case 1: s[i] = x1 - x2; break;
            case 2: s[i] = x2 - x1; break;
            case 3: s[i] = std::min(s[i], d[i]); break;
            case 4: s[i] = std::max(s[i], d[i]); break;
            case 5: s[i] = std::fabs(s[i] - d[i]); break;
            default: s[i] = x1 + x2; break;
            }
        }
    }
    psp_w32(mem, pa, col_word(s[0], s[1], s[2], d[3]));
    if (c.ztest && c.zwrite) psp_w16(mem, za, zi);
}

void tri(Ctx& c, const V& a, const V& b, const V& d) {
    const float area = (b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x);
    if (std::fabs(area) < 1e-6f) return;
    int x0 = (int)std::floor(std::min({a.x, b.x, d.x})), x1 = (int)std::ceil(std::max({a.x, b.x, d.x}));
    int y0 = (int)std::floor(std::min({a.y, b.y, d.y})), y1 = (int)std::ceil(std::max({a.y, b.y, d.y}));
    x0 = std::max(x0, c.sx1); y0 = std::max(y0, c.sy1); x1 = std::min(x1, c.sx2); y1 = std::min(y1, c.sy2);
    const bool persp = !(a.w == 1 && b.w == 1 && d.w == 1);
    const float ia = 1.f / area;
    // l0(x) = ((b.x - px)(d.y - py) - (b.y - py)(d.x - px)) / area: linear in px, Schritt je Pixel -(d.y - b.y)/area
    const float s0 = -(d.y - b.y) * ia, s1 = -(a.y - d.y) * ia;
    for (int y = y0; y <= y1; y++) {
        const float py = y + 0.5f, px0 = x0 + 0.5f;
        float l0 = ((b.x - px0) * (d.y - py) - (b.y - py) * (d.x - px0)) * ia;
        float l1 = ((d.x - px0) * (a.y - py) - (d.y - py) * (a.x - px0)) * ia;
        for (int x = x0; x <= x1; x++, l0 += s0, l1 += s1) {
            const float l2 = 1 - l0 - l1;
            if (l0 < 0 || l1 < 0 || l2 < 0) continue;
            const float z = l0 * a.z + l1 * b.z + l2 * d.z;
            float w0 = l0, w1 = l1, w2 = l2;
            if (persp) {
                w0 *= a.w; w1 *= b.w; w2 *= d.w;
                const float s = 1.f / (w0 + w1 + w2);
                w0 *= s; w1 *= s; w2 *= s;
            }
            float col[4];
            if ((R[0x50] & 1) == 0) { col[0] = d.r; col[1] = d.g; col[2] = d.b; col[3] = d.a; }   // flat: letzter Vertex
            else {
                col[0] = w0 * a.r + w1 * b.r + w2 * d.r; col[1] = w0 * a.g + w1 * b.g + w2 * d.g;
                col[2] = w0 * a.b + w1 * b.b + w2 * d.b; col[3] = w0 * a.a + w1 * b.a + w2 * d.a;
            }
            float u = w0 * a.u + w1 * b.u + w2 * d.u, v = w0 * a.v + w1 * b.v + w2 * d.v;
            plot(c, x, y, z, col, u, v);
        }
    }
}

void sprite(Ctx& c, const V& a, const V& b) {
    const int x0 = std::max((int)std::min(a.x, b.x), c.sx1), x1 = std::min((int)std::max(a.x, b.x) - 1, c.sx2);
    const int y0 = std::max((int)std::min(a.y, b.y), c.sy1), y1 = std::min((int)std::max(a.y, b.y) - 1, c.sy2);
    const float dx = b.x - a.x, dy = b.y - a.y;
    float col[4] = {b.r, b.g, b.b, b.a};
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const float fx = dx ? (x + 0.5f - a.x) / dx : 0, fy = dy ? (y + 0.5f - a.y) / dy : 0;
            float cc[4] = {col[0], col[1], col[2], col[3]};
            plot(c, x, y, b.z, cc, a.u + (b.u - a.u) * fx, a.v + (b.v - a.v) * fy);
        }
}

// ---- Thread-Pool, Bildstreifen
class Pool {
public:
    Pool() {
        n_ = std::clamp((int)std::thread::hardware_concurrency() - 2, 1, 8);
        for (int i = 1; i < n_; i++) workers_.emplace_back([this, i] { loop(i); });
    }
    int size() const { return n_; }
    void run(const std::function<void(int)>& job) {
        {
            std::lock_guard<std::mutex> lk(m_);
            job_ = &job;
            pending_ = n_ - 1;
            gen_++;
        }
        cv_.notify_all();
        job(0);
        std::unique_lock<std::mutex> lk(m_);
        done_.wait(lk, [this] { return pending_ == 0; });
    }
private:
    void loop(int idx) {
        rt::name_thread(L"Raster");
        uint64_t seen = 0;
        for (;;) {
            const std::function<void(int)>* job;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return gen_ != seen; });
                seen = gen_;
                job = job_;
            }
            (*job)(idx);
            std::lock_guard<std::mutex> lk(m_);
            if (--pending_ == 0) done_.notify_one();
        }
    }
    int n_;
    std::vector<std::thread> workers_;
    std::mutex m_;
    std::condition_variable cv_, done_;
    const std::function<void(int)>* job_ = nullptr;
    int pending_ = 0;
    uint64_t gen_ = 0;
};

template <class F>
void parallel_bands(int y0, int y1, F&& f) {
    static Pool pool;
    const int n = pool.size(), h = y1 - y0;
    const std::function<void(int)> job = [&](int i) {
        const int a = y0 + h * i / n, b = y0 + h * (i + 1) / n;
        if (b > a) f(a, b);
    };
    pool.run(job);
}

void draw(uint8_t* mem, uint32_t arg) {
    const int type = (arg >> 16) & 7, count = (int)(arg & 0xFFFF);
    const uint32_t vt = R[0x12];
    const Fmt f = decode_fmt(vt);
    Ctx c = {};
    c.mem = mem;
    c.fb = 0x04000000u | (R[0x9C] & 0x1FFFF0u);
    c.fbw = R[0x9D] & 0x7FF; if (!c.fbw) c.fbw = 512;
    c.zb = 0x04000000u | (R[0x9E] & 0x1FFFF0u);
    c.zbw = R[0x9F] & 0x7FF; if (!c.zbw) c.zbw = 512;
    c.fbfmt = (int)(R[0xD2] & 3);
    c.clear = R[0xD3] & 1;
    c.tex_on = !c.clear && (R[0x1E] & 1);
    c.blend = R[0x21] & 1; c.atest = R[0x22] & 1; c.ztest = R[0x23] & 1; c.zwrite = !(R[0xE7] & 1);
    c.sx1 = (int)(R[0xD4] & 0x3FF); c.sy1 = (int)((R[0xD4] >> 10) & 0x3FF);
    c.sx2 = (int)(R[0xD5] & 0x3FF); c.sy2 = (int)((R[0xD5] >> 10) & 0x3FF);
    c.sx2 = std::min(c.sx2, (int)c.fbw - 1); c.sy2 = std::min(c.sy2, 271);
    if (c.tex_on) {
        c.tex.addr = (R[0xA0] & 0xFFFFF0u) | ((R[0xA8] << 8) & 0x0F000000u);
        c.tex.bufw = R[0xA8] & 0x7FF; if (!c.tex.bufw) c.tex.bufw = 1;
        c.tex.w = 1 << (R[0xB8] & 15); c.tex.h = 1 << ((R[0xB8] >> 8) & 15);
        c.tex.fmt = (int)(R[0xC3] & 7); c.tex.swz = R[0xC2] & 1;
        c.tex.clampu = R[0xC7] & 1; c.tex.clampv = (R[0xC7] >> 8) & 1;
        static const int bits[8] = {16, 16, 16, 32, 4, 8, 16, 32};
        const uint32_t rows = (uint32_t)((c.tex.h + 7) & ~7), rowb = std::max<uint32_t>(c.tex.bufw * (uint32_t)bits[c.tex.fmt] / 8, 16);
        if (!mem_valid(c.tex.addr, rows * rowb)) c.tex_on = false;   // Textur außerhalb Speicher: ohne Textur
    }
    const int isz = f.it == 1 ? 1 : f.it == 2 ? 2 : 0;
    std::vector<V> vs((size_t)count);
    std::vector<bool> ok((size_t)count);
    for (int i = 0; i < count; i++) {
        uint32_t idx = (uint32_t)i;
        if (isz == 1) idx = mem_valid(iaddr + i, 1) ? mem[iaddr + i] : 0;
        else if (isz == 2) idx = mem_valid(iaddr + i * 2, 2) ? psp_r16(mem, iaddr + i * 2) : 0;
        ok[(size_t)i] = load_vertex(mem, f, vaddr + idx * (uint32_t)f.size, vs[(size_t)i]);
        // Through + nicht perspektivisch: UVs in Texeln, sonst 0..1
        if (ok[(size_t)i] && !f.through && c.tex_on) { vs[(size_t)i].u *= (float)c.tex.w; vs[(size_t)i].v *= (float)c.tex.h; }
    }
    if (isz) iaddr += (uint32_t)(count * isz); else vaddr += (uint32_t)(count * f.size);
    if (!mem_valid(c.fb, 4) || !mem_valid(c.zb, 2)) return;
    // Streifen parallel, jeder Streifen genau ein Thread, Primitiv-Reihenfolge pro Pixel bleibt (Blending, Tiefe)
    float ylo = 1e9f, yhi = -1e9f;
    for (int i = 0; i < count; i++)
        if (ok[(size_t)i]) { ylo = std::min(ylo, vs[(size_t)i].y); yhi = std::max(yhi, vs[(size_t)i].y); }
    auto run = [&](const Ctx& cc) {
        Ctx cb = cc;
        auto T = [&](int i, int j, int k) { if (ok[(size_t)i] && ok[(size_t)j] && ok[(size_t)k]) tri(cb, vs[(size_t)i], vs[(size_t)j], vs[(size_t)k]); };
        switch (type) {
        case 3: for (int i = 0; i + 2 < count; i += 3) T(i, i + 1, i + 2); break;
        case 4: for (int i = 0; i + 2 < count; i++) T(i, i + 1, i + 2); break;
        case 5: for (int i = 1; i + 1 < count; i++) T(0, i, i + 1); break;
        case 6: for (int i = 0; i + 1 < count; i += 2) if (ok[(size_t)i] && ok[(size_t)i + 1]) sprite(cb, vs[(size_t)i], vs[(size_t)i + 1]); break;
        default: break;
        }
    };
    const int y0 = std::max(c.sy1, (int)ylo), y1 = std::min(c.sy2, (int)yhi + 1);
    if (y1 - y0 < 32 || count < 3) { run(c); return; }
    parallel_bands(y0, y1, [&](int a, int b) { Ctx cb = c; cb.sy1 = a; cb.sy2 = b - 1; run(cb); });
}

} // namespace

void ge_soft_cmd(uint8_t* mem, uint32_t w, uint32_t base) {
    const uint32_t cmd = w >> 24, arg = w & 0xFFFFFF;
    R[cmd] = arg;
    switch (cmd) {
    case 0x01: vaddr = (base | arg) & 0x0FFFFFFF; break;
    case 0x02: iaddr = (base | arg) & 0x0FFFFFFF; break;
    case 0x04: draw(mem, arg); break;
    case 0x2A: bone_i = (int)(arg & 0x7F); break;
    case 0x2B: if (bone_i < 96) bones[bone_i++] = f24(arg); break;
    case 0x3A: world_i = (int)(arg & 15); break;
    case 0x3B: if (world_i < 12) world[world_i++] = f24(arg); break;
    case 0x3C: view_i = (int)(arg & 15); break;
    case 0x3D: if (view_i < 12) view[view_i++] = f24(arg); break;
    case 0x3E: proj_i = (int)(arg & 15); break;
    case 0x3F: if (proj_i < 16) proj[proj_i++] = f24(arg); break;
    case 0x40: tgen_i = (int)(arg & 15); break;
    case 0x41: if (tgen_i < 12) tgen[tgen_i++] = f24(arg); break;
    case 0xC4: {   // LOADCLUT: arg * 32 Byte
        const uint32_t a = (R[0xB0] & 0xFFFFF0u) | ((R[0xB1] << 8) & 0x0F000000u);
        const uint32_t n = std::min<uint32_t>((arg & 0x3F) * 32, sizeof clut);
        if (mem_valid(a, n)) memcpy(clut, mem + a, n);
        break;
    }
    default: break;
    }
}
