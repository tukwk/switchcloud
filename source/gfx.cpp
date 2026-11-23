#include "gfx.h"
#include "font8x16.h"
#include <algorithm>
#include <cmath>

Gfx gfx;

u32 mixColor(u32 a, u32 b, float t) {
    int ar = a & 0xFF, ag = (a >> 8) & 0xFF, ab = (a >> 16) & 0xFF;
    int br = b & 0xFF, bg = (b >> 8) & 0xFF, bb = (b >> 16) & 0xFF;
    int r = (int)(ar + (br - ar) * t);
    int g = (int)(ag + (bg - ag) * t);
    int bl = (int)(ab + (bb - ab) * t);
    return RGBA8(r, g, bl, 255);
}

static const Glyph8x16* findGlyph(uint32_t cp) {
    int lo = 0, hi = (int)kFont8x16Count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (kFont8x16[mid].cp == cp) return &kFont8x16[mid];
        if (kFont8x16[mid].cp < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return nullptr;
}

// Stylised Unicode (mathematical bold/italic/sans, fullwidth) is common in
// SoundCloud titles. Map the common blocks to plain ASCII so the 8x16 font can
// render them instead of dropping the glyphs.
static uint32_t normalizeCodepoint(uint32_t cp) {
    if (cp >= 0xFF01 && cp <= 0xFF5E) return cp - 0xFEE0;
    static const uint32_t letterRanges[] = {
        0x1D400, 0x1D434, 0x1D468, 0x1D49C, 0x1D4D0, 0x1D504, 0x1D538,
        0x1D56C, 0x1D5A0, 0x1D5D4, 0x1D608, 0x1D63C, 0x1D670
    };
    for (uint32_t start : letterRanges) {
        if (cp >= start && cp < start + 52) {
            uint32_t off = cp - start;
            return off < 26 ? ('A' + off) : ('a' + off - 26);
        }
    }
    static const uint32_t digitStarts[] = {
        0x1D7CE, 0x1D7D8, 0x1D7E2, 0x1D7EC, 0x1D7F6
    };
    for (uint32_t start : digitStarts) {
        if (cp >= start && cp < start + 10) return '0' + (cp - start);
    }
    if (cp == 0x200B || cp == 0x00A0) return ' ';
    return cp;
}

static uint32_t nextCodepoint(const char** p) {
    const unsigned char* s = (const unsigned char*)*p;
    if (!*s) return 0;
    uint32_t cp;
    int len;
    if (s[0] < 0x80) { cp = s[0]; len = 1; }
    else if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F; len = 2; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; len = 3; }
    else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07; len = 4; }
    else { cp = '?'; len = 1; }
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) { cp = '?'; len = i; break; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p += len;
    return cp;
}

bool Gfx::init() {
    Result rc = framebufferCreate(&m_fb, nwindowGetDefault(), 1280, 720, PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(rc)) return false;
    rc = framebufferMakeLinear(&m_fb);
    if (R_FAILED(rc)) { framebufferClose(&m_fb); return false; }
    m_width = 1280;
    m_height = 720;
    m_ready = true;
    return true;
}

void Gfx::shutdown() {
    if (!m_ready) return;
    framebufferClose(&m_fb);
    m_ready = false;
}

void Gfx::begin() {
    u32 stride = 0;
    m_pixels = (u32*)framebufferBegin(&m_fb, &stride);
    m_stridePx = (int)(stride / 4);
    resetClip();
}

void Gfx::end() {
    framebufferEnd(&m_fb);
    m_pixels = nullptr;
}

void Gfx::setClip(int x, int y, int w, int h) {
    int x0 = std::max(0, x), y0 = std::max(0, y);
    int x1 = std::min(m_width, x + w), y1 = std::min(m_height, y + h);
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    m_clipX = x0; m_clipY = y0; m_clipW = x1 - x0; m_clipH = y1 - y0;
}

void Gfx::resetClip() {
    m_clipX = 0; m_clipY = 0; m_clipW = m_width; m_clipH = m_height;
}

void Gfx::putPixel(int x, int y, u32 c) {
    if (x < m_clipX || y < m_clipY || x >= m_clipX + m_clipW || y >= m_clipY + m_clipH) return;
    m_pixels[y * m_stridePx + x] = c;
}

