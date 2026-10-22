#include "NativeRenderer.hpp"
#include <SDL_opengl.h>
#include <fstream>
#include <sstream>

namespace {

// GL entry points, loaded at runtime through SDL so nothing links against opengl32/libGL.
#define GL_FUNCS(X)                                                                              \
    X(decltype(&::glViewport), Viewport) X(decltype(&::glClearColor), ClearColor)              \
    X(decltype(&::glClear), Clear) X(decltype(&::glGenTextures), GenTextures)                  \
    X(decltype(&::glDeleteTextures), DeleteTextures) X(decltype(&::glBindTexture), BindTexture) \
    X(decltype(&::glTexParameteri), TexParameteri) X(decltype(&::glTexImage2D), TexImage2D)    \
    X(decltype(&::glTexSubImage2D), TexSubImage2D) X(decltype(&::glPixelStorei), PixelStorei)  \
    X(decltype(&::glDrawArrays), DrawArrays) X(PFNGLACTIVETEXTUREPROC, ActiveTexture)          \
    X(PFNGLCREATESHADERPROC, CreateShader) X(PFNGLSHADERSOURCEPROC, ShaderSource)              \
    X(PFNGLCOMPILESHADERPROC, CompileShader) X(PFNGLGETSHADERIVPROC, GetShaderiv)              \
    X(PFNGLDELETESHADERPROC, DeleteShader) X(PFNGLCREATEPROGRAMPROC, CreateProgram)            \
    X(PFNGLATTACHSHADERPROC, AttachShader) X(PFNGLLINKPROGRAMPROC, LinkProgram)                \
    X(PFNGLGETPROGRAMIVPROC, GetProgramiv) X(PFNGLUSEPROGRAMPROC, UseProgram)                  \
    X(PFNGLDELETEPROGRAMPROC, DeleteProgram) X(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation) \
    X(PFNGLUNIFORM1IPROC, Uniform1i) X(PFNGLGENVERTEXARRAYSPROC, GenVertexArrays)              \
    X(PFNGLBINDVERTEXARRAYPROC, BindVertexArray) X(PFNGLDELETEVERTEXARRAYSPROC, DeleteVertexArrays) \
    X(PFNGLUNIFORM2FPROC, Uniform2f) X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog)            \
    X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog)

struct Gl {
#define X(type, name) type name = nullptr;
    GL_FUNCS(X)
#undef X
    bool load() {
        bool ok = true;
#define X(type, name) ok &= (name = reinterpret_cast<type>(SDL_GL_GetProcAddress("gl" #name))) != nullptr;
        GL_FUNCS(X)
#undef X
        return ok;
    }
} gl;

// Full-screen quad from gl_VertexID (no vertex buffer). The guest framebuffer's row 0 is the top.
const char* VERT = R"(#version 330 core
out vec2 uv;
void main() {
    vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    uv = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";
// Prepended to every effect (presets and .glsl files, which therefore have no #version line).
const char* FX_HEADER = R"(#version 330 core
in vec2 uv;               // 0..1 across the game picture, (0,0) = top-left
uniform sampler2D layer;  // the game picture, bilinear; sample texel centres for sharp pixels
uniform vec2 source_size; // game resolution (320x240)
uniform vec2 output_size; // size of the picture viewport in window pixels
out vec4 color;
vec3 pixel(vec2 p) { return texture(layer, (floor(p * source_size) + 0.5) / source_size).rgb; }
#line 1
)";

// ponytail: single-pass presets; multi-pass chains (xBR, ScaleFX) need render-to-texture passes.
const char* const PRESET_NAMES[] = {"none", "sharp", "scanlines", "crt"};
const char* const PRESET_FRAGS[] = {
    "void main() { color = vec4(pixel(uv), 1.0); }",
    // Sharp bilinear: integer-scale nearest, with a 1-pixel blend only where scaling is fractional.
    R"(void main() {
    vec2 t = uv * source_size, scale = max(floor(output_size / source_size), 1.0);
    vec2 d = fract(t) - 0.5, r = 0.5 - 0.5 / scale;
    color = vec4(texture(layer, (floor(t) + 0.5 + (d - clamp(d, -r, r)) * scale) / source_size).rgb, 1.0);
})",
    // Darkened gaps between the game's 240 lines.
    R"(void main() {
    float d = fract(uv.y * source_size.y) - 0.5;
    color = vec4(min(pixel(uv) * (0.75 + 0.25 * cos(6.2832 * d)) * 1.15, 1.0), 1.0);
})",
    // Arcade monitor: curved glass, soft horizontal beam, scanlines, RGB mask, vignette.
    R"(void main() {
    vec2 p = uv * 2.0 - 1.0;
    p *= 1.0 + vec2(0.04, 0.06) * (p.yx * p.yx);
    vec2 st = p * 0.5 + 0.5;
    if (any(lessThan(st, vec2(0.0))) || any(greaterThan(st, vec2(1.0)))) { color = vec4(0.0, 0.0, 0.0, 1.0); return; }
    float y = st.y * source_size.y;
    vec3 c = texture(layer, vec2(st.x, (floor(y) + 0.5) / source_size.y)).rgb;
    c *= 0.6 + 0.4 * cos(6.2832 * (fract(y) - 0.5));
    vec3 mask = vec3(0.85);
    mask[int(gl_FragCoord.x) % 3] = 1.2;
    c *= mask * (1.0 - 0.15 * dot(p, p)) * 1.25;
    color = vec4(min(c, 1.0), 1.0);
})",
};
static_assert(std::size(PRESET_NAMES) == std::size(PRESET_FRAGS));

