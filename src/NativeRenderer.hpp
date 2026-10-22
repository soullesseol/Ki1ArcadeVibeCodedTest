#pragma once
// Phase 4 native renderer: SDL2 window + OpenGL 3.3 core context (Windows, Linux, Steam Deck).
// Registered as "opengl" in Renderer.cpp.
//
// Layer model: the background layer is a raw guest pixel buffer (KI's 320x240 BGR555 framebuffer,
// R in bits 0-4, bit 15 ignored) drawn as a textured quad, letterboxed to its own aspect
// (square pixels: 4:3 for 320x240, wider for the widescreen buffer).
// Later layers (fighters, HUD, mods) draw on top of it before present().
// Effects (set_fx) are fragment shaders that draw the game picture into the 4:3 viewport.
#include "Renderer.hpp"

class NativeRenderer : public Renderer {
public:
    NativeRenderer() = default;
    NativeRenderer(const NativeRenderer&) = delete;
    NativeRenderer& operator=(const NativeRenderer&) = delete;
    ~NativeRenderer() override { shutdown(); }

    // Creates the window and GL context (initializes SDL video if needed).
    // flags: extra SDL_WindowFlags, e.g. SDL_WINDOW_HIDDEN for tests.
    bool init(const char* title, int width, int height, uint32_t flags = 0) override;
    void shutdown();

    // Uploads a BGR555 buffer and draws it as the background layer, filling the 4:3 viewport.
    // pitch is in bytes (the KI framebuffer is 640). The size may change between calls.
    void draw_background(const uint16_t* pixels, int width, int height, int pitch);
    void present();
    void draw(const Frame& f) override { draw_background(f.pixels, f.width, f.height, f.pitch); present(); }

    SDL_Window* window() const override { return win; }
    bool set_fx(const std::string& fx) override;
    std::span<const char* const> fx_presets() const override;

private:
    bool fail(const char* what);
    void viewport(int pic_w, int pic_h);
    unsigned build(const std::string& frag); // links VERT + FX_HEADER + frag; 0 (and err) on failure

    SDL_Window* win = nullptr;
    SDL_GLContext gl_ctx = nullptr;
    unsigned program = 0, vao = 0, bg_tex = 0;
    int bg_w = 0, bg_h = 0, out_w = 0, out_h = 0;
};
