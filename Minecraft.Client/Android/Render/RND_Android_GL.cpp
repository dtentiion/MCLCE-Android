// The renderer's lifecycle hooks and the world frame. While no world is
// running the frame is a placeholder triangle, so a glance shows the
// renderer is alive.

#include <android/log.h>

#include <atomic>
#include <cstdint>
#include <ctime>

#include "GLInternal.h"
#include "MetalContext.h"
#include "RND_iOS_Stub.h"

// World render bridge (MCLEGameLoop.cpp). Once the world simulation is
// ticking the frame is cleared to its sky colour and the level is drawn.
extern "C" int  mcle_world_is_ticking(void);
extern "C" void mcle_world_get_sky_color(float* r, float* g, float* b);

// Per-frame upstream renderer drive. No-op if there's no level renderer.
extern "C" void mcle_world_drive_renderer(void);
extern "C" void mcle_metal_add_phase_ms(int phase, double ms);

namespace {

const char* const kTriangleVertSrc = R"(#version 300 es
out vec3 v_color;
void main() {
    vec2 positions[3] = vec2[3](vec2(0.0, 0.6), vec2(-0.6, -0.5), vec2(0.6, -0.5));
    vec3 colors[3] = vec3[3](vec3(0.95, 0.32, 0.32), vec3(0.32, 0.95, 0.45), vec3(0.32, 0.55, 0.95));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
    v_color = colors[gl_VertexID];
}
)";

const char* const kTriangleFragSrc = R"(#version 300 es
precision mediump float;
in vec3 v_color;
out vec4 o_color;
void main() {
    o_color = vec4(v_color, 1.0);
}
)";

std::atomic<bool> g_ready{false};

// Built in the first frame that needs it, a context is only current then
void draw_placeholder() {
    static GLuint s_program = 0;
    static bool   s_failed  = false;
    if (!s_program && !s_failed) {
        s_program = mcle_gl::build_program("triangle", kTriangleVertSrc, kTriangleFragSrc);
        s_failed  = (s_program == 0);
    }
    mcle_gl::draw_screen_triangle(s_program);
}

uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

} // namespace

// The world layer's ANativeWindow and the size to draw it at. Called on the
// thread that will run the frames.
extern "C" int mcle_render_init(void* native_window, int width, int height) {
    if (mcle_metal_ensure_device() != 0) return 1;
    mcle_metal_attach_layer(native_window, width, height);
    g_ready.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, "mcle", "[mcle_render] init ok: %dx%d", width, height);
    return 0;
}

extern "C" void mcle_render_resize(int width, int height) {
    mcle_gl::resize_layer(width, height);
}

extern "C" void mcle_render_frame(void) {
    if (!g_ready.load(std::memory_order_acquire)) return;

    const bool worldAlive = (mcle_world_is_ticking() != 0);

    // Charcoal before there's a world, its sky colour once it's ticking
    float cr, cg, cb;
    if (worldAlive) {
        mcle_world_get_sky_color(&cr, &cg, &cb);
    } else {
        cr = 0.07f; cg = 0.07f; cb = 0.09f;
    }
    // Minecraft::run captures the save thumbnail from the world frame
    if (worldAlive) mcle_metal_thumbnail_world_frame();
    if (mcle_metal_frame_begin(cr, cg, cb, 1.0f) != 0) return;

    if (!worldAlive) {
        draw_placeholder();
    } else {
        // Upstream LevelRenderer::render(): chunk geometry, entity renderers
        // and clouds come back through Tesselator into DrawVertices
        const uint64_t t0 = now_ns();
        mcle_world_drive_renderer();
        mcle_metal_add_phase_ms(1, (double)(now_ns() - t0) / 1.0e6);
    }

    mcle_metal_frame_end();
}

extern "C" void mcle_render_shutdown(void) {
    g_ready.store(false, std::memory_order_release);
}