unsigned compile(GLenum type, const std::string& src, std::string& log) {
    unsigned s = gl.CreateShader(type);
    const char* p = src.c_str();
    gl.ShaderSource(s, 1, &p, nullptr);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[2048] = {};
        gl.GetShaderInfoLog(s, sizeof buf, nullptr, buf);
        log = buf;
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

} // namespace

bool NativeRenderer::fail(const char* what) {
    err = std::string(what) + ": " + SDL_GetError();
    shutdown();
    return false;
}

bool NativeRenderer::init(const char* title, int width, int height, uint32_t flags) {
    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return fail("SDL video");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | flags);
    if (!win) return fail("window");
    gl_ctx = SDL_GL_CreateContext(win);
    if (!gl_ctx) return fail("OpenGL 3.3 context");
    if (!gl.load()) return fail("OpenGL functions");
    SDL_GL_SetSwapInterval(1); // vsync; not fatal if the driver refuses

    if (!(program = build(PRESET_FRAGS[0]))) { SDL_SetError("%s", err.c_str()); return fail("renderer shaders"); }

    gl.GenVertexArrays(1, &vao); // core profile needs a bound VAO even without attributes
    gl.BindVertexArray(vao);

    gl.GenTextures(1, &bg_tex);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, bg_tex);
    // Bilinear for effects that want it; pixel() in FX_HEADER samples texel centres (= nearest).
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

unsigned NativeRenderer::build(const std::string& frag) {
    std::string log;
    unsigned vs = compile(GL_VERTEX_SHADER, VERT, log), fs = vs ? compile(GL_FRAGMENT_SHADER, FX_HEADER + frag, log) : 0;
    unsigned p = gl.CreateProgram();
    if (vs) gl.AttachShader(p, vs);
    if (fs) gl.AttachShader(p, fs);
    GLint linked = 0;
    if (vs && fs) {
        gl.LinkProgram(p);
        gl.GetProgramiv(p, GL_LINK_STATUS, &linked);
        if (!linked) {
            char buf[2048] = {};
            gl.GetProgramInfoLog(p, sizeof buf, nullptr, buf);
            log = buf;
        }
    }
    if (vs) gl.DeleteShader(vs);
    if (fs) gl.DeleteShader(fs);
    if (!linked) { gl.DeleteProgram(p); err = "shader: " + log; return 0; }
    gl.UseProgram(p);
    gl.Uniform1i(gl.GetUniformLocation(p, "layer"), 0);
    return p;
}

std::span<const char* const> NativeRenderer::fx_presets() const { return PRESET_NAMES; }

bool NativeRenderer::set_fx(const std::string& fx) {
    if (!gl_ctx) { err = "renderer not started"; return false; }
    std::string frag;
    for (size_t i = 0; i < std::size(PRESET_NAMES); i++) if (fx == PRESET_NAMES[i]) frag = PRESET_FRAGS[i];
    if (frag.empty()) {
        std::ifstream f(fx, std::ios::binary);
        if (!f) { err = "unknown effect '" + fx + "' (not a preset or a readable .glsl file)"; return false; }
        std::stringstream ss;
        ss << f.rdbuf();
        frag = ss.str();
    }
    unsigned p = build(frag);
    if (!p) { err = fx + ": " + err; gl.UseProgram(program); return false; }
    gl.DeleteProgram(program);
    program = p;
    return true;
}

void NativeRenderer::shutdown() {
    if (gl_ctx) {
        if (bg_tex) gl.DeleteTextures(1, &bg_tex);
        if (vao) gl.DeleteVertexArrays(1, &vao);
        if (program) gl.DeleteProgram(program);
        SDL_GL_DeleteContext(gl_ctx);
    }
    if (win) SDL_DestroyWindow(win);
    win = nullptr, gl_ctx = nullptr, program = vao = bg_tex = 0, bg_w = bg_h = 0;
}

// Largest rectangle with the picture's aspect (square pixels: 4:3 for 320x240) centred in the
// drawable; the rest is cleared to black.
void NativeRenderer::viewport(int pic_w, int pic_h) {
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(win, &w, &h);
    gl.Viewport(0, 0, w, h);
    gl.ClearColor(0, 0, 0, 1);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    int vw = w, vh = w * pic_h / pic_w;
    if (vh > h) vh = h, vw = h * pic_w / pic_h;
    gl.Viewport((w - vw) / 2, (h - vh) / 2, vw, vh);
    out_w = vw, out_h = vh;
}

void NativeRenderer::draw_background(const uint16_t* pixels, int width, int height, int pitch) {
    if (!gl_ctx) return;
    viewport(width, height);
    gl.BindTexture(GL_TEXTURE_2D, bg_tex);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 2);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, pitch / 2);
    // BGR555 with R in the low bits is exactly GL's RGBA + UNSIGNED_SHORT_1_5_5_5_REV.
    if (width != bg_w || height != bg_h) {
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, width, height, 0, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, pixels);
        bg_w = width, bg_h = height;
    } else {
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, pixels);
    }
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.UseProgram(program);
    gl.Uniform2f(gl.GetUniformLocation(program, "source_size"), float(width), float(height));
    gl.Uniform2f(gl.GetUniformLocation(program, "output_size"), float(out_w), float(out_h));
    gl.BindVertexArray(vao);
    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void NativeRenderer::present() {
    if (win) SDL_GL_SwapWindow(win);
}
