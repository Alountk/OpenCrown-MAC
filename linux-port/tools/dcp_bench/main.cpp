// Micro-benchmark of the render devices (src/render/device.h), with no game data: how long a BC7 texture upload holds
// up the render thread, and what a frame of many textured draws costs. Same Device classes the game uses, so
// `--renderer gl` (OpenGL; on a Mac BC7 is decoded on the CPU) and `--renderer gpu` (SDL GPU; Metal on a Mac) can be
// compared on one machine.   dcp_bench [--renderer gl|gpu] [--iters N] [--frames N] [--draws N]
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "render/device.h"
#include "render/gl.h"

using namespace dcp::render;
using Clock = std::chrono::steady_clock;

namespace {

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// Valid BC7 blocks: mode 6 (lowest set bit of byte 0 is bit 6), the rest random, so the decoder does real work.
std::vector<uint8_t> bc7_blocks(int w, int h, uint32_t seed) {
    std::vector<uint8_t> b(size_t(w / 4) * size_t(h / 4) * 16);
    uint32_t s = seed * 2654435761u + 1;
    for (size_t i = 0; i < b.size(); i++) {
        s = s * 1664525u + 1013904223u;
        b[i] = uint8_t(s >> 24);
        if (i % 16 == 0) b[i] = uint8_t(0x40 | (b[i] & 0x80));
    }
    return b;
}

struct Stat { double sum = 0, max = 0, min = 1e18; int n = 0;
    void add(double v) { sum += v; max = std::max(max, v); min = std::min(min, v); n++; }
    double mean() const { return n ? sum / n : 0; } };

// Share of RGBA8 pixels whose colour is not black: the sanity check that a draw put something on the target.
double lit_percent(const std::vector<uint8_t>& px) {
    size_t lit = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4) lit += (px[i] | px[i + 1] | px[i + 2]) != 0;
    return px.empty() ? 0.0 : 100.0 * double(lit) / (double(px.size()) / 4.0);
}

Draw quad_draw(TexId target, int tw, int th, TexId tex, const Vtx* v) {
    Draw d{};
    d.target = target; d.tw = tw; d.th = th;
    d.blend.on = true; d.blend.cs = 4; d.blend.cd = 5;  // SRC_ALPHA, ONE_MINUS_SRC_ALPHA
    d.tex[0] = tex;
    d.samp_set[0] = true;  // as the game does: without a sampler GL keeps a mipmapped min filter and the texture is incomplete
    d.v = v; d.n = 6;
    d.raw = true; d.flags = 1;
    for (int i = 0; i < 4; i++) d.tint[i] = d.cst[i] = 1.f;
    return d;
}

