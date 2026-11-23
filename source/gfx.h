#pragma once
#include <switch.h>
#include <stdint.h>
#include <string>

// SoundCloud-ish palette
namespace pal {
constexpr u32 bg        = 0xFF121212;
constexpr u32 sidebar   = 0xFF0B0B0B;
constexpr u32 card      = 0xFF1B1B1B;
constexpr u32 cardHover = 0xFF232323;
constexpr u32 line      = 0xFF2A2A2A;
constexpr u32 text      = 0xFFFFFFFF;
constexpr u32 textDim   = 0xFF9A9A9A;
constexpr u32 textMute  = 0xFF6E6E6E;
constexpr u32 accent    = 0xFFFF5500;
constexpr u32 accentDim = 0xFF7A2A05;
constexpr u32 green     = 0xFF2ECC71;
constexpr u32 red       = 0xFFE74C3C;
}

inline u32 rgba(u8 r, u8 g, u8 b, u8 a = 255) { return RGBA8(r, g, b, a); }
u32 mixColor(u32 a, u32 b, float t);

class Gfx {
public:
    bool init();
    void shutdown();

    void begin();
    void end();

    int width() const { return m_width; }
    int height() const { return m_height; }

    void clear(u32 c);
    void fillRect(int x, int y, int w, int h, u32 c);
    void fillRectBlend(int x, int y, int w, int h, u32 c);
    void vGradient(int x, int y, int w, int h, u32 top, u32 bottom);
    void hLine(int x, int y, int w, u32 c);
    void frame(int x, int y, int w, int h, u32 c);

    void setClip(int x, int y, int w, int h);
    void resetClip();

    void text(int x, int y, const char* s, u32 color, int scale = 1, int maxWidth = -1);
    int textWidth(const char* s, int scale = 1) const;
    // Draws text centered inside [x, x+w)
    void textCenter(int x, int y, int w, const char* s, u32 color, int scale = 1);

    void wave(int x, int y, int w, int h, const std::string& seed, float progress, u32 played, u32 rest);
    void artwork(int x, int y, int size, const std::string& seed);
    void spinner(int cx, int cy, int r, float angle, u32 color);
    void iconPlay(int x, int y, int size, u32 color);
    void iconPause(int x, int y, int size, u32 color);
    void iconPrev(int x, int y, int size, u32 color);
    void iconNext(int x, int y, int size, u32 color);

private:
    void putPixel(int x, int y, u32 c);
    void blendPixel(int x, int y, u32 c);
    void blitGlyph(int x, int y, const uint8_t* rows, int scale, u32 color);

    Framebuffer m_fb{};
    u32* m_pixels = nullptr;
    int m_stridePx = 0;
    int m_width = 0, m_height = 0;
    bool m_ready = false;
    int m_clipX = 0, m_clipY = 0, m_clipW = 0, m_clipH = 0;
};

extern Gfx gfx;