void Gfx::blendPixel(int x, int y, u32 c) {
    if (x < m_clipX || y < m_clipY || x >= m_clipX + m_clipW || y >= m_clipY + m_clipH) return;
    u32* dst = &m_pixels[y * m_stridePx + x];
    int sa = (c >> 24) & 0xFF;
    if (sa == 255) { *dst = c; return; }
    if (sa == 0) return;
    int dr = *dst & 0xFF, dg = (*dst >> 8) & 0xFF, db = (*dst >> 16) & 0xFF;
    int sr = c & 0xFF, sg = (c >> 8) & 0xFF, sb = (c >> 16) & 0xFF;
    int r = (sr * sa + dr * (255 - sa)) / 255;
    int g = (sg * sa + dg * (255 - sa)) / 255;
    int b = (sb * sa + db * (255 - sa)) / 255;
    *dst = RGBA8(r, g, b, 255);
}

void Gfx::clear(u32 c) {
    for (int y = 0; y < m_height; y++) {
        u32* row = m_pixels + y * m_stridePx;
        for (int x = 0; x < m_width; x++) row[x] = c;
    }
}

void Gfx::fillRect(int x, int y, int w, int h, u32 c) {
    int x0 = std::max(x, m_clipX), y0 = std::max(y, m_clipY);
    int x1 = std::min(x + w, m_clipX + m_clipW), y1 = std::min(y + h, m_clipY + m_clipH);
    for (int yy = y0; yy < y1; yy++) {
        u32* row = m_pixels + yy * m_stridePx;
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

void Gfx::fillRectBlend(int x, int y, int w, int h, u32 c) {
    int x0 = std::max(x, m_clipX), y0 = std::max(y, m_clipY);
    int x1 = std::min(x + w, m_clipX + m_clipW), y1 = std::min(y + h, m_clipY + m_clipH);
    int sa = (c >> 24) & 0xFF;
    if (sa == 0) return;
    int sr = c & 0xFF, sg = (c >> 8) & 0xFF, sb = (c >> 16) & 0xFF;
    for (int yy = y0; yy < y1; yy++) {
        u32* row = m_pixels + yy * m_stridePx;
        for (int xx = x0; xx < x1; xx++) {
            if (sa == 255) { row[xx] = c; continue; }
            u32 d = row[xx];
            int dr = d & 0xFF, dg = (d >> 8) & 0xFF, db = (d >> 16) & 0xFF;
            row[xx] = RGBA8((sr * sa + dr * (255 - sa)) / 255,
                            (sg * sa + dg * (255 - sa)) / 255,
                            (sb * sa + db * (255 - sa)) / 255, 255);
        }
    }
}

void Gfx::vGradient(int x, int y, int w, int h, u32 top, u32 bottom) {
    if (h <= 0) return;
    for (int i = 0; i < h; i++) {
        u32 c = mixColor(top, bottom, (float)i / (float)(h > 1 ? h - 1 : 1));
        fillRect(x, y + i, w, 1, c);
    }
}

void Gfx::hLine(int x, int y, int w, u32 c) { fillRect(x, y, w, 1, c); }

void Gfx::frame(int x, int y, int w, int h, u32 c) {
    hLine(x, y, w, c);
    hLine(x, y + h - 1, w, c);
    fillRect(x, y, 1, h, c);
    fillRect(x + w - 1, y, 1, h, c);
}

void Gfx::blitGlyph(int x, int y, const uint8_t* rows, int scale, u32 color) {
    int sa = (color >> 24) & 0xFF;
    int sr = color & 0xFF, sg = (color >> 8) & 0xFF, sb = (color >> 16) & 0xFF;
    for (int gy = 0; gy < 16; gy++) {
        uint8_t bits = rows[gy];
        if (!bits) continue;
        for (int gx = 0; gx < 8; gx++) {
            if (!(bits & (1 << (7 - gx)))) continue;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    int px = x + gx * scale + sx, py = y + gy * scale + sy;
                    if (px < m_clipX || py < m_clipY || px >= m_clipX + m_clipW || py >= m_clipY + m_clipH) continue;
                    u32* dst = &m_pixels[py * m_stridePx + px];
                    if (sa == 255) { *dst = color; continue; }
                    u32 d = *dst;
                    int dr = d & 0xFF, dg = (d >> 8) & 0xFF, db = (d >> 16) & 0xFF;
                    *dst = RGBA8((sr * sa + dr * (255 - sa)) / 255,
                                 (sg * sa + dg * (255 - sa)) / 255,
                                 (sb * sa + db * (255 - sa)) / 255, 255);
                }
            }
        }
    }
}

void Gfx::text(int x, int y, const char* s, u32 color, int scale, int maxWidth) {
    if (scale < 1) scale = 1;
    int adv = 8 * scale;
    int cursor = x;
    const char* p = s;
    while (*p) {
        uint32_t cp = nextCodepoint(&p);
        if (cp == '\n') { cursor = x; y += 16 * scale; continue; }
        if (maxWidth >= 0 && cursor + adv > x + maxWidth) break;
        const Glyph8x16* g = findGlyph(cp);
        if (!g && cp != ' ') g = findGlyph(normalizeCodepoint(cp));
        if (!g && cp != ' ') g = findGlyph('?');
        if (g) blitGlyph(cursor, y, g->rows, scale, color);
        cursor += adv;
    }
}

