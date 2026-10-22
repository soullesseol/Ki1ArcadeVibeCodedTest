// NativeRenderer smoke test: a 320x240 BGR555 frame in the guest layout (640-byte rows) must come
// out of the GL backbuffer with the right colours, orientation and channel order.
#define SDL_MAIN_HANDLED
#include <cstdio>
#include <vector>
#include <SDL_opengl.h>
#include "../src/NativeRenderer.hpp"

#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(int, char**) {
    NativeRenderer r;
    if (!r.init("renderer_test", 320, 240, SDL_WINDOW_HIDDEN)) { std::printf("FAIL init: %s\n", r.error().c_str()); return 1; }
    // Top half red, bottom half blue, with bit 15 set on blue (must not matter); top-left pixel green.
    std::vector<uint16_t> fb(320 * 240);
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++) fb[y * 320 + x] = y < 120 ? 0x001f : 0xfc00;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) fb[y * 320 + x] = 0x03e0;
    r.draw_background(fb.data(), 320, 240, 640);

    auto read = reinterpret_cast<decltype(&::glReadPixels)>(SDL_GL_GetProcAddress("glReadPixels"));
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(r.window(), &w, &h);
    auto px = [&](int x, int y) { // y from the top of the window
        uint8_t p[4] = {};
        read(x, h - 1 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
        return uint32_t(p[0]) << 16 | p[1] << 8 | p[2];
    };
    std::printf("drawable %dx%d: top-left %06x top %06x bottom %06x\n", w, h, px(1, 1), px(w / 2, h / 4), px(w / 2, h * 3 / 4));
    CHECK(px(1, 1) == 0x00ff00);
    CHECK(px(w / 2, h / 4) == 0xff0000);
    CHECK(px(w / 2, h * 3 / 4) == 0x0000ff);
    r.present();

    // Effects: every preset compiles and draws; a .glsl file works; a broken one is rejected
    // and the previous effect stays.
    for (const char* fx : r.fx_presets()) {
        if (!r.set_fx(fx)) { std::printf("FAIL fx %s: %s\n", fx, r.error().c_str()); return 1; }
        r.draw_background(fb.data(), 320, 240, 640);
        std::printf("fx %s ok\n", fx);
    }
    std::FILE* f = std::fopen("fx_test.glsl", "w");
    std::fputs("void main() { color = vec4(1.0 - pixel(uv), 1.0); }\n", f); // invert
    std::fclose(f);
    CHECK(r.set_fx("fx_test.glsl"));
    r.draw_background(fb.data(), 320, 240, 640);
    CHECK(px(w / 2, h / 4) == 0x00ffff);
    f = std::fopen("fx_test.glsl", "w");
    std::fputs("void main() { oops }\n", f);
    std::fclose(f);
    CHECK(!r.set_fx("fx_test.glsl") && r.error().find("shader") != std::string::npos);
    std::printf("bad shader rejected: %s\n", r.error().c_str());
    CHECK(!r.set_fx("missing.glsl"));
    r.draw_background(fb.data(), 320, 240, 640);
    CHECK(px(w / 2, h / 4) == 0x00ffff); // still the inverting effect
    std::remove("fx_test.glsl");
    r.shutdown();

    // Every registered renderer starts and draws (a new backend gets this check for free).
    for (const RendererInfo& info : renderers()) {
        std::unique_ptr<Renderer> v = make_renderer(info.name);
        CHECK(v);
        if (!v->init(info.name, 320, 240, SDL_WINDOW_HIDDEN)) { std::printf("FAIL %s: %s\n", info.name, v->error().c_str()); return 1; }
        v->draw({fb.data(), 320, 240, 640});
        std::printf("%s ok\n", info.name);
    }
    CHECK(!make_renderer("nope"));

    // Overlay font: "1" is 010/110/010/010/111; its shadow is offset by one pixel.
    std::vector<uint16_t> t(8 * 8, 0x7fff);
    stamp_text(t.data(), 8, 8, 16, 0, 0, "1", 0x03ff);
    CHECK(t[0 * 8 + 1] == 0x03ff && t[1 * 8 + 0] == 0x03ff && t[4 * 8 + 2] == 0x03ff);
    CHECK(t[0 * 8 + 0] == 0x7fff && t[2 * 8 + 2] == 0 && t[5 * 8 + 3] == 0);
    CHECK(text_width("60 FPS") == 23);

    // Widescreen bars: 4-column margins around a 4-column picture whose red channel is 8, 16, 24, 31.
    auto bars = [](MarginFill mode) {
        std::vector<uint16_t> f(12 * 20, 0x7fff);
        for (int y = 0; y < 20; y++)
            for (int x = 0; x < 4; x++) f[y * 12 + 4 + x] = uint16_t(x == 3 ? 31 : 8 * (x + 1));
        fill_margins(f.data(), 12, 20, 4, mode);
        return f;
    };
    std::vector<uint16_t> m = bars(MarginFill::Black);
    CHECK(m[5 * 12 + 0] == 0 && m[5 * 12 + 11] == 0 && m[5 * 12 + 4] == 8);
    m = bars(MarginFill::Mirror); // next to each edge: that edge's column at 5/8 brightness
    CHECK(m[5 * 12 + 3] == 8 * 5 / 8 && m[5 * 12 + 0] == 31 * 5 / 8 && m[5 * 12 + 8] == 31 * 5 / 8 && m[5 * 12 + 11] == 8 * 5 / 8);
    std::vector<uint16_t> flat(12 * 20, 0x7fff); // a flat picture blurs to flat half brightness
    for (int y = 0; y < 20; y++) std::fill_n(&flat[y * 12 + 4], 4, uint16_t(20 | 20 << 5 | 20 << 10));
    fill_margins(flat.data(), 12, 20, 4, MarginFill::Blur);
    CHECK(flat[0] == (10 | 10 << 5 | 10 << 10) && flat[19 * 12 + 11] == (10 | 10 << 5 | 10 << 10));
    std::printf("ok\n");
    return 0;
}
