#pragma once
// The original presenter: uploads the guest framebuffer to an SDL_Renderer streaming texture
// (XBGR1555 is the board's BGR555 layout, so no conversion) and lets SDL letterbox it to 4:3.
#include "Renderer.hpp"

class SdlRenderer : public Renderer {
public:
    ~SdlRenderer() override;
    bool init(const char* title, int width, int height, uint32_t flags = 0) override;
    void draw(const Frame& f) override;
    SDL_Window* window() const override { return win; }

private:
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    SDL_Texture* tex = nullptr;
    int tex_w = 0, tex_h = 0;
};