int Gfx::textWidth(const char* s, int scale) const {
    int n = 0;
    const char* p = s;
    while (*p) { nextCodepoint(&p); n++; }
    return n * 8 * scale;
}

void Gfx::textCenter(int x, int y, int w, const char* s, u32 color, int scale) {
    int tw = textWidth(s, scale);
    text(x + (w - tw) / 2, y, s, color, scale);
}

static u32 hashStr(const std::string& s, u32 salt) {
    u32 h = 2166136261u ^ salt;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

void Gfx::wave(int x, int y, int w, int h, const std::string& seed, float progress, u32 played, u32 rest) {
    int bars = w / 4;
    if (bars <= 0) return;
    for (int i = 0; i < bars; i++) {
        u32 h1 = hashStr(seed, (u32)i * 2654435761u);
        float v = 0.18f + 0.82f * (float)((h1 >> 8) % 1000) / 1000.0f;
        // add a little shape so it feels musical
        v *= 0.65f + 0.35f * std::fabs(std::sin((float)i * 0.35f + (float)(h1 % 7)));
        int bh = std::max(2, (int)(v * h));
        u32 c = (progress * bars > i) ? played : rest;
        fillRect(x + i * 4, y + (h - bh) / 2, 2, bh, c);
    }
}

void Gfx::artwork(int x, int y, int size, const std::string& seed) {
    u32 a = hashStr(seed, 1), b = hashStr(seed, 2);
    u32 c1 = RGBA8(60 + a % 120, 30 + (a >> 8) % 80, 120 + (a >> 16) % 100, 255);
    u32 c2 = RGBA8(180 + b % 60, 60 + (b >> 8) % 90, 20 + (b >> 16) % 60, 255);
    vGradient(x, y, size, size, c1, c2);
    // subtle diagonal lines
    for (int i = 0; i < size; i += 12) {
        for (int d = 0; d < size; d++) {
            int px = x + d, py = y + (d + i) % size;
            blendPixel(px, py, RGBA8(255, 255, 255, 18));
        }
    }
    // three vinyl rings
    int cx = x + size / 2, cy = y + size / 2;
    for (int r = size / 5; r < size / 2; r += size / 8) {
        for (int a2 = 0; a2 < 720; a2++) {
            float t = (float)a2 * 3.14159265f / 360.0f;
            int px = cx + (int)(std::cos(t) * r);
            int py = cy + (int)(std::sin(t) * r);
            blendPixel(px, py, RGBA8(0, 0, 0, 60));
        }
    }
    fillRectBlend(cx - size / 12, cy - size / 12, size / 6, size / 6, RGBA8(0, 0, 0, 90));
    fillRectBlend(cx - size / 24, cy - size / 24, size / 12, size / 12, RGBA8(255, 85, 0, 220));
}

void Gfx::spinner(int cx, int cy, int r, float angle, u32 color) {
    for (int i = 0; i < 48; i++) {
        float t = angle + (float)i * 3.14159265f * 2.0f / 48.0f;
        int px = cx + (int)(std::cos(t) * r);
        int py = cy + (int)(std::sin(t) * r);
        int alpha = 40 + i * 215 / 48;
        blendPixel(px, py, (color & 0x00FFFFFF) | ((u32)alpha << 24));
        blendPixel(px + 1, py, (color & 0x00FFFFFF) | ((u32)alpha << 24));
    }
}

void Gfx::iconPlay(int x, int y, int size, u32 c) {
    for (int i = 0; i < size; i++) {
        int off = (size / 2) * i / size;
        fillRect(x + i, y + off, 1, size - off * 2, c);
    }
}

void Gfx::iconPause(int x, int y, int size, u32 c) {
    int w = size / 3;
    fillRect(x, y, w, size, c);
    fillRect(x + size - w, y, w, size, c);
}

void Gfx::iconPrev(int x, int y, int size, u32 c) {
    fillRect(x, y, 3, size, c);
    for (int i = 0; i < size; i++) {
        int off = (size / 2) * i / size;
        fillRect(x + size - 1 - i, y + off, 1, size - off * 2, c);
    }
}

void Gfx::iconNext(int x, int y, int size, u32 c) {
    for (int i = 0; i < size; i++) {
        int off = (size / 2) * i / size;
        fillRect(x + i, y + off, 1, size - off * 2, c);
    }
    fillRect(x + size - 3, y, 3, size, c);
}
