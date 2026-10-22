#pragma once
// Video backends. kinst picks one by name (kinst.ini `renderer = NAME` or --renderer NAME).
// Adding a renderer: subclass Renderer in its own file, then add one line to the table in
// Renderer.cpp. Backends own their window; main.cpp only feeds them frames.
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <SDL.h>

// What the game produced this frame. For now the displayed framebuffer (320x240 BGR555, R in
// bits 0-4, pitch in bytes); per-layer data from hooked draw routines will be added here.
struct Frame {
    const uint16_t* pixels;
    int width, height, pitch;
};

class Renderer {
public:
    virtual ~Renderer() = default;
    // Creates the window. flags: extra SDL_WindowFlags (e.g. SDL_WINDOW_HIDDEN).
    virtual bool init(const char* title, int width, int height, uint32_t flags = 0) = 0;
    virtual void draw(const Frame& f) = 0; // draws and presents one frame
    virtual SDL_Window* window() const = 0;
    // Post-processing effect: a preset name from fx_presets() or a path to a .glsl file.
    // Backends without shaders only accept "none". On failure the current effect stays.
    virtual bool set_fx(const std::string& fx) {
        if (fx == "none") return true;
        err = "effects need the opengl renderer";
        return false;
    }
    virtual std::span<const char* const> fx_presets() const { return {}; }
    const std::string& error() const { return err; }

protected:
    std::string err;
};

struct RendererInfo {
    const char* name;
    const char* description;
    std::unique_ptr<Renderer> (*make)();
};
// Host overlay text (digits, space, F, P, S) in a 3x5 pixel font with a black shadow, stamped
// into a BGR555 frame; x is the left edge of the first glyph. Returns the text's width in pixels.
int text_width(std::string_view s);
void stamp_text(uint16_t* pixels, int width, int height, int pitch, int x, int y, std::string_view s, uint16_t color);

// Widescreen side bars for a 4:3 picture centred in a packed BGR555 frame (`margin` columns each
// side): black, the picture's edges mirrored and dimmed, or a blurred, dimmed mirror of them.
enum class MarginFill { Black, Mirror, Blur };
void fill_margins(uint16_t* pixels, int width, int height, int margin, MarginFill mode);

std::span<const RendererInfo> renderers();               // first entry is the default
std::unique_ptr<Renderer> make_renderer(std::string_view name); // nullptr if unknown
