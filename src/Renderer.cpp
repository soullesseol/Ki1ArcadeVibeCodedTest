#include "Renderer.hpp"
#include "NativeRenderer.hpp"
#include "SdlRenderer.hpp"
#include <algorithm>
#include <vector>

namespace {
template <class T> std::unique_ptr<Renderer> make() { return std::make_unique<T>(); }

constexpr RendererInfo TABLE[] = {
    {"opengl", "native OpenGL 3.3 renderer (visual mods build on this)", make<NativeRenderer>},
    {"sdl", "original framebuffer blit through SDL_Renderer", make<SdlRenderer>},
};
} // namespace

std::span<const RendererInfo> renderers() { return TABLE; }

namespace {
// 3x5 glyphs, 3 bits per row, top row in the high bits.
uint16_t glyph(char c) {
    static const uint16_t DIGITS[10] = {075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111, 075757, 075717};
    if (c >= '0' && c <= '9') return DIGITS[c - '0'];
    if (c == 'F') return 074644;
    if (c == 'P') return 075744;
    if (c == 'S') return 074717;
    return 0;
}
} // namespace

int text_width(std::string_view s) { return s.empty() ? 0 : int(s.size()) * 4 - 1; }

void stamp_text(uint16_t* pixels, int width, int height, int pitch, int x, int y, std::string_view s, uint16_t color) {
    auto plot = [&](int px, int py, uint16_t c) {
        if (px >= 0 && py >= 0 && px < width && py < height) pixels[py * (pitch / 2) + px] = c;
    };
    for (int pass = 0; pass < 2; pass++) // shadow first, then the text over it
        for (size_t i = 0; i < s.size(); i++)
            for (int row = 0; row < 5; row++)
                for (int col = 0; col < 3; col++)
                    if (glyph(s[i]) >> ((4 - row) * 3 + (2 - col)) & 1)
                        plot(x + int(i) * 4 + col + (pass ? 0 : 1), y + row + (pass ? 0 : 1), pass ? color : 0);
}

std::unique_ptr<Renderer> make_renderer(std::string_view name) {
    for (const RendererInfo& r : TABLE)
        if (name == r.name) return r.make();
    return nullptr;
}

void fill_margins(uint16_t* pixels, int width, int height, int margin, MarginFill mode) {
    const int inner = width - 2 * margin;
    std::vector<int> ch[3]; // one side's bar per colour channel, margin x height (blur only)
    for (int side = 0; side < 2; side++) {
        const int bar = side ? margin + inner : 0;
        // Mirror: bar column j shows the picture column at the same distance from the edge.
        auto src = [&](int y, int j) {
            int x = side ? margin + inner - 1 - j : margin + (margin - 1 - j);
            return pixels[y * width + std::clamp(x, margin, margin + inner - 1)];
        };
        if (mode == MarginFill::Black) {
            for (int y = 0; y < height; y++) std::fill_n(pixels + y * width + bar, margin, uint16_t(0));
            continue;
        }
        if (mode == MarginFill::Mirror) {
            for (int y = 0; y < height; y++)
                for (int j = 0; j < margin; j++) {
                    uint16_t p = src(y, j);
                    int r = (p & 31) * 5 / 8, g = (p >> 5 & 31) * 5 / 8, b = (p >> 10 & 31) * 5 / 8;
                    pixels[y * width + bar + j] = uint16_t(r | g << 5 | b << 10);
                }
            continue;
        }
        // Blur: two passes of a 17-tap box blur in each direction (close to a gaussian), then half brightness.
        for (int c = 0; c < 3; c++) {
            ch[c].assign(size_t(margin) * height, 0);
            for (int y = 0; y < height; y++)
                for (int j = 0; j < margin; j++) ch[c][y * margin + j] = src(y, j) >> (5 * c) & 31;
        }
        constexpr int R = 8;
        auto box = [&](std::vector<int>& v, int n, int count, int step, int stride) {
            std::vector<int> line(n);
            for (int k = 0; k < count; k++) {
                int* p = v.data() + k * stride;
                for (int i = 0; i < n; i++) line[i] = p[i * step];
                int sum = 0;
                for (int i = -R; i <= R; i++) sum += line[std::clamp(i, 0, n - 1)];
                for (int i = 0; i < n; i++) {
                    p[i * step] = sum / (2 * R + 1);
                    sum += line[std::min(i + R + 1, n - 1)] - line[std::max(i - R, 0)];
                }
            }
        };
        for (auto& v : ch)
            for (int pass = 0; pass < 2; pass++) {
                box(v, margin, height, 1, margin); // along rows
                box(v, height, margin, margin, 1); // along columns
            }
        for (int y = 0; y < height; y++)
            for (int j = 0; j < margin; j++) {
                int i = y * margin + j;
                pixels[y * width + bar + j] = uint16_t(ch[0][i] / 2 | (ch[1][i] / 2) << 5 | (ch[2][i] / 2) << 10);
            }
    }
}