void fill_quad(Vtx* v, float x0, float y0, float x1, float y1) {
    const float xs[6] = {x0, x1, x0, x1, x1, x0}, ys[6] = {y0, y0, y1, y0, y1, y1};
    const float us[6] = {0, 1, 0, 1, 1, 0}, vs[6] = {0, 0, 1, 0, 1, 1};
    for (int i = 0; i < 6; i++) {
        Vtx& p = v[i];
        p.pos[0] = xs[i]; p.pos[1] = ys[i]; p.pos[2] = 0; p.pos[3] = 1;
        for (int c = 0; c < 4; c++) p.col[c] = 1.f;
        p.uv[0] = us[i]; p.uv[1] = vs[i]; p.muv[0] = us[i]; p.muv[1] = vs[i];
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string renderer = "gl";
    int iters = 12, frames = 240, draws = 900;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--renderer" && i + 1 < argc) renderer = argv[++i];
        else if (a == "--iters" && i + 1 < argc) iters = std::atoi(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--draws" && i + 1 < argc) draws = std::atoi(argv[++i]);
        else { std::fprintf(stderr, "usage: dcp_bench [--renderer gl|gpu] [--iters N] [--frames N] [--draws N]\n"); return 2; }
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    std::unique_ptr<Device> dev;
    SDL_Window* win = nullptr;
    SDL_GPUDevice* gpu = nullptr;
    if (renderer == "gpu") {
        win = SDL_CreateWindow("dcp_bench", 640, 360, SDL_WINDOW_HIDDEN);
#ifdef __APPLE__
        gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
#else
        gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
#endif
        std::string err;
        if (!win || !gpu || !(dev = make_gpu_device(gpu, nullptr, &err))) {
            std::fprintf(stderr, "SDL GPU: %s\n", err.empty() ? SDL_GetError() : err.c_str());
            return 1;
        }
        std::printf("renderer: SDL GPU (%s)\n", SDL_GetGPUDeviceDriver(gpu));
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        win = SDL_CreateWindow("dcp_bench", 640, 360, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : nullptr;
        if (!ctx) { std::fprintf(stderr, "GL context: %s\n", SDL_GetError()); return 1; }
        if (const char* miss = dcp::gl::load([](const char* n) { return (void*)SDL_GL_GetProcAddress(n); })) {
            std::fprintf(stderr, "missing GL entry point %s\n", miss);
            return 1;
        }
        SDL_GL_SetSwapInterval(0);
        dev = make_gl_device();
        if (!dev || !dev->init()) { std::fprintf(stderr, "GL device init failed\n"); return 1; }
        std::printf("renderer: OpenGL %s\n", (const char*)dcp::gl::glGetString(GL_VERSION));
    }

    const int TW = 1920, TH = 1080;
    TexId target = dev->create_target(TW, TH);
    std::vector<uint8_t> sink;

    // 1. One BC7 upload: the call itself (what the render thread is held for) and until the pixels exist (upload,
    //    a draw that samples it, and a read back of the target).
    std::printf("\nBC7 upload (%d each)                    call ms (mean / max)    until usable ms (mean / max)   target\n", iters);
    Vtx full[6];
    fill_quad(full, -1, -1, 1, 1);
    for (int sz : {64, 256, 512, 1024, 2048}) {
        Stat call, total;
        for (int it = 0; it < iters; it++) {
            auto blocks = bc7_blocks(sz, sz, uint32_t(sz * 31 + it));
            auto t0 = Clock::now();
            dev->begin_frame();
            TexId t = dev->upload(0, TexFormat::BC7, sz, sz, blocks.data(), blocks.size(), false);
            call.add(ms_since(t0));
            Draw d = quad_draw(target, TW, TH, t, full);
            dev->draw(d);
            dev->end_frame();
            dev->read_rgba(target, TW, TH, &sink);
            total.add(ms_since(t0));
            dev->destroy(t);
        }
        std::printf("  %4dx%-4d                                %8.2f / %-8.2f       %8.2f / %-8.2f   %5.1f%% lit\n", sz, sz,
                    call.mean(), call.max, total.mean(), total.max, lit_percent(sink));
    }

    // 2. A frame of many small textured draws on one 512x512 BC7 texture (the game replays about 900 draws a frame).
    auto blocks = bc7_blocks(512, 512, 7);
    TexId tex = dev->upload(0, TexFormat::BC7, 512, 512, blocks.data(), blocks.size(), false);
    std::vector<Vtx> quads(size_t(draws) * 6);
    for (int i = 0; i < draws; i++) {
        float x = -1.f + 1.8f * float(i % 30) / 30.f, y = -1.f + 1.8f * float((i / 30) % 30) / 30.f;
        fill_quad(&quads[size_t(i) * 6], x, y, x + 0.12f, y + 0.12f);
    }
    Stat frame;
    auto tall = Clock::now();
    for (int f = 0; f < frames; f++) {
        auto t0 = Clock::now();
        dev->begin_frame();
        for (int i = 0; i < draws; i++) {
            Draw d = quad_draw(target, TW, TH, tex, &quads[size_t(i) * 6]);
            dev->draw(d);
        }
        dev->end_frame();
        frame.add(ms_since(t0));
    }
    dev->read_rgba(target, TW, TH, &sink);  // wait for the GPU to finish what was submitted
    const double wall = ms_since(tall);
    std::printf("\nframe of %d draws, %d frames: submit ms mean %.2f max %.2f; wall per frame %.2f ms (GPU included); %.1f%% of the target pixels lit\n",
                draws, frames, frame.mean(), frame.max, wall / frames, lit_percent(sink));
    return 0;
}
