#include "SdlRenderer.hpp"

SdlRenderer::~SdlRenderer() {
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
}

bool SdlRenderer::init(const char* title, int width, int height, uint32_t flags) {
    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return err = SDL_GetError(), false;
    win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_RESIZABLE | flags);
    if (win) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) return err = SDL_GetError(), false;
    return true;
}

void SdlRenderer::draw(const Frame& f) {
    if (!ren) return;
    if (f.width != tex_w || f.height != tex_h) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XBGR1555, SDL_TEXTUREACCESS_STREAMING, f.width, f.height);
        SDL_RenderSetLogicalSize(ren, f.width, f.height); // keeps 4:3 with letterboxing
        tex_w = f.width, tex_h = f.height;
    }
    SDL_UpdateTexture(tex, nullptr, f.pixels, f.pitch);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, nullptr, nullptr);
    SDL_RenderPresent(ren);
}
