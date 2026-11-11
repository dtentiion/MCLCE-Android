// OpenGL ES 3.0 renderer behind the game's fixed-function GL bridge: matrix
// stacks, fog, lighting, the lightmap, display lists and immediate draws, on
// a world layer and a transparent UI layer over the movies.
//
// GL is only touched between a frame's begin and end (and when a layer is
// attached), on the thread that runs the frames. The rest of the bridge can
// be called from any thread: render state is per thread, and textures and
// lists stay on the CPU until a frame picks them up.

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <android/native_window.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "GLInternal.h"
#include "MetalContext.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "mcle", __VA_ARGS__)

// The log file the game's own lines go to (png_decode.cpp)
extern "C" int mcle_log_msg(const char* msg);

extern "C" int  mcle_png_decode_rgba8(const void* data, unsigned long length,
                                       unsigned char** out_rgba, int* out_w, int* out_h);
extern "C" void mcle_png_decode_free(unsigned char* pixels);

namespace {

std::atomic<unsigned long long> g_draw_count{0};

uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// A window the renderer draws to. The size is the one asked for, the
// window's buffers are set to it and the system scales them to the view.
struct Layer {
    ANativeWindow* window  = nullptr;
    EGLSurface     surface = EGL_NO_SURFACE;
    int            width   = 0;
    int            height  = 0;
};

struct Ctx {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig  config  = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLint     visual  = 0;

    Layer world;
    // Over the movies, where the custom draw regions go (items in hotbar
    // and container slots). Cleared to transparent each frame.
    Layer ui;

    bool   inFrame    = false;
    Layer* frameLayer = nullptr;
    // Gamma: when it isn't 1 the frame is drawn into the scene target and
    // copied to the window through the gamma pass, like Windows64's
    // PostProcesser
    bool gammaThisFrame = false;
};

Ctx g;

// Matrices, column major like GL's

struct Mat4 { float m[16]; };

inline Mat4 mat_identity() {
    return Mat4{{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
}

inline void mat_mul(float* out, const float* a, const float* b) {
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            float s = 0;
            for (int k = 0; k < 4; k++) {
                s += a[i + k * 4] * b[k + j * 4];
            }
            out[i + j * 4] = s;
        }
    }
}

inline void mat_translate(float* m, float x, float y, float z) {
    for (int i = 0; i < 4; i++) {
        m[12 + i] = m[i] * x + m[4 + i] * y + m[8 + i] * z + m[12 + i];
    }
}

inline void mat_rotate(float* m, float angle_deg, float ax, float ay, float az) {
    const float a   = angle_deg * (float)M_PI / 180.0f;
    const float c   = cosf(a);
    const float s   = sinf(a);
    const float n   = sqrtf(ax * ax + ay * ay + az * az);
    if (n > 0.0f) { ax /= n; ay /= n; az /= n; }
    const float omc = 1.0f - c;
    const float r[16] = {
        ax*ax*omc +    c,  ay*ax*omc + az*s,  az*ax*omc - ay*s,  0,
        ax*ay*omc - az*s,  ay*ay*omc +    c,  az*ay*omc + ax*s,  0,
        ax*az*omc + ay*s,  ay*az*omc - ax*s,  az*az*omc +    c,  0,
        0,                 0,                 0,                 1,
    };
    float result[16];
    mat_mul(result, m, r);
    memcpy(m, result, sizeof(result));
}

inline void mat_scale(float* m, float sx, float sy, float sz) {
    for (int i = 0; i < 4; i++) {
        m[i + 0] *= sx;
        m[i + 4] *= sy;
        m[i + 8] *= sz;
    }
}

// XMMatrixInverse
bool mat_inverse(const float* m, float* out) {
    float inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det == 0.0f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) out[i] = inv[i] * det;
    return true;
}

constexpr int kMatrixModeModelview  = 0;
constexpr int kMatrixModeProjection = 1;
constexpr int kMatrixModeTexture    = 2;
constexpr int kGL_MODELVIEW         = 0x1700;
constexpr int kGL_PROJECTION        = 0x1701;
constexpr int kGL_TEXTURE           = 0x1702;
constexpr int kGL_QUADS             = 7;

// Bytes per vertex. Tesselator's _array stores 8 ints a vertex for the
// PF3_TF2_CB4_NB4_XW1 layout and 4 for the compressed one.
inline int vertex_stride(int fmt) {
    switch (fmt) {
        case 1: /* VERTEX_TYPE_PF3_TF2_CB4_NB4_XW1        */ return 32;
        case 2: /* VERTEX_TYPE_PF3_TF2_CB4_NB4_XW1_TEXGEN */ return 32;
        case 3: /* VERTEX_TYPE_PS3_TS2_CS1                */ return 12;
        case 4: /* VERTEX_TYPE_COMPRESSED                 */ return 16;
        default:                                              return 32;
    }
}

inline bool drawable_format(int fmt) {
    return fmt == 1 || fmt == 2 || fmt == 4;
}

// Display lists. glNewList records the DrawVertices payloads on the CPU,
// from whichever thread compiles the list, and glCallList replays them. The
// first replay in a frame moves a list's vertices into a buffer of its own.

struct DrawCmd {
    int          prim   = 0;
    int          count  = 0;
    int          fmt    = 0;
    int          shader = 0;
    // Where the vertices start in the list's blob, then in its buffer
    size_t       offset = 0;
    // The chunk's world translation (xRenderOffs/yRenderOffs/zRenderOffs)
    // at record time, applied on top of the modelview at replay
    float        translate[3] = {0, 0, 0};
    bool         hasTranslate = false;
    // A glBindTexture compiled into the list before this draw, 0 if none.
    // Without one the draw uses whatever is bound when the list is called.
    unsigned int texId  = 0;
    // The vertex layout at this draw's offset, made with the list's buffer
    GLuint       vao    = 0;
};

// Buffers and vertex arrays dropped by any thread, deleted at the start of
// the next frame
std::mutex          g_dead_mu;
std::vector<GLuint> g_dead_buffers;
std::vector<GLuint> g_dead_vaos;

struct DisplayList {
    std::vector<DrawCmd> draws;
    std::vector<uint8_t> blob;
    // Size of the vertices, in the blob or in the buffer
    size_t               bytes    = 0;
    GLuint               vbo      = 0;
    bool                 uploaded = false;

    DisplayList() = default;
    DisplayList(const DisplayList&) = delete;
    DisplayList& operator=(const DisplayList&) = delete;
    DisplayList(DisplayList&& o) noexcept { take(o); }
    DisplayList& operator=(DisplayList&& o) noexcept {
        if (this != &o) {
            retire();
            take(o);
        }
        return *this;
    }
    ~DisplayList() { retire(); }

    void take(DisplayList& o) {
        draws    = std::move(o.draws);
        blob     = std::move(o.blob);
        bytes    = o.bytes;
        vbo      = o.vbo;
        uploaded = o.uploaded;
        o.draws.clear();
        o.blob.clear();
        o.bytes    = 0;
        o.vbo      = 0;
        o.uploaded = false;
    }

    // A list can be dropped by a chunk rebuild thread, its GL objects can't
    void retire() {
        if (vbo) {
            std::lock_guard<std::mutex> lk(g_dead_mu);
            g_dead_buffers.push_back(vbo);
            for (const DrawCmd& cmd : draws) {
                if (cmd.vao) g_dead_vaos.push_back(cmd.vao);
            }
        }
        draws.clear();
        blob.clear();
        bytes    = 0;
        vbo      = 0;
        uploaded = false;
    }
};

// Workers spawned by LevelRenderer build chunk meshes into here in parallel
// while the frame thread replays, so readers share the lock and only
// inserting or erasing takes it alone.
std::unordered_map<int, DisplayList> g_lists;
std::shared_mutex                    g_lists_mu;
std::atomic<int>                     g_next_list_id{1};

// Everything glEnable, glMatrixMode and the rest set, one copy per thread.
// Chunk::rebuild walks the matrix stacks on its worker the way the frame
// thread does, and neither may see the other's.
struct ThreadState {
    // Set on the frame thread while a frame is open, the only time and
    // place GL calls are made
    bool in_frame = false;

    int               matrix_mode = kMatrixModeModelview;
    std::vector<Mat4> modelview{mat_identity()};
    std::vector<Mat4> projection{mat_identity()};
    // renderAdvancedClouds scrolls the cloud pattern with this one
    std::vector<Mat4> texture{mat_identity()};

    // RenderManager.StateSetTexGenCol's matTexGenView and matTexGenObj, a
    // plane in each column
    float texgen_eye[16] = {};
    float texgen_obj[16] = {};

    // GL_CURRENT_COLOR, multiplied into the vertex colour
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    // The translation of the chunk list being replayed
    float chunk_offset[3] = {0, 0, 0};

    bool  depth_test  = true;
    bool  depth_write = true;
    int   depth_func  = GL_LEQUAL;
    // GL_CULL_FACE, only honoured for the chunk format (the rest never culled)
    bool  cull_face   = true;
    bool  texture_2d  = true;
    bool  alpha_test  = true;
    // glAlphaFunc's ref. The 4J renderer ignores the func, its pixel shaders
    // discard when the final alpha is below the ref.
    float alpha_ref   = 0.1f;
    bool  blend       = false;
    int   blend_src   = GL_SRC_ALPHA;
    int   blend_dst   = GL_ONE_MINUS_SRC_ALPHA;
    bool  polygon_offset_fill   = false;
    float polygon_offset_factor = 0.0f;
    float polygon_offset_units  = 0.0f;

    bool  fog         = false;
    float fog_color[3] = {0.0f, 0.0f, 0.0f};
    float fog_start   = 0.0f;
    float fog_end     = 1.0f;
    // GL_FOG_MODE: 0 GL_LINEAR, 1 GL_EXP with fog_density
    int   fog_mode    = 0;
    float fog_density = 1.0f;

    // GL_LIGHTING the way the 4J renderer does it: two directional lights in
    // eye space and an ambient colour. A light that's off counts as black.
    bool  lighting          = false;
    bool  light_enabled[2]  = {false, false};
    float light_dir[2][3]   = {{0, 0, 0}, {0, 0, 0}};
    float light_col[2][3]   = {{0, 0, 0}, {0, 0, 0}};
    float light_ambient[3]  = {0, 0, 0};

    // GameRenderer::turnOnLightLayer: the lightmap for the formats other
    // than the chunks', at the vertex's own tex2 or, where it has none, at
    // StateSetVertexTextureUV's, kept as u - 1 and v - 1
    bool  light_layer  = false;
    float vertex_uv[2] = {-1.0f, -1.0f};

    // The list being compiled on this thread and what it has so far, put
    // into g_lists in one go at glEndList
    int          recording_list = 0;
    DisplayList  recording;
    // GL_COMPILE records a bind instead of doing it
    unsigned int recording_tex = 0;
    float        recording_translate[3] = {0, 0, 0};
    bool         recording_has_translate = false;

    // C4JRender::CBuffDeferredModeStart / End: the lists finished while
    // deferred are swapped in together, so the near chunks updateDirtyChunks
    // rebuilds as a group show up in the same frame
    bool deferred_on = false;
    std::vector<std::pair<int, DisplayList>> deferred;
};

thread_local ThreadState t_state;

inline std::vector<Mat4>& current_stack(ThreadState& s) {
    switch (s.matrix_mode) {
        case kMatrixModeProjection: return s.projection;
        case kMatrixModeTexture:    return s.texture;
        default:                    return s.modelview;
    }
}

// Textures. Ids are handed out and images are taken from any thread, the GL
// objects are made from the queued images in a frame.

struct TexOp {
    unsigned int         id     = 0;
    int                  x      = 0;
    int                  y      = 0;
    int                  width  = 0;
    int                  height = 0;
    // A new image for the texture, otherwise a region of the one it has
    bool                 whole  = false;
    bool                 remove = false;
    // Empty for an image given no pixels
    std::vector<uint8_t> pixels;
};

struct TexSize { int width; int height; };

std::mutex                                g_tex_mu;
// The textures that have an image, as of the last call made
std::unordered_map<unsigned int, TexSize> g_tex_sizes;
std::vector<TexOp>                        g_tex_ops;
std::atomic<bool>                         g_tex_ops_waiting{false};
std::atomic<unsigned int>                 g_next_tex_id{1};
// One binding for every thread: the loading threads bind an id, then
// upload to whichever id is bound
std::atomic<unsigned int>                 g_bound_tex_id{0};

// RenderManager.TextureSetParam's state for each texture id: bit 0 clamps s,
// bit 1 clamps t, bit 2 is a linear min filter and bit 3 a linear mag one.
// Nothing set is nearest and repeat.
constexpr unsigned int kTexParamSlots = 1u << 16;
std::atomic<uint8_t>   g_tex_params[kTexParamSlots];

std::mutex                                    g_path_mu;
std::unordered_map<std::string, unsigned int> g_path_to_tex;

// 16x16 lightmap, indexed by (skyLevel << 4) | blockLevel. White until
// GameRenderer::updateLightTexture's first update.
struct Lightmap {
    // What mcle_lightmap_set_entry writes
    uint8_t pixels[16 * 16 * 4];
    // The entries as of the last upload, what the texture gets
    uint8_t staged[16 * 16 * 4];
    Lightmap() {
        memset(pixels, 255, sizeof(pixels));
        memset(staged, 255, sizeof(staged));
    }
};

Lightmap          g_lightmap;
std::mutex        g_lightmap_mu;
std::atomic<bool> g_lightmap_dirty{false};

std::atomic<float> g_gamma{1.0f};

// From here to the end of the namespace is the frame thread's alone.

struct GLTex { GLuint name = 0; int width = 0; int height = 0; };
std::unordered_map<unsigned int, GLTex> g_gl_textures;
// The last id looked up for a draw, until the textures change
unsigned int g_resolved_id    = 0;
GLuint       g_resolved_name  = 0;
bool         g_resolved_valid = false;

// The uniforms a program was last sent, so a draw only sends what changed
struct SentUniforms {
    float mvp[16];
    float tex_matrix[16];
    float color[4];
    float mv_row2[4];
    float fog[8];
    float light[36];
    float alpha_ref;
    float texgen;
    float chunk[8];
};

struct WorldProgram {
    GLuint id = 0;
    GLint  loc_mvp = -1;
    GLint  loc_tex_matrix = -1;
    GLint  loc_color = -1;
    GLint  loc_mv_row2 = -1;
    GLint  loc_fog = -1;
    GLint  loc_alpha_ref = -1;
    GLint  loc_light = -1;
    GLint  loc_texgen = -1;
    GLint  loc_texgen_mats = -1;
    GLint  loc_chunk = -1;
    SentUniforms sent;
};

WorldProgram g_prog_std;
WorldProgram g_prog_compact;
bool         g_objects_tried = false;
bool         g_world_ready   = false;

GLuint g_prog_gamma       = 0;
GLint  g_gamma_params_loc = -1;
GLuint g_prog_thumb       = 0;

// One sampler for each combination of the texture parameter bits
GLuint g_samplers[16];
constexpr int kSamplerNearestClamp = 1 | 2;
constexpr int kSamplerLinearClamp  = 1 | 2 | 4 | 8;
// The lightmap is filtered so a fragment between two cells gets a mix of
// them, soft gradients across a block face instead of an edge per cell
GLuint g_lightmap_sampler = 0;
// 1x1 white, drawn with when texturing is off or nothing is bound, and in
// the lightmap's place outside the light layer
GLuint g_white_tex    = 0;
GLuint g_lightmap_tex = 0;

// GL_QUADS as triangles (0,1,2 and 0,2,3 for each quad). Short indices
// cover 16384 quads, a longer draw gets the 32 bit ones.
constexpr int kQuadIbo16Quads = 16384;
GLuint g_quad_ibo16       = 0;
GLuint g_quad_ibo32       = 0;
int    g_quad_ibo32_quads = 0;

// Immediate draws are appended to this buffer and drawn from where they
// landed. One vertex array per layout, both over the whole buffer.
GLuint g_stream_vbo    = 0;
GLuint g_stream_vao[2] = {0, 0};
size_t g_stream_cap    = 4u << 20;
size_t g_stream_cursor = 0;

// With GL_EXT_buffer_storage the buffer stays mapped and the vertices are
// written straight into it. Mapping a range for every draw has the Adreno
// driver allocate memory each time.
constexpr GLbitfield kMapPersistent = 0x0040; // GL_MAP_PERSISTENT_BIT_EXT
constexpr GLbitfield kMapCoherent   = 0x0080; // GL_MAP_COHERENT_BIT_EXT
typedef void (*BufferStorageFn)(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags);
BufferStorageFn g_buffer_storage = nullptr;
uint8_t*        g_stream_map     = nullptr;
// Written since the frame was opened
size_t          g_stream_frame_bytes = 0;

// World frames presented so far
std::atomic<unsigned long> g_world_frames{0};

// The key of the frame the UI layer shows, when it was drawn for one
bool               g_ui_key_shown = false;
bool               g_ui_key_next  = false;
unsigned long long g_ui_key       = 0;

GLuint g_scene_fbo    = 0;
GLuint g_scene_tex    = 0;
GLuint g_scene_depth  = 0;
int    g_scene_w      = 0;
int    g_scene_h      = 0;

// What the context was last told, so a draw only changes what differs.
// Forgotten at the start of each frame.
constexpr GLuint kUnknown = 0xFFFFFFFFu;

struct GLState {
    GLuint program      = kUnknown;
    GLuint vao          = kUnknown;
    GLuint array_buffer = kUnknown;
    GLuint tex0         = kUnknown;
    GLuint sampler0     = kUnknown;
    GLuint tex1         = kUnknown;
    int    blend        = -1;
    int    blend_src    = -1;
    int    blend_dst    = -1;
    int    depth_test   = -1;
    int    depth_func   = -1;
    int    depth_write  = -1;
    int    cull         = -1;
    int    offset_fill  = -1;
    float  offset_factor = NAN;
    float  offset_units  = NAN;
};

GLState g_gl;

inline void gl_use_program(GLuint program) {
    if (g_gl.program == program) return;
    glUseProgram(program);
    g_gl.program = program;
}

inline void gl_bind_vao(GLuint vao) {
    if (g_gl.vao == vao) return;
    glBindVertexArray(vao);
    g_gl.vao = vao;
}

inline void gl_bind_array_buffer(GLuint buffer) {
    if (g_gl.array_buffer == buffer) return;
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    g_gl.array_buffer = buffer;
}

// Unit 0 stays the active one
inline void gl_bind_tex0(GLuint tex) {
    if (g_gl.tex0 == tex) return;
    glBindTexture(GL_TEXTURE_2D, tex);
    g_gl.tex0 = tex;
}

inline void gl_bind_sampler0(GLuint sampler) {
    if (g_gl.sampler0 == sampler) return;
    glBindSampler(0, sampler);
    g_gl.sampler0 = sampler;
}

inline void gl_bind_tex1(GLuint tex) {
    if (g_gl.tex1 == tex) return;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex);
    glActiveTexture(GL_TEXTURE0);
    g_gl.tex1 = tex;
}

inline void gl_set_cap(int& cached, GLenum cap, bool on) {
    if (cached == (on ? 1 : 0)) return;
    if (on) glEnable(cap);
    else    glDisable(cap);
    cached = on ? 1 : 0;
}

inline void gl_set_blend(bool on)      { gl_set_cap(g_gl.blend, GL_BLEND, on); }
inline void gl_set_depth_test(bool on) { gl_set_cap(g_gl.depth_test, GL_DEPTH_TEST, on); }
inline void gl_set_cull(bool on)       { gl_set_cap(g_gl.cull, GL_CULL_FACE, on); }

inline GLenum gl_blend_factor(int f) {
    switch (f) {
    case 0x0000: case 0x0001:
    case 0x0300: case 0x0301: case 0x0302: case 0x0303: case 0x0304:
    case 0x0305: case 0x0306: case 0x0307: case 0x0308:
    case 0x8001: case 0x8002: case 0x8003: case 0x8004:
        return (GLenum)f;
    }
    return GL_ONE;
}

inline void gl_set_blend_func(int src, int dst) {
    if (g_gl.blend_src == src && g_gl.blend_dst == dst) return;
    const GLenum s = gl_blend_factor(src);
    const GLenum d = gl_blend_factor(dst);
    // The layers composite over what's under them, so alpha adds up like
    // premultiplied instead of being multiplied by itself
    glBlendFuncSeparate(s, d, src == GL_SRC_ALPHA ? (GLenum)GL_ONE : s, d);
    g_gl.blend_src = src;
    g_gl.blend_dst = dst;
}

inline void gl_set_depth_func(int func) {
    if (g_gl.depth_func == func) return;
    glDepthFunc(func >= GL_NEVER && func <= GL_ALWAYS ? (GLenum)func : (GLenum)GL_LEQUAL);
    g_gl.depth_func = func;
}

inline void gl_set_depth_write(bool on) {
    if (g_gl.depth_write == (on ? 1 : 0)) return;
    glDepthMask(on ? GL_TRUE : GL_FALSE);
    g_gl.depth_write = on ? 1 : 0;
}

inline void gl_set_polygon_offset(bool on, float factor, float units) {
    gl_set_cap(g_gl.offset_fill, GL_POLYGON_OFFSET_FILL, on);
    if (on && (g_gl.offset_factor != factor || g_gl.offset_units != units)) {
        glPolygonOffset(factor, units);
        g_gl.offset_factor = factor;
        g_gl.offset_units  = units;
    }
}

// EGL

EGLConfig choose_config(EGLDisplay display) {
    const EGLint want[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      24,
        EGL_NONE,
    };
    EGLint count = 0;
    if (!eglChooseConfig(display, want, nullptr, 0, &count) || count <= 0) return nullptr;
    std::vector<EGLConfig> configs((size_t)count);
    if (!eglChooseConfig(display, want, configs.data(), count, &count)) return nullptr;
    // The sizes asked for are minimums and deeper colour sorts first
    EGLConfig deeper = nullptr;
    for (EGLint i = 0; i < count; i++) {
        EGLint r = 0, gr = 0, b = 0, a = 0, depth = 0, samples = 0;
        eglGetConfigAttrib(display, configs[i], EGL_RED_SIZE, &r);
        eglGetConfigAttrib(display, configs[i], EGL_GREEN_SIZE, &gr);
        eglGetConfigAttrib(display, configs[i], EGL_BLUE_SIZE, &b);
        eglGetConfigAttrib(display, configs[i], EGL_ALPHA_SIZE, &a);
        eglGetConfigAttrib(display, configs[i], EGL_DEPTH_SIZE, &depth);
        eglGetConfigAttrib(display, configs[i], EGL_SAMPLE_BUFFERS, &samples);
        if (r != 8 || gr != 8 || b != 8 || a != 8 || samples != 0) continue;
        if (depth == 24) return configs[i];
        if (!deeper) deeper = configs[i];
    }
    return deeper;
}

void release_layer(Layer& layer) {
    if (layer.surface != EGL_NO_SURFACE) {
        // The context and its objects stay, only the binding goes
        if (eglGetCurrentSurface(EGL_DRAW) == layer.surface) {
            eglMakeCurrent(g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        eglDestroySurface(g.display, layer.surface);
        layer.surface = EGL_NO_SURFACE;
    }
    if (layer.window) {
        ANativeWindow_release(layer.window);
        layer.window = nullptr;
    }
}

bool make_current(const Layer& layer) {
    if (layer.surface == EGL_NO_SURFACE) return false;
    if (eglGetCurrentContext() == g.context && eglGetCurrentSurface(EGL_DRAW) == layer.surface) return true;
    // Logged once per run of failures, a frame is tried again every vsync
    static EGLint s_lastError = EGL_SUCCESS;
    if (!eglMakeCurrent(g.display, layer.surface, layer.surface, g.context)) {
        const EGLint err = eglGetError();
        if (err != s_lastError) LOGI("[mcle_gl] eglMakeCurrent failed: 0x%x", err);
        s_lastError = err;
        return false;
    }
    s_lastError = EGL_SUCCESS;
    // The frame thread paces itself on the display's refresh. A swap that
    // also waits for one makes a pass that's a little late twice as late.
    eglSwapInterval(g.display, 0);
    return true;
}

// Texture objects

GLuint new_texture() {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    gl_bind_tex0(tex);
    // One level and no mip filter, complete with or without a sampler
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return tex;
}

// The images and deletions queued since the last time, in order
void apply_tex_ops() {
    std::vector<TexOp> ops;
    {
        std::lock_guard<std::mutex> lk(g_tex_mu);
        ops.swap(g_tex_ops);
        g_tex_ops_waiting.store(false, std::memory_order_relaxed);
    }
    for (TexOp& op : ops) {
        auto it = g_gl_textures.find(op.id);
        if (op.remove) {
            if (it == g_gl_textures.end()) continue;
            if (g_gl.tex0 == it->second.name) g_gl.tex0 = kUnknown;
            glDeleteTextures(1, &it->second.name);
            g_gl_textures.erase(it);
            continue;
        }
        if (op.whole) {
            GLTex& tex = (it != g_gl_textures.end()) ? it->second : g_gl_textures[op.id];
            if (!tex.name) tex.name = new_texture();
            gl_bind_tex0(tex.name);
            if (op.pixels.empty()) op.pixels.assign((size_t)op.width * (size_t)op.height * 4, 0);
            if (tex.width == op.width && tex.height == op.height) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, op.width, op.height,
                                GL_RGBA, GL_UNSIGNED_BYTE, op.pixels.data());
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, op.width, op.height, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, op.pixels.data());
                tex.width  = op.width;
                tex.height = op.height;
            }
            continue;
        }
        if (it == g_gl_textures.end()) continue;
        const GLTex& tex = it->second;
        if (op.x + op.width > tex.width || op.y + op.height > tex.height) continue;
        gl_bind_tex0(tex.name);
        glTexSubImage2D(GL_TEXTURE_2D, 0, op.x, op.y, op.width, op.height,
                        GL_RGBA, GL_UNSIGNED_BYTE, op.pixels.data());
    }
    g_resolved_valid = false;
}

// The GL texture for an id, 0 when it has no image yet
GLuint texture_name(unsigned int id) {
    if (!g_resolved_valid || g_resolved_id != id) {
        auto it = g_gl_textures.find(id);
        g_resolved_name  = (it != g_gl_textures.end()) ? it->second.name : 0;
        g_resolved_id    = id;
        g_resolved_valid = true;
    }
    return g_resolved_name;
}

inline GLuint sampler_for_texture(unsigned int id) {
    const uint8_t bits = id < kTexParamSlots ? g_tex_params[id].load(std::memory_order_relaxed) : 0;
    return g_samplers[bits & 15];
}

void upload_lightmap() {
    uint8_t pixels[sizeof(g_lightmap.staged)];
    {
        std::lock_guard<std::mutex> lk(g_lightmap_mu);
        memcpy(pixels, g_lightmap.staged, sizeof(pixels));
        g_lightmap_dirty.store(false, std::memory_order_relaxed);
    }
    gl_bind_tex0(g_lightmap_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

// Vertex layouts, into the bound vertex array from the bound array buffer

void set_vertex_layout(bool compact, size_t base) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(base);
    if (compact) {
        // short3 pos*1024 @ 0, short packed colour @ 6, short2 uv*8192 @ 8,
        // short2 light @ 12
        for (GLuint i = 0; i < 4; i++) glEnableVertexAttribArray(i);
        glVertexAttribPointer(0, 3, GL_SHORT, GL_FALSE, 16, p + 0);
        glVertexAttribPointer(1, 1, GL_SHORT, GL_FALSE, 16, p + 6);
        glVertexAttribPointer(2, 2, GL_SHORT, GL_FALSE, 16, p + 8);
        glVertexAttribPointer(3, 2, GL_SHORT, GL_FALSE, 16, p + 12);
    } else {
        // float3 pos @ 0, float2 uv @ 12, byte4 colour @ 20, signed byte4
        // normal @ 24 (Tesselator::normal), short2 tex2 @ 28
        for (GLuint i = 0; i < 5; i++) glEnableVertexAttribArray(i);
        glVertexAttribPointer(0, 3, GL_FLOAT,         GL_FALSE, 32, p + 0);
        glVertexAttribPointer(1, 2, GL_FLOAT,         GL_FALSE, 32, p + 12);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE,  32, p + 20);
        glVertexAttribPointer(3, 4, GL_BYTE,          GL_TRUE,  32, p + 24);
        glVertexAttribPointer(4, 2, GL_SHORT,         GL_FALSE, 32, p + 28);
    }
}

template <typename Index>
std::vector<Index> quad_indices(int quads) {
    std::vector<Index> idx((size_t)quads * 6);
    for (int q = 0; q < quads; q++) {
        const Index base = (Index)(q * 4);
        idx[(size_t)q * 6 + 0] = base + 0;
        idx[(size_t)q * 6 + 1] = base + 1;
        idx[(size_t)q * 6 + 2] = base + 2;
        idx[(size_t)q * 6 + 3] = base + 0;
        idx[(size_t)q * 6 + 4] = base + 2;
        idx[(size_t)q * 6 + 5] = base + 3;
    }
    return idx;
}

// The quad indices for the bound vertex array
void bind_quad_indices(int quads) {
    if (quads <= kQuadIbo16Quads) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_quad_ibo16);
        return;
    }
    if (!g_quad_ibo32) glGenBuffers(1, &g_quad_ibo32);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_quad_ibo32);
    if (quads > g_quad_ibo32_quads) {
        // Grown in place, the vertex arrays made earlier keep the same buffer
        const int n = std::max(quads, 65536);
        const std::vector<uint32_t> idx = quad_indices<uint32_t>(n);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(idx.size() * sizeof(uint32_t)),
                     idx.data(), GL_STATIC_DRAW);
        g_quad_ibo32_quads = n;
    }
}

// A compiled list never changes, so its vertices go into one static buffer
// with a vertex array for each draw
void upload_list(DisplayList& dl) {
    dl.uploaded = true;
    if (dl.blob.empty()) return;
    glGenBuffers(1, &dl.vbo);
    gl_bind_array_buffer(dl.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)dl.blob.size(), dl.blob.data(), GL_STATIC_DRAW);
    for (DrawCmd& cmd : dl.draws) {
        if (cmd.count <= 0 || !drawable_format(cmd.fmt)) continue;
        glGenVertexArrays(1, &cmd.vao);
        gl_bind_vao(cmd.vao);
        set_vertex_layout(cmd.fmt == 4, cmd.offset);
        if (cmd.prim == kGL_QUADS && cmd.count >= 4) bind_quad_indices(cmd.count / 4);
    }
    std::vector<uint8_t>().swap(dl.blob);
}

// The stream buffer at g_stream_cap bytes, under both of its vertex arrays.
// The one before it, if any, lives on until its draws are done.
void make_stream_buffer() {
    if (g_stream_vbo) {
        if (g_stream_map) {
            gl_bind_array_buffer(g_stream_vbo);
            glUnmapBuffer(GL_ARRAY_BUFFER);
            g_stream_map = nullptr;
        }
        glDeleteBuffers(1, &g_stream_vbo);
        g_gl.array_buffer = kUnknown;
    }
    glGenBuffers(1, &g_stream_vbo);
    gl_bind_array_buffer(g_stream_vbo);
    if (g_buffer_storage) {
        const GLbitfield flags = GL_MAP_WRITE_BIT | kMapPersistent | kMapCoherent;
        g_buffer_storage(GL_ARRAY_BUFFER, (GLsizeiptr)g_stream_cap, nullptr, flags);
        g_stream_map = static_cast<uint8_t*>(glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)g_stream_cap, flags));
        if (!g_stream_map) {
            // Its storage can't be changed any more, so a new buffer
            g_buffer_storage = nullptr;
            glDeleteBuffers(1, &g_stream_vbo);
            g_gl.array_buffer = kUnknown;
            glGenBuffers(1, &g_stream_vbo);
            gl_bind_array_buffer(g_stream_vbo);
        }
    }
    if (!g_stream_map) glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)g_stream_cap, nullptr, GL_STREAM_DRAW);
    gl_bind_vao(g_stream_vao[0]);
    set_vertex_layout(false, 0);
    gl_bind_vao(g_stream_vao[1]);
    set_vertex_layout(true, 0);
    g_stream_cursor = 0;
}

// Copies vertices to the end of the stream buffer, each quad as two
// triangles. Returns the index of the first one, or -1 with nothing to draw.
GLint stream_vertices(const void* data, int count, int stride, bool quads, GLsizei* out_count) {
    const int verts = quads ? (count / 4) * 6 : count;
    if (verts <= 0) return -1;
    const size_t bytes = (size_t)verts * (size_t)stride;
    size_t at = (g_stream_cursor + (size_t)stride - 1) / (size_t)stride * (size_t)stride;
    void* dst = nullptr;
    if (g_stream_map) {
        if (at + bytes > g_stream_cap) {
            // Back to the start, where the draws are frames old. Not over
            // this frame's own though.
            if (g_stream_frame_bytes + bytes > g_stream_cap) {
                while (g_stream_cap < g_stream_frame_bytes + bytes) g_stream_cap *= 2;
                make_stream_buffer();
            }
            at = 0;
        }
        dst = g_stream_map + at;
    } else {
        gl_bind_array_buffer(g_stream_vbo);
        if (at + bytes > g_stream_cap) {
            // Full: a new store, so nothing waits on the draws still reading
            // the old one
            while (g_stream_cap < bytes) g_stream_cap *= 2;
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)g_stream_cap, nullptr, GL_STREAM_DRAW);
            at = 0;
        }
        dst = glMapBufferRange(GL_ARRAY_BUFFER, (GLintptr)at, (GLsizeiptr)bytes,
                               GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    }
    if (!dst) return -1;
    g_stream_frame_bytes += bytes;
    if (quads) {
        const uint8_t* src = static_cast<const uint8_t*>(data);
        uint8_t*       out = static_cast<uint8_t*>(dst);
        const size_t   v   = (size_t)stride;
        for (int q = 0; q < count / 4; q++, src += 4 * v, out += 6 * v) {
            memcpy(out,         src,         3 * v);
            memcpy(out + 3 * v, src,         v);
            memcpy(out + 4 * v, src + 2 * v, 2 * v);
        }
    } else {
        memcpy(dst, data, bytes);
    }
    if (!g_stream_map) glUnmapBuffer(GL_ARRAY_BUFFER);
    g_stream_cursor = at + bytes;
    *out_count = verts;
    return (GLint)(at / (size_t)stride);
}

inline GLenum gl_primitive(int prim) {
    switch (prim) {
        case 1:  return GL_LINES;
        case 3:  return GL_LINE_STRIP;
        case 5:  return GL_TRIANGLE_STRIP;
        case 6:  return GL_TRIANGLE_FAN;
        default: return GL_TRIANGLES;
    }
}

// Uniforms

inline void send_mat4(GLint loc, float* sent, const float* v) {
    if (memcmp(sent, v, 16 * sizeof(float)) == 0) return;
    memcpy(sent, v, 16 * sizeof(float));
    glUniformMatrix4fv(loc, 1, GL_FALSE, v);
}

inline void send_vec4(GLint loc, float* sent, const float* v, int n) {
    if (memcmp(sent, v, (size_t)n * 4 * sizeof(float)) == 0) return;
    memcpy(sent, v, (size_t)n * 4 * sizeof(float));
    glUniform4fv(loc, n, v);
}

void send_world_uniforms(const ThreadState& s, WorldProgram& p, int fmt) {
    const bool   compact = (fmt == 4);
    const float* mv      = s.modelview.back().m;

    float mvp[16];
    mat_mul(mvp, s.projection.back().m, mv);
    // The projections put clip z in 0..w. GL clips to -w..w and halves it
    // back into the same depth.
    for (int c = 0; c < 4; c++) mvp[c * 4 + 2] = 2.0f * mvp[c * 4 + 2] - mvp[c * 4 + 3];
    send_mat4(p.loc_mvp, p.sent.mvp, mvp);
    send_mat4(p.loc_tex_matrix, p.sent.tex_matrix, s.texture.back().m);
    send_vec4(p.loc_color, p.sent.color, s.color, 1);

    // Row 2 of the modelview, for the eye depth the fog works on
    const float row2[4] = {mv[2], mv[6], mv[10], mv[14]};
    send_vec4(p.loc_mv_row2, p.sent.mv_row2, row2, 1);

    const float fog[8] = {
        s.fog_color[0], s.fog_color[1], s.fog_color[2], s.fog ? 1.0f : 0.0f,
        s.fog_start, s.fog_end, s.fog_mode ? 1.0f : 0.0f, s.fog_density,
    };
    send_vec4(p.loc_fog, p.sent.fog, fog, 2);

    // StateSetAlphaTestEnable: the ref with the test on, 0 with it off.
    // The chunks always cut out.
    const float ref = (compact || s.alpha_test) ? s.alpha_ref : 0.0f;
    if (p.sent.alpha_ref != ref) {
        glUniform1f(p.loc_alpha_ref, ref);
        p.sent.alpha_ref = ref;
    }
    if (compact) {
        // A chunk's translation only moves the matrix's last column and the
        // fog's depth. Those two go by themselves, the matrix stays as sent.
        const float* t = s.chunk_offset;
        float chunk[8] = {};
        for (int r = 0; r < 4; r++) {
            chunk[r] = mvp[12 + r] + mvp[r] * t[0] + mvp[4 + r] * t[1] + mvp[8 + r] * t[2];
        }
        chunk[4] = row2[3] + row2[0] * t[0] + row2[1] * t[1] + row2[2] * t[2];
        send_vec4(p.loc_chunk, p.sent.chunk, chunk, 2);
        return;
    }

    // GL_LIGHTING only reaches the PF3_TF2_CB4_NB4_XW1 format, as in the 4J
    // renderer's DrawVertexSetup. Unlit, the lights are left zero so a
    // change of modelview alone sends nothing.
    const bool lit = s.lighting && fmt == 1;
    float light[36] = {};
    if (lit) {
        for (int i = 0; i < 3; i++) {
            light[0 + i]  = s.light_dir[0][i];
            light[4 + i]  = s.light_dir[1][i];
            light[8 + i]  = s.light_enabled[0] ? s.light_col[0][i] : 0.0f;
            light[12 + i] = s.light_enabled[1] ? s.light_col[1][i] : 0.0f;
            light[16 + i] = s.light_ambient[i];
        }
        light[11] = s.light_enabled[0] ? 1.0f : 0.0f;
        light[15] = s.light_enabled[1] ? 1.0f : 0.0f;
        light[19] = 1.0f;
        for (int c = 0; c < 3; c++) {
            for (int r = 0; r < 3; r++) light[24 + c * 4 + r] = mv[c * 4 + r];
        }
    }
    light[20] = lit ? 1.0f : 0.0f;
    light[21] = s.light_layer ? 1.0f : 0.0f;
    light[22] = s.vertex_uv[0];
    light[23] = s.vertex_uv[1];
    send_vec4(p.loc_light, p.sent.light, light, 9);

    const bool  texgen = (fmt == 2);
    const float flag   = texgen ? 1.0f : 0.0f;
    if (p.sent.texgen != flag) {
        glUniform1f(p.loc_texgen, flag);
        p.sent.texgen = flag;
    }
    if (texgen) {
        float tg[48];
        memcpy(tg, mv, 16 * sizeof(float));
        memcpy(tg + 16, s.texgen_eye, sizeof(s.texgen_eye));
        memcpy(tg + 32, s.texgen_obj, sizeof(s.texgen_obj));
        glUniformMatrix4fv(p.loc_texgen_mats, 3, GL_FALSE, tg);
    }
}

// One batch of vertices with the calling thread's state: straight from
// data, or a compiled list's draw from its own buffer
void draw_batch(ThreadState& s, int prim, int count, int fmt, const void* data, const DrawCmd* cmd) {
    g_draw_count.fetch_add(1, std::memory_order_relaxed);

    if (!s.in_frame || !g_world_ready)  return;
    if (count <= 0)                     return;
    if (!drawable_format(fmt))          return;
    if (cmd ? cmd->vao == 0 : !data)    return;

    if (g_tex_ops_waiting.load(std::memory_order_relaxed)) apply_tex_ops();
    if (g_lightmap_dirty.load(std::memory_order_relaxed))  upload_lightmap();

    const bool    compact = (fmt == 4);
    WorldProgram& prog    = compact ? g_prog_compact : g_prog_std;
    gl_use_program(prog.id);

    gl_set_blend(s.blend);
    if (s.blend) gl_set_blend_func(s.blend_src, s.blend_dst);

    // With the test off nothing is written either, the HUD and overlays
    gl_set_depth_test(s.depth_test);
    if (s.depth_test) {
        gl_set_depth_func(s.depth_func);
        gl_set_depth_write(s.depth_write);
    }
    gl_set_polygon_offset(s.polygon_offset_fill, s.polygon_offset_factor, s.polygon_offset_units);
    // Chunks are wound counter clockwise, the other formats draw both sides
    gl_set_cull(compact && s.cull_face);

    send_world_uniforms(s, prog, fmt);

    // With GL_TEXTURE_2D off (the sky dome, stars, the sunrise fan) or
    // nothing uploaded for the bound id, white, so the fragment is the
    // vertex colour alone
    GLuint tex     = g_white_tex;
    GLuint sampler = g_samplers[0];
    if (s.texture_2d) {
        const unsigned int bound = g_bound_tex_id.load(std::memory_order_relaxed);
        const GLuint       name  = bound ? texture_name(bound) : 0;
        if (name) {
            tex     = name;
            sampler = sampler_for_texture(bound);
        }
    }
    gl_bind_tex0(tex);
    gl_bind_sampler0(sampler);
    // The chunks always have the lightmap, the other formats only inside
    // the light layer
    gl_bind_tex1((compact || s.light_layer) ? g_lightmap_tex : g_white_tex);

    if (cmd) {
        gl_bind_vao(cmd->vao);
        if (prim == kGL_QUADS) {
            const int quads = count / 4;
            if (quads <= 0) return;
            glDrawElements(GL_TRIANGLES, quads * 6,
                           quads <= kQuadIbo16Quads ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, nullptr);
        } else {
            glDrawArrays(gl_primitive(prim), 0, count);
        }
        return;
    }

    GLsizei     verts = 0;
    const GLint first = stream_vertices(data, count, vertex_stride(fmt), prim == kGL_QUADS, &verts);
    if (first < 0) return;
    gl_bind_vao(g_stream_vao[compact ? 1 : 0]);
    glDrawArrays(gl_primitive(prim), first, verts);
}

// Replays a list. Each draw gets its recorded translation and texture, so
// a chunk lands at its world position with terrain.png whatever is current.
void replay_list(ThreadState& s, DisplayList& dl) {
    if (s.in_frame && g_world_ready && !dl.uploaded) upload_list(dl);
    for (const DrawCmd& cmd : dl.draws) {
        Mat4 savedMv;
        // A chunk's is left out of the modelview, so the matrices stay as
        // they were sent from one chunk to the next
        const bool chunk = cmd.hasTranslate && cmd.fmt == 4;
        if (chunk) {
            memcpy(s.chunk_offset, cmd.translate, sizeof(s.chunk_offset));
        } else if (cmd.hasTranslate) {
            savedMv = s.modelview.back();
            mat_translate(s.modelview.back().m, cmd.translate[0], cmd.translate[1], cmd.translate[2]);
        }
        // Only a bind compiled into the list changes the texture, and it
        // stays bound after, as glCallList leaves it. Model parts are
        // compiled in their model's constructor, so the texture bound back
        // then means nothing for them.
        if (cmd.texId != 0) g_bound_tex_id.store(cmd.texId, std::memory_order_relaxed);
        draw_batch(s, cmd.prim, cmd.count, cmd.fmt, nullptr, &cmd);
        if (chunk) {
            s.chunk_offset[0] = s.chunk_offset[1] = s.chunk_offset[2] = 0;
        } else if (cmd.hasTranslate) {
            s.modelview.back() = savedMv;
        }
    }
}

// GL objects every frame draws with

bool build_world_program(WorldProgram& p, const char* label, const char* vert_src) {
    p.id = mcle_gl::build_program(label, vert_src, mcle_gl::kWorldFragSrc);
    if (!p.id) return false;
    p.loc_mvp         = glGetUniformLocation(p.id, "u_mvp");
    p.loc_tex_matrix  = glGetUniformLocation(p.id, "u_tex_matrix");
    p.loc_color       = glGetUniformLocation(p.id, "u_color");
    p.loc_mv_row2     = glGetUniformLocation(p.id, "u_mv_row2");
    p.loc_fog         = glGetUniformLocation(p.id, "u_fog");
    p.loc_alpha_ref   = glGetUniformLocation(p.id, "u_alpha_ref");
    p.loc_light       = glGetUniformLocation(p.id, "u_light");
    p.loc_texgen      = glGetUniformLocation(p.id, "u_texgen");
    p.loc_texgen_mats = glGetUniformLocation(p.id, "u_texgen_mats");
    p.loc_chunk       = glGetUniformLocation(p.id, "u_chunk");
    // All ones is a NaN in every float, equal to nothing a draw can ask for
    memset(&p.sent, 0xff, sizeof(p.sent));
    gl_use_program(p.id);
    glUniform1i(glGetUniformLocation(p.id, "u_tex"), 0);
    glUniform1i(glGetUniformLocation(p.id, "u_lightmap"), 1);
    return true;
}

// Once, the first time the context is current
void init_gl_objects() {
    if (g_objects_tried) return;
    g_objects_tried = true;

    LOGI("[mcle_gl] %s, %s", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));

    if (!build_world_program(g_prog_std, "world", mcle_gl::kWorldVertSrc) ||
        !build_world_program(g_prog_compact, "world compact", mcle_gl::kWorldCompactVertSrc)) {
        return;
    }

    glDisable(GL_DITHER);
    glActiveTexture(GL_TEXTURE0);

    glGenSamplers(16, g_samplers);
    for (int bits = 0; bits < 16; bits++) {
        const GLuint sm = g_samplers[bits];
        glSamplerParameteri(sm, GL_TEXTURE_WRAP_S, (bits & 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glSamplerParameteri(sm, GL_TEXTURE_WRAP_T, (bits & 2) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glSamplerParameteri(sm, GL_TEXTURE_MIN_FILTER, (bits & 4) ? GL_LINEAR : GL_NEAREST);
        glSamplerParameteri(sm, GL_TEXTURE_MAG_FILTER, (bits & 8) ? GL_LINEAR : GL_NEAREST);
    }
    glGenSamplers(1, &g_lightmap_sampler);
    glSamplerParameteri(g_lightmap_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(g_lightmap_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(g_lightmap_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(g_lightmap_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindSampler(1, g_lightmap_sampler);

    const uint8_t white[4] = {255, 255, 255, 255};
    g_white_tex = new_texture();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    // Filled from the staged entries before the first draw
    g_lightmap_tex = new_texture();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    g_lightmap_dirty.store(true, std::memory_order_relaxed);

    gl_bind_vao(0);
    const std::vector<uint16_t> idx = quad_indices<uint16_t>(kQuadIbo16Quads);
    glGenBuffers(1, &g_quad_ibo16);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_quad_ibo16);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(idx.size() * sizeof(uint16_t)), idx.data(), GL_STATIC_DRAW);

    const char* extensions = (const char*)glGetString(GL_EXTENSIONS);
    if (extensions && strstr(extensions, "GL_EXT_buffer_storage")) {
        g_buffer_storage = (BufferStorageFn)eglGetProcAddress("glBufferStorageEXT");
    }
    glGenVertexArrays(2, g_stream_vao);
    make_stream_buffer();

    g_world_ready = true;
    LOGI("[mcle_gl] world programs and default textures built");
}

void delete_dead_objects() {
    std::vector<GLuint> buffers, vaos;
    {
        std::lock_guard<std::mutex> lk(g_dead_mu);
        buffers.swap(g_dead_buffers);
        vaos.swap(g_dead_vaos);
    }
    if (!vaos.empty())    glDeleteVertexArrays((GLsizei)vaos.size(), vaos.data());
    if (!buffers.empty()) glDeleteBuffers((GLsizei)buffers.size(), buffers.data());
}

// Gamma

bool gamma_program_ready() {
    if (g_prog_gamma) return true;
    // A program that didn't build isn't tried again every frame
    static bool s_failed = false;
    if (s_failed) return false;
    g_prog_gamma = mcle_gl::build_program("gamma", mcle_gl::kScreenVertSrc, mcle_gl::kGammaFragSrc);
    if (!g_prog_gamma) {
        s_failed = true;
        return false;
    }
    g_gamma_params_loc = glGetUniformLocation(g_prog_gamma, "u_params");
    gl_use_program(g_prog_gamma);
    glUniform1i(glGetUniformLocation(g_prog_gamma, "u_scene"), 0);
    return true;
}

// The colour and depth a frame with gamma is drawn into, at the layer's size
bool scene_target_ready(int width, int height) {
    static bool s_failed = false;
    if (s_failed) return false;
    if (g_scene_fbo && g_scene_w == width && g_scene_h == height) return true;
    if (!g_scene_fbo) {
        glGenFramebuffers(1, &g_scene_fbo);
        glGenRenderbuffers(1, &g_scene_depth);
        g_scene_tex = new_texture();
    }
    gl_bind_tex0(g_scene_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindRenderbuffer(GL_RENDERBUFFER, g_scene_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, g_scene_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_scene_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_scene_depth);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGI("[mcle_gl] scene target %dx%d incomplete: 0x%x", width, height, status);
        s_failed = true;
        return false;
    }
    g_scene_w = width;
    g_scene_h = height;
    return true;
}

// Whether this frame goes through the gamma pass
bool gamma_frame_begin(int width, int height) {
    const float gamma = g_gamma.load(std::memory_order_relaxed);
    // PostProcesser skips the pass when gamma is 1
    if (!(gamma < 0.99f || gamma > 1.01f) || !gamma_program_ready()) return false;
    return scene_target_ready(width, height);
}

// The gamma pass from the scene target to the window
void gamma_frame_end(bool premultiplied) {
    // The scene's depth isn't needed again and nothing of the window's is
    const GLenum depth = GL_DEPTH_ATTACHMENT;
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &depth);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    const GLenum window[2] = {GL_COLOR, GL_DEPTH};
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, window);

    gl_bind_tex0(g_scene_tex);
    gl_bind_sampler0(g_samplers[kSamplerNearestClamp]);
    gl_use_program(g_prog_gamma);
    glUniform2f(g_gamma_params_loc, g_gamma.load(std::memory_order_relaxed), premultiplied ? 1.0f : 0.0f);
    mcle_gl::draw_screen_triangle(g_prog_gamma);
}

// The save thumbnail. CConsoleMinecraftApp::CaptureSaveThumbnail,
// RenderManager.CaptureThumbnail on Windows64: after the world is drawn, the
// square in the middle of the frame goes down to 64x64.

const int kThumbSize = 64;

// 0 nothing, 1 wanted on the next world frame
std::atomic<int>           g_thumbState{0};
bool                       g_thumbArmed = false;
std::mutex                 g_thumbMutex;
std::vector<unsigned char> g_thumbRgba;

// At the end of the world frame, with the finished frame in the window's
// back buffer
void capture_thumbnail() {
    if (!g_thumbArmed) return;
    g_thumbArmed = false;

    const int w = g.world.width, h = g.world.height;
    const int side = std::min(w, h);
    if (!g_prog_thumb && g_world_ready) {
        g_prog_thumb = mcle_gl::build_program("thumbnail", mcle_gl::kThumbVertSrc, mcle_gl::kThumbFragSrc);
        if (g_prog_thumb) {
            gl_use_program(g_prog_thumb);
            glUniform1i(glGetUniformLocation(g_prog_thumb, "u_src"), 0);
        }
    }
    if (!g_prog_thumb || side <= 0) {
        g_thumbState.store(0);
        return;
    }

    // The window can't be sampled, so the square is copied out of it first
    const GLuint square = new_texture();
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (w - side) / 2, (h - side) / 2, side, side, 0);
    const GLuint target = new_texture();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kThumbSize, kThumbSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glViewport(0, 0, kThumbSize, kThumbSize);
        gl_bind_tex0(square);
        gl_bind_sampler0(g_samplers[kSamplerLinearClamp]);
        mcle_gl::draw_screen_triangle(g_prog_thumb);
        std::vector<unsigned char> px((size_t)kThumbSize * kThumbSize * 4);
        glReadPixels(0, 0, kThumbSize, kThumbSize, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        std::lock_guard<std::mutex> lock(g_thumbMutex);
        g_thumbRgba = std::move(px);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glDeleteFramebuffers(1, &fbo);
    const GLuint both[2] = {square, target};
    glDeleteTextures(2, both);
    g_gl.tex0 = kUnknown;
    g_thumbState.store(0);
}

// Frames

uint64_t g_frame_start_ns = 0;

// Time spent per frame in each part, summed for the frame_ms line:
// 0 presenting the world, 1 the world, 2 the movie and custom draws
double g_phase_ms[3];

// What RenderManager.StartFrame resets that every frame here starts with
// too: texturing, white, and no light layer. The bound texture stays, the
// loading threads bind and upload through the same id.
void start_frame_common(ThreadState& s) {
    s.texture_2d = true;
    s.color[0] = s.color[1] = s.color[2] = s.color[3] = 1.0f;
    s.light_layer = false;
}

// Makes the layer the one being drawn and clears it. Each frame starts with
// the depth test and write on, the alpha test on (chunks need the cutout
// for foliage), and blending and fog off.
bool open_frame(Layer& layer, float r, float gn, float b, float a) {
    if (!make_current(layer)) return false;
    init_gl_objects();
    delete_dead_objects();
    g_gl = GLState();
    glActiveTexture(GL_TEXTURE0);
    if (g_tex_ops_waiting.load(std::memory_order_relaxed)) apply_tex_ops();

    // The mapped stream buffer wraps back onto vertices whose draws are a
    // few frames old, so one frame can only take a small part of it
    if (g_stream_map && g_stream_frame_bytes * 8 > g_stream_cap) {
        while (g_stream_frame_bytes * 8 > g_stream_cap) g_stream_cap *= 2;
        make_stream_buffer();
    }
    g_stream_frame_bytes = 0;

    g.gammaThisFrame = g_world_ready && gamma_frame_begin(layer.width, layer.height);
    glBindFramebuffer(GL_FRAMEBUFFER, g.gammaThisFrame ? g_scene_fbo : 0);
    glViewport(0, 0, layer.width, layer.height);
    gl_set_depth_write(true);
    glClearColor(r, gn, b, a);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    ThreadState& s = t_state;
    s.in_frame    = true;
    s.depth_test  = true;
    s.cull_face   = true;
    s.depth_write = true;
    s.alpha_test  = true;
    s.alpha_ref   = 0.1f;
    s.blend       = false;
    s.blend_src   = GL_SRC_ALPHA;
    s.blend_dst   = GL_ONE_MINUS_SRC_ALPHA;
    s.depth_func  = GL_LEQUAL;
    s.fog         = false;
    start_frame_common(s);

    g.frameLayer = &layer;
    g.inFrame    = true;
    return true;
}

void close_frame(ThreadState& s) {
    s.in_frame       = false;
    g.inFrame        = false;
    g.frameLayer     = nullptr;
    g.gammaThisFrame = false;
}

void present(const Layer& layer) {
    static EGLint s_lastError = EGL_SUCCESS;
    if (eglSwapBuffers(g.display, layer.surface)) {
        s_lastError = EGL_SUCCESS;
        return;
    }
    // Most likely the window went away under the surface, and the next
    // attach brings one back. Logged once per run of failures.
    const EGLint err = eglGetError();
    if (err != s_lastError) LOGI("[mcle_gl] eglSwapBuffers failed: 0x%x", err);
    s_lastError = err;
}

// A window for a layer, a new size for the one it has, or with no window
// the layer's surface released
void attach(Layer& layer, void* native_window, int width, int height) {
    layer.width  = width;
    layer.height = height;
    if (&layer == &g.ui) g_ui_key_shown = false;

    ANativeWindow* window = static_cast<ANativeWindow*>(native_window);
    if (!window) {
        release_layer(layer);
        return;
    }
    const bool sized = width > 0 && height > 0;
    if (window != layer.window || layer.surface == EGL_NO_SURFACE) {
        // The window was recreated: only the surface is, the context and
        // everything made in it carry over
        release_layer(layer);
        ANativeWindow_acquire(window);
        layer.window = window;
        ANativeWindow_setBuffersGeometry(window, sized ? width : 0, sized ? height : 0, g.visual);
        layer.surface = eglCreateWindowSurface(g.display, g.config, window, nullptr);
        if (layer.surface == EGL_NO_SURFACE) {
            LOGI("[mcle_gl] eglCreateWindowSurface failed: 0x%x", eglGetError());
            ANativeWindow_release(window);
            layer.window = nullptr;
            return;
        }
    } else {
        ANativeWindow_setBuffersGeometry(window, sized ? width : 0, sized ? height : 0, g.visual);
    }
    if (!sized) {
        EGLint w = 0, h = 0;
        eglQuerySurface(g.display, layer.surface, EGL_WIDTH, &w);
        eglQuerySurface(g.display, layer.surface, EGL_HEIGHT, &h);
        layer.width  = w;
        layer.height = h;
    }
    if (make_current(layer)) init_gl_objects();
}

} // namespace

namespace mcle_gl {

void draw_screen_triangle(GLuint program) {
    if (!t_state.in_frame || !program) return;
    gl_set_blend(false);
    gl_set_depth_test(false);
    gl_set_cull(false);
    gl_set_polygon_offset(false, 0.0f, 0.0f);
    gl_use_program(program);
    gl_bind_vao(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void resize_layer(int width, int height) {
    g.world.width  = width;
    g.world.height = height;
    if (g.world.window && width > 0 && height > 0) {
        ANativeWindow_setBuffersGeometry(g.world.window, width, height, g.visual);
    }
}

} // namespace mcle_gl

// glEnable / glDisable and the state calls next to them

extern "C" void mcle_glbridge_set_depth_test(int enabled) {
    t_state.depth_test = (enabled != 0);
}

extern "C" void mcle_glbridge_set_cull_face(int enabled) {
    t_state.cull_face = (enabled != 0);
}

extern "C" void mcle_glbridge_set_texture_2d(int enabled) {
    t_state.texture_2d = (enabled != 0);
}

// glDepthMask. renderSky turns it off for the sky dome, so the sun and moon
// drawn after are only tested against the terrain.
extern "C" void mcle_glbridge_set_depth_write(int enabled) {
    t_state.depth_write = (enabled != 0);
}

extern "C" void mcle_glbridge_set_depth_func(int func) {
    t_state.depth_func = func;
}

extern "C" void mcle_glbridge_set_alpha_test(int enabled) {
    t_state.alpha_test = (enabled != 0);
}

// glAlphaFunc, Renderer::StateSetAlphaFunc keeps only the ref
extern "C" void mcle_glbridge_set_alpha_func(int /*func*/, float ref) {
    t_state.alpha_ref = ref;
}

extern "C" void mcle_glbridge_set_blend_enabled(int enabled) {
    t_state.blend = (enabled != 0);
}

// glBlendFunc's factors, as GL enums
extern "C" void mcle_glbridge_set_blend_func(int src, int dst) {
    ThreadState& s = t_state;
    s.blend_src = src;
    s.blend_dst = dst;
}

extern "C" void mcle_glbridge_set_polygon_offset_fill(int enabled) {
    t_state.polygon_offset_fill = (enabled != 0);
}

extern "C" void mcle_glbridge_set_polygon_offset(float factor, float units) {
    ThreadState& s = t_state;
    s.polygon_offset_factor = factor;
    s.polygon_offset_units  = units;
}

// GameRenderer::setupFog's glFog calls
extern "C" void mcle_glbridge_set_fog_enabled(int enabled) {
    t_state.fog = (enabled != 0);
}

extern "C" void mcle_glbridge_set_fog_color(float r, float gr, float b, float /*a*/) {
    ThreadState& s = t_state;
    s.fog_color[0] = r;
    s.fog_color[1] = gr;
    s.fog_color[2] = b;
}

extern "C" void mcle_glbridge_set_fog_start(float v) { t_state.fog_start = v; }
extern "C" void mcle_glbridge_set_fog_end(float v)   { t_state.fog_end   = v; }
extern "C" void mcle_glbridge_set_fog_mode(int exp_mode) { t_state.fog_mode = exp_mode ? 1 : 0; }
extern "C" void mcle_glbridge_set_fog_density(float v)   { t_state.fog_density = v; }

// glEnable/glDisable(GL_LIGHTING), (GL_LIGHT0 / GL_LIGHT1)
extern "C" void mcle_glbridge_set_lighting(int enabled) { t_state.lighting = (enabled != 0); }

extern "C" void mcle_glbridge_set_light_enabled(int light, int enabled) {
    if (light >= 0 && light < 2) t_state.light_enabled[light] = (enabled != 0);
}

// glLight GL_DIFFUSE and glLightModel GL_LIGHT_MODEL_AMBIENT
extern "C" void mcle_glbridge_set_light_colour(int light, float r, float gr, float b) {
    if (light < 0 || light >= 2) return;
    ThreadState& s = t_state;
    s.light_col[light][0] = r;
    s.light_col[light][1] = gr;
    s.light_col[light][2] = b;
}

extern "C" void mcle_glbridge_set_light_ambient(float r, float gr, float b) {
    ThreadState& s = t_state;
    s.light_ambient[0] = r;
    s.light_ambient[1] = gr;
    s.light_ambient[2] = b;
}

// glLight GL_POSITION: the direction goes through the modelview of the time
// and is normalised, as StateSetLightDirection does
extern "C" void mcle_glbridge_set_light_direction(int light, float x, float y, float z) {
    if (light < 0 || light >= 2) return;
    ThreadState& s = t_state;
    const float* m = s.modelview.back().m;
    const float d[3] = {
        m[0] * x + m[4] * y + m[8] * z,
        m[1] * x + m[5] * y + m[9] * z,
        m[2] * x + m[6] * y + m[10] * z,
    };
    const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; i++) s.light_dir[light][i] = len > 0.0f ? d[i] / len : 0.0f;
}

// GameRenderer::turnOnLightLayer / turnOffLightLayer
extern "C" void mcle_glbridge_set_light_layer(int on) { t_state.light_layer = (on != 0); }

// glMultiTexCoord2f(GL_TEXTURE1, u, v), the light for vertices without a tex2
extern "C" void mcle_glbridge_set_vertex_uv(float u, float v) {
    if (u > 255.0f) u = 255.0f;
    if (v > 255.0f) v = 255.0f;
    ThreadState& s = t_state;
    s.vertex_uv[0] = u / 256.0f - 1.0f;
    s.vertex_uv[1] = v / 256.0f - 1.0f;
}

// RenderManager.StartFrame's state, Windows64's
// UIController::setupCustomDrawGameState starts each custom draw with it
extern "C" void mcle_glbridge_start_frame(void) {
    ThreadState& s = t_state;
    start_frame_common(s);
    s.depth_write = true;
    s.blend       = true;
    s.blend_src   = GL_SRC_ALPHA;
    s.blend_dst   = GL_ONE_MINUS_SRC_ALPHA;
    s.depth_func  = GL_LEQUAL;
    s.cull_face   = true;
    s.depth_test  = false;
    s.alpha_test  = true;
    s.alpha_ref   = 0.1f;
}

// The lightmap. The game loop works out each of the 256 entries the way
// GameRenderer::updateLightTexture does, then uploads the lot.

extern "C" void mcle_lightmap_set_entry(int idx, float r, float gr, float b) {
    if (idx < 0 || idx >= 256) return;
    int ir = (int)(r * 255.0f); if (ir < 0) ir = 0; if (ir > 255) ir = 255;
    int ig = (int)(gr * 255.0f); if (ig < 0) ig = 0; if (ig > 255) ig = 255;
    int ib = (int)(b * 255.0f); if (ib < 0) ib = 0; if (ib > 255) ib = 255;
    g_lightmap.pixels[idx * 4 + 0] = (uint8_t)ir;
    g_lightmap.pixels[idx * 4 + 1] = (uint8_t)ig;
    g_lightmap.pixels[idx * 4 + 2] = (uint8_t)ib;
    g_lightmap.pixels[idx * 4 + 3] = 255;
}

// The texture takes the entries at the next draw
extern "C" void mcle_lightmap_upload(void) {
    std::lock_guard<std::mutex> lk(g_lightmap_mu);
    memcpy(g_lightmap.staged, g_lightmap.pixels, sizeof(g_lightmap.staged));
    g_lightmap_dirty.store(true, std::memory_order_relaxed);
}

// Matrix stacks

extern "C" void mcle_glbridge_get_modelview(float* out16) {
    if (!out16) return;
    memcpy(out16, t_state.modelview.back().m, 16 * sizeof(float));
}

// Frustum::calculateFrustum reads both matrices for its clip planes
extern "C" void mcle_glbridge_get_projection(float* out16) {
    if (!out16) return;
    memcpy(out16, t_state.projection.back().m, 16 * sizeof(float));
}

extern "C" void mcle_glbridge_matrix_mode(int mode) {
    ThreadState& s = t_state;
    switch (mode) {
        case kGL_PROJECTION: s.matrix_mode = kMatrixModeProjection; break;
        case kGL_TEXTURE:    s.matrix_mode = kMatrixModeTexture;    break;
        default:             s.matrix_mode = kMatrixModeModelview;  break;
    }
}

extern "C" void mcle_glbridge_load_identity(void) {
    current_stack(t_state).back() = mat_identity();
}

extern "C" void mcle_glbridge_load_matrix(const float* m16) {
    if (!m16) return;
    memcpy(current_stack(t_state).back().m, m16, 16 * sizeof(float));
}

extern "C" void mcle_glbridge_mult_matrix(const float* m16) {
    if (!m16) return;
    float* m = current_stack(t_state).back().m;
    float r[16];
    mat_mul(r, m, m16);
    memcpy(m, r, sizeof(r));
}

extern "C" void mcle_glbridge_push_matrix(void) {
    std::vector<Mat4>& stack = current_stack(t_state);
    const Mat4 top = stack.back();
    stack.push_back(top);
}

extern "C" void mcle_glbridge_pop_matrix(void) {
    std::vector<Mat4>& stack = current_stack(t_state);
    if (stack.size() > 1) stack.pop_back();
}

// The last translate made while a list is being recorded is kept, so a
// chunk's list carries its world position to replay
extern "C" void mcle_glbridge_translate(float x, float y, float z) {
    ThreadState& s = t_state;
    mat_translate(current_stack(s).back().m, x, y, z);
    if (s.recording_list != 0) {
        s.recording_translate[0] = x;
        s.recording_translate[1] = y;
        s.recording_translate[2] = z;
        s.recording_has_translate = true;
    }
}

extern "C" void mcle_glbridge_rotate(float angle, float x, float y, float z) {
    mat_rotate(current_stack(t_state).back().m, angle, x, y, z);
}

extern "C" void mcle_glbridge_scale(float x, float y, float z) {
    mat_scale(current_stack(t_state).back().m, x, y, z);
}

// RenderManager.StateSetTexGenCol. An eye plane is multiplied, as a row, by
// the inverse of the modelview at the time, its rows taken as the matrix's
// (the 4J renderer's order, not GL's). The plane goes in column col of its
// matrix and the same column of the other one is cleared.
extern "C" void mcle_glbridge_set_texgen_col(int col, float x, float y, float z, float w, bool eyeSpace) {
    if (col < 0 || col > 3) return;
    ThreadState& s = t_state;
    float v[4] = {x, y, z, w};
    if (eyeSpace) {
        float inv[16];
        if (!mat_inverse(s.modelview.back().m, inv)) return;
        float r[4];
        for (int j = 0; j < 4; j++) {
            r[j] = v[0] * inv[j] + v[1] * inv[4 + j] + v[2] * inv[8 + j] + v[3] * inv[12 + j];
        }
        memcpy(v, r, sizeof(v));
    }
    float* dst   = eyeSpace ? s.texgen_eye : s.texgen_obj;
    float* other = eyeSpace ? s.texgen_obj : s.texgen_eye;
    for (int row = 0; row < 4; row++) {
        dst[row * 4 + col]   = v[row];
        other[row * 4 + col] = 0.0f;
    }
}

// Both projections keep the depth range the game's matrices were written
// for, clip z from 0 to w. The frustum planes are read back from them, and
// z is moved into GL's range when a draw builds its MVP.
extern "C" void mcle_glbridge_metal_perspective(float fov_y_deg, float aspect, float near_z, float far_z) {
    const float fov_rad = fov_y_deg * (float)M_PI / 180.0f;
    const float t       = tanf(fov_rad / 2.0f) * near_z;
    const float r       = t * aspect;
    Mat4 P;
    P.m[0]  = near_z / r;  P.m[1]  = 0;            P.m[2]  = 0;                                    P.m[3]  = 0;
    P.m[4]  = 0;           P.m[5]  = near_z / t;   P.m[6]  = 0;                                    P.m[7]  = 0;
    P.m[8]  = 0;           P.m[9]  = 0;            P.m[10] = -far_z / (far_z - near_z);            P.m[11] = -1;
    P.m[12] = 0;           P.m[13] = 0;            P.m[14] = -(near_z * far_z) / (far_z - near_z); P.m[15] = 0;
    current_stack(t_state).back() = P;
}

// glOrtho(left, right, bottom, top, near, far), y down with top < bottom
extern "C" void mcle_glbridge_metal_ortho(float left, float right, float bottom, float top,
                                           float near_z, float far_z) {
    const float rl = right - left;
    const float tb = top   - bottom;
    const float fn = far_z - near_z;
    Mat4 P;
    P.m[0]  = 2.0f / rl;          P.m[1]  = 0;                  P.m[2]  = 0;            P.m[3]  = 0;
    P.m[4]  = 0;                  P.m[5]  = 2.0f / tb;          P.m[6]  = 0;            P.m[7]  = 0;
    P.m[8]  = 0;                  P.m[9]  = 0;                  P.m[10] = -1.0f / fn;   P.m[11] = 0;
    P.m[12] = -(right + left)/rl; P.m[13] = -(top + bottom)/tb; P.m[14] = -near_z / fn; P.m[15] = 1;
    current_stack(t_state).back() = P;
}

// GL_CURRENT_COLOR. Every draw multiplies the vertex colour by it, so
// glColor takes effect across replayed display lists too.

extern "C" void mcle_glbridge_color4f(float r, float gr, float b, float a) {
    ThreadState& s = t_state;
    s.color[0] = r;
    s.color[1] = gr;
    s.color[2] = b;
    s.color[3] = a;
}

extern "C" void mcle_glbridge_color3f(float r, float gr, float b) {
    mcle_glbridge_color4f(r, gr, b, 1.0f);
}

extern "C" void mcle_glbridge_color4ub(unsigned char r, unsigned char gr,
                                        unsigned char b, unsigned char a) {
    mcle_glbridge_color4f((float)r / 255.0f, (float)gr / 255.0f, (float)b / 255.0f, (float)a / 255.0f);
}

// Textures

namespace {

// Queues an image or a deletion. One that replaces everything an earlier
// one in the queue did takes its place, so the queue stays bounded while no
// frames are drawn. Called with g_tex_mu held.
void queue_tex_op(TexOp&& op) {
    const auto covered = [&op](const TexOp& o) {
        if (o.id != op.id) return false;
        if (op.whole || op.remove) return true;
        return !o.whole && !o.remove && o.x == op.x && o.y == op.y &&
               o.width == op.width && o.height == op.height;
    };
    g_tex_ops.erase(std::remove_if(g_tex_ops.begin(), g_tex_ops.end(), covered), g_tex_ops.end());
    g_tex_ops.push_back(std::move(op));
    g_tex_ops_waiting.store(true, std::memory_order_relaxed);
}

TexOp make_tex_op(unsigned int id, int x, int y, int width, int height, const void* rgba_pixels) {
    TexOp op;
    op.id     = id;
    op.x      = x;
    op.y      = y;
    op.width  = width;
    op.height = height;
    if (rgba_pixels) {
        const uint8_t* p = static_cast<const uint8_t*>(rgba_pixels);
        op.pixels.assign(p, p + (size_t)width * (size_t)height * 4);
    }
    return op;
}

} // namespace

extern "C" unsigned int mcle_glbridge_gen_texture(void) {
    return g_next_tex_id.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void mcle_glbridge_gen_textures_n(int n, unsigned int* out) {
    if (!out || n <= 0) return;
    for (int i = 0; i < n; i++) {
        out[i] = g_next_tex_id.fetch_add(1, std::memory_order_relaxed);
    }
}

extern "C" void mcle_glbridge_delete_texture(unsigned int tex_id) {
    {
        std::lock_guard<std::mutex> lk(g_tex_mu);
        g_tex_sizes.erase(tex_id);
        TexOp op;
        op.id     = tex_id;
        op.remove = true;
        queue_tex_op(std::move(op));
    }
    if (tex_id < kTexParamSlots) g_tex_params[tex_id].store(0, std::memory_order_relaxed);
    unsigned int bound = tex_id;
    g_bound_tex_id.compare_exchange_strong(bound, 0, std::memory_order_relaxed);
}

extern "C" void mcle_glbridge_bind_texture(unsigned int tex_id) {
    ThreadState& s = t_state;
    if (s.recording_list != 0) {
        s.recording_tex = tex_id;
        return;
    }
    g_bound_tex_id.store(tex_id, std::memory_order_relaxed);
}

extern "C" unsigned int mcle_glbridge_get_bound_texture(void) {
    return g_bound_tex_id.load(std::memory_order_relaxed);
}

// glTexParameteri, RenderManager.TextureSetParam: the wrap and filter of the
// bound texture, used whenever it's drawn with
extern "C" void mcle_glbridge_tex_parameter(int pname, int value) {
    const unsigned int tex_id = g_bound_tex_id.load(std::memory_order_relaxed);
    if (tex_id == 0 || tex_id >= kTexParamSlots) return;
    const bool clamp  = value == 0x2900 /* GL_CLAMP */ || value == GL_CLAMP_TO_EDGE;
    const bool linear = value == GL_LINEAR;
    uint8_t bit;
    bool on;
    switch (pname) {
        case GL_TEXTURE_WRAP_S:     bit = 1; on = clamp;  break;
        case GL_TEXTURE_WRAP_T:     bit = 2; on = clamp;  break;
        case GL_TEXTURE_MIN_FILTER: bit = 4; on = linear; break;
        case GL_TEXTURE_MAG_FILTER: bit = 8; on = linear; break;
        default: return;
    }
    if (on) g_tex_params[tex_id].fetch_or(bit, std::memory_order_relaxed);
    else    g_tex_params[tex_id].fetch_and((uint8_t)~bit, std::memory_order_relaxed);
}

// glTexImage2D for level 0: RGBA bytes, first row first. No pixels makes
// the image blank.
extern "C" void mcle_glbridge_tex_image_2d_rgba(unsigned int tex_id, int width, int height,
                                                  const void* rgba_pixels) {
    if (width <= 0 || height <= 0) return;
    TexOp op = make_tex_op(tex_id, 0, 0, width, height, rgba_pixels);
    op.whole = true;
    std::lock_guard<std::mutex> lk(g_tex_mu);
    g_tex_sizes[tex_id] = TexSize{width, height};
    queue_tex_op(std::move(op));
}

// RenderManager.TextureDataUpdate: new pixels for part or all of a texture
// that's already there (the map, the animated block and item atlases). A
// texture that isn't there yet is made at the update's size. A whole-texture
// update of a different size isn't the texture it was meant for, so it's left.
extern "C" void mcle_glbridge_tex_sub_image_rgba(unsigned int tex_id, int x, int y,
                                                   int width, int height,
                                                   const void* rgba_pixels) {
    if (!rgba_pixels || width <= 0 || height <= 0 || x < 0 || y < 0) return;
    TexSize size{0, 0};
    {
        std::lock_guard<std::mutex> lk(g_tex_mu);
        auto it = g_tex_sizes.find(tex_id);
        if (it != g_tex_sizes.end()) size = it->second;
    }
    if (size.width == 0) {
        if (x == 0 && y == 0) mcle_glbridge_tex_image_2d_rgba(tex_id, width, height, rgba_pixels);
        return;
    }
    if (x + width > size.width || y + height > size.height) return;
    if (x == 0 && y == 0 && (width != size.width || height != size.height)) return;
    TexOp op = make_tex_op(tex_id, x, y, width, height, rgba_pixels);
    op.whole = (width == size.width && height == size.height);
    std::lock_guard<std::mutex> lk(g_tex_mu);
    // Deleted or given another image in between: this one was for the old one
    auto it = g_tex_sizes.find(tex_id);
    if (it == g_tex_sizes.end() || it->second.width != size.width || it->second.height != size.height) return;
    queue_tex_op(std::move(op));
}

// Loads a PNG file as a new texture and returns its id, the same id for the
// same path from then on. 0 if the file is missing or doesn't decode, and
// the caller tries its next path.
extern "C" unsigned int mcle_glbridge_load_or_get_png_path(const char* path) {
    if (!path) return 0;
    std::lock_guard<std::mutex> lk(g_path_mu);
    auto it = g_path_to_tex.find(path);
    if (it != g_path_to_tex.end()) return it->second;

    FILE* f = fopen(path, "rb");
    if (!f) {
        LOGI("[mcle_glbridge] PNG missing: %s", path);
        const std::string m = std::string("PNG_MISSING ") + path;
        mcle_log_msg(m.c_str());
        g_path_to_tex[path] = 0;
        return 0;
    }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        g_path_to_tex[path] = 0;
        return 0;
    }
    std::vector<uint8_t> bytes((size_t)sz);
    const size_t read = fread(bytes.data(), 1, (size_t)sz, f);
    fclose(f);
    if (read != (size_t)sz) {
        LOGI("[mcle_glbridge] short read for %s", path);
        g_path_to_tex[path] = 0;
        return 0;
    }

    unsigned char* rgba = nullptr;
    int            w = 0, h = 0;
    if (!mcle_png_decode_rgba8(bytes.data(), (unsigned long)bytes.size(), &rgba, &w, &h)) {
        LOGI("[mcle_glbridge] PNG decode failed: %s", path);
        g_path_to_tex[path] = 0;
        return 0;
    }

    const unsigned int id = g_next_tex_id.fetch_add(1, std::memory_order_relaxed);
    mcle_glbridge_tex_image_2d_rgba(id, w, h, rgba);
    mcle_png_decode_free(rgba);
    g_path_to_tex[path] = id;
    // Which path won the bindTexture fallback chain, and the atlas size
    char buf[512];
    snprintf(buf, sizeof(buf), "PNG_LOADED %s -> id=%u (%dx%d)", path, id, w, h);
    mcle_log_msg(buf);
    return id;
}

// HUD: a textured quad in screen pixels, y down. Sets its own ortho and
// puts the matrices and the bound texture back after.
extern "C" void mcle_hud_draw_textured_quad(int x, int y, int w, int h,
                                             float u0, float v0,
                                             float u1, float v1,
                                             unsigned int tex_id) {
    ThreadState& s = t_state;
    if (!s.in_frame) return;
    int sw = 0, sh = 0;
    mcle_metal_current_size(&sw, &sh);
    if (sw <= 0 || sh <= 0) return;

    const Mat4         savedProj  = s.projection.back();
    const Mat4         savedMv    = s.modelview.back();
    const unsigned int savedTexId = g_bound_tex_id.load(std::memory_order_relaxed);

    mcle_glbridge_matrix_mode(kGL_PROJECTION);
    mcle_glbridge_load_identity();
    mcle_glbridge_metal_ortho(0.0f, (float)sw, (float)sh, 0.0f, -1.0f, 1.0f);
    mcle_glbridge_matrix_mode(kGL_MODELVIEW);
    mcle_glbridge_load_identity();

    if (tex_id != 0) mcle_glbridge_bind_texture(tex_id);

    struct V {
        float    pos[3];
        float    uv[2];
        uint8_t  color[4];
        uint8_t  normal[4];
        uint8_t  extra[4];
    };
    static_assert(sizeof(V) == 32, "HUD vertex must be 32 bytes");
    V v[4];
    const float fx0 = (float)x;
    const float fy0 = (float)y;
    const float fx1 = (float)(x + w);
    const float fy1 = (float)(y + h);
    const uint8_t white[4] = {255, 255, 255, 255};
    const uint8_t up[4]    = {0, 127, 0, 0};
    const uint8_t zero[4]  = {0, 0, 0, 0};
    const auto fill = [&](V& vv, float px, float py, float pu, float pv) {
        vv.pos[0] = px; vv.pos[1] = py; vv.pos[2] = 0.0f;
        vv.uv[0]  = pu; vv.uv[1]  = pv;
        memcpy(vv.color,  white, 4);
        memcpy(vv.normal, up,    4);
        memcpy(vv.extra,  zero,  4);
    };
    // top left, bottom left, bottom right, top right
    fill(v[0], fx0, fy0, u0, v0);
    fill(v[1], fx0, fy1, u0, v1);
    fill(v[2], fx1, fy1, u1, v1);
    fill(v[3], fx1, fy0, u1, v0);

    draw_batch(s, kGL_QUADS, 4, 1, v, nullptr);

    s.projection.back() = savedProj;
    s.modelview.back()  = savedMv;
    if (tex_id != 0) g_bound_tex_id.store(savedTexId, std::memory_order_relaxed);
}

// The context and the layers

// Makes the EGL display, config and context the first time. 0 once there's
// a context.
extern "C" int mcle_metal_ensure_device(void) {
    if (g.context != EGL_NO_CONTEXT) return 0;
    const EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
        LOGI("[mcle_gl] no EGL display: 0x%x", eglGetError());
        return 1;
    }
    const EGLConfig config = choose_config(display);
    if (!config) {
        LOGI("[mcle_gl] no RGBA8888 depth 24 ES 3 config");
        return 1;
    }
    const EGLint attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, attribs);
    if (context == EGL_NO_CONTEXT) {
        LOGI("[mcle_gl] failed to create an ES 3 context: 0x%x", eglGetError());
        return 2;
    }
    g.display = display;
    g.config  = config;
    g.context = context;
    // The format the windows' buffers need for this config
    eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &g.visual);
    return 0;
}

extern "C" int mcle_metal_available(void) {
    return g.context != EGL_NO_CONTEXT ? 1 : 0;
}

// The world's ANativeWindow, with the size its buffers are to have. The
// same window again only resizes, a new one replaces the surface, and none
// releases it. The context is current on the calling thread after.
extern "C" void mcle_metal_attach_layer(void* native_window, int width, int height) {
    if (mcle_metal_ensure_device() != 0) return;
    attach(g.world, native_window, width, height);
}

// The same for the layer over the movies. Its window has to be a
// translucent one: what's drawn here is premultiplied, on transparent.
extern "C" void mcle_metal_attach_ui_layer(void* native_window, int width, int height) {
    if (mcle_metal_ensure_device() != 0) return;
    attach(g.ui, native_window, width, height);
}

extern "C" unsigned long mcle_gl_world_frames(void) {
    return g_world_frames.load(std::memory_order_relaxed);
}

extern "C" void mcle_metal_current_size(int* out_width, int* out_height) {
    if (out_width)  *out_width  = g.world.width;
    if (out_height) *out_height = g.world.height;
}

extern "C" void mcle_metal_ui_size(int* out_width, int* out_height) {
    if (out_width)  *out_width  = g.ui.width;
    if (out_height) *out_height = g.ui.height;
}

// There is no encoder to hand out, nothing outside this file draws into a frame
extern "C" void* mcle_metal_current_encoder(void) {
    return nullptr;
}

extern "C" void* mcle_metal_shared_device_objc(void) {
    return nullptr;
}

extern "C" void mcle_metal_set_gamma(float gamma) {
    // What the UI layer shows went through the old curve
    if (g_gamma.exchange(gamma, std::memory_order_relaxed) != gamma) g_ui_key_shown = false;
}

// The save thumbnail: asked for here, taken from the next world frame, then
// handed over once as 64x64 RGBA, top row first

extern "C" void mcle_metal_capture_thumbnail(void) {
    {
        std::lock_guard<std::mutex> lock(g_thumbMutex);
        g_thumbRgba.clear();
    }
    g_thumbState.store(1);
}

extern "C" int mcle_metal_thumbnail_pending(void) {
    return g_thumbState.load() != 0;
}

extern "C" int mcle_metal_take_thumbnail(unsigned char* rgba) {
    std::lock_guard<std::mutex> lock(g_thumbMutex);
    if (g_thumbRgba.size() != (size_t)kThumbSize * kThumbSize * 4) return 0;
    if (rgba) memcpy(rgba, g_thumbRgba.data(), g_thumbRgba.size());
    g_thumbRgba.clear();
    return 1;
}

// Before a world frame begins
extern "C" void mcle_metal_thumbnail_world_frame(void) {
    if (g_thumbState.load() != 1 || g.world.surface == EGL_NO_SURFACE) return;
    g_thumbArmed = true;
}

extern "C" void mcle_metal_add_phase_ms(int phase, double ms) {
    if (phase >= 0 && phase < 3) g_phase_ms[phase] += ms;
}

// Starts a world frame, cleared to the colour given. 0 when it's open.
extern "C" int mcle_metal_frame_begin(float r, float gn, float b, float a) {
    g_frame_start_ns = now_ns();
    if (g.context == EGL_NO_CONTEXT || g.world.surface == EGL_NO_SURFACE) return 1;
    if (g.inFrame) return 0;
    if (!open_frame(g.world, r, gn, b, a)) return 2;

    // Min, average and max time between frames, logged every 60 of them
    static uint64_t s_lastNs = 0;
    static uint64_t s_minNs = UINT64_MAX, s_maxNs = 0, s_sumNs = 0;
    static int      s_count = 0;
    const uint64_t now = now_ns();
    if (s_lastNs != 0) {
        const uint64_t dt = now - s_lastNs;
        if (dt < s_minNs) s_minNs = dt;
        if (dt > s_maxNs) s_maxNs = dt;
        s_sumNs += dt;
        s_count++;
        if (s_count >= 60) {
            char buf[200];
            snprintf(buf, sizeof(buf),
                     "frame_ms min=%.2f avg=%.2f max=%.2f n=%d wait=%.2f world=%.2f ui=%.2f",
                     s_minNs / 1.0e6, (s_sumNs / (double)s_count) / 1.0e6,
                     s_maxNs / 1.0e6, s_count,
                     g_phase_ms[0] / s_count, g_phase_ms[1] / s_count, g_phase_ms[2] / s_count);
            mcle_log_msg(buf);
            s_minNs = UINT64_MAX; s_maxNs = 0; s_sumNs = 0; s_count = 0;
            g_phase_ms[0] = g_phase_ms[1] = g_phase_ms[2] = 0;
        }
    }
    s_lastNs = now;
    return 0;
}

// glClear(GL_DEPTH_BUFFER_BIT) mid frame, renderLevel does it before the hand
extern "C" void mcle_metal_clear_depth(void) {
    if (!t_state.in_frame) return;
    gl_set_depth_write(true);
    glClear(GL_DEPTH_BUFFER_BIT);
}

extern "C" void mcle_metal_frame_end(void) {
    ThreadState& s = t_state;
    if (!g.inFrame || !s.in_frame || g.frameLayer != &g.world) return;
    if (g.gammaThisFrame) gamma_frame_end(false);
    capture_thumbnail();
    close_frame(s);

    const uint64_t swapStart = now_ns();
    present(g.world);
    g_world_frames.fetch_add(1, std::memory_order_relaxed);
    const uint64_t now = now_ns();
    g_phase_ms[0] += (double)(now - swapStart) / 1.0e6;

    // A frame over 200ms is a freeze the player sees, logged to tell the
    // render thread's from the simulation's
    if (g_frame_start_ns != 0) {
        const uint64_t ms = (now - g_frame_start_ns) / 1000000ull;
        if (ms > 200) {
            char buf[80];
            snprintf(buf, sizeof(buf), "SLOW_FRAME took=%llums", (unsigned long long)ms);
            mcle_log_msg(buf);
        }
    }
}

// 0 when the UI layer is ready to draw into
extern "C" int mcle_metal_ui_frame_begin(void) {
    if (g.context == EGL_NO_CONTEXT || g.ui.surface == EGL_NO_SURFACE || g.inFrame ||
        g.ui.width <= 0 || g.ui.height <= 0) {
        return 1;
    }
    if (!open_frame(g.ui, 0.0f, 0.0f, 0.0f, 0.0f)) return 2;
    g_ui_key_next = false;
    return 0;
}

// mcle_metal_ui_frame_begin for a frame that only depends on key. -1 when
// the layer still shows the last one drawn for it.
extern "C" int mcle_metal_ui_frame_begin_keyed(unsigned long long key) {
    if (g_ui_key_shown && g_ui_key == key) return -1;
    const int result = mcle_metal_ui_frame_begin();
    if (result == 0) {
        g_ui_key      = key;
        g_ui_key_next = true;
    }
    return result;
}

extern "C" void mcle_metal_ui_frame_end(void) {
    ThreadState& s = t_state;
    if (!g.inFrame || !s.in_frame || g.frameLayer != &g.ui) return;
    // Gamma goes on the slot items too, PostProcesser runs over the whole
    // frame after ui.render
    if (g.gammaThisFrame) gamma_frame_end(true);
    close_frame(s);
    present(g.ui);
    g_ui_key_shown = g_ui_key_next;
    g_ui_key_next  = false;
}

// Tesselator::end. Recorded into the list being compiled on this thread,
// drawn straight away otherwise.

static std::atomic<unsigned long> g_fmt_counts[16];

extern "C" void mcle_metal_draw_vertices(int prim, int count, const void* data, int fmt, int shader) {
    if (fmt >= 0 && fmt < 16) g_fmt_counts[fmt].fetch_add(1, std::memory_order_relaxed);
    ThreadState& s = t_state;
    if (s.recording_list == 0) {
        draw_batch(s, prim, count, fmt, data, nullptr);
        return;
    }
    DrawCmd cmd;
    cmd.prim   = prim;
    cmd.count  = count;
    cmd.fmt    = fmt;
    cmd.shader = shader;
    cmd.offset = s.recording.blob.size();
    const long long sz = (long long)count * vertex_stride(fmt);
    if (data && sz > 0) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        s.recording.blob.insert(s.recording.blob.end(), p, p + sz);
    } else {
        cmd.count = 0;
    }
    // translateToPos comes before t->end()
    if (s.recording_has_translate) {
        cmd.translate[0] = s.recording_translate[0];
        cmd.translate[1] = s.recording_translate[1];
        cmd.translate[2] = s.recording_translate[2];
        cmd.hasTranslate = true;
    }
    cmd.texId = s.recording_tex;
    s.recording.draws.push_back(cmd);
}

extern "C" void mcle_glbridge_fmt_stats(unsigned long* out, int max_count) {
    if (!out) return;
    const int n = (max_count < 16) ? max_count : 16;
    for (int i = 0; i < n; i++) out[i] = g_fmt_counts[i].load(std::memory_order_relaxed);
}

extern "C" unsigned long long mcle_metal_draw_count(void) {
    return g_draw_count.load(std::memory_order_relaxed);
}

// glGenLists / glNewList / glEndList / glCallList / glDeleteLists

extern "C" int mcle_glbridge_gen_lists(int range) {
    return g_next_list_id.fetch_add(range, std::memory_order_relaxed);
}

extern "C" void mcle_glbridge_begin_list(int id, int /*mode*/) {
    ThreadState& s = t_state;
    s.recording_list = id;
    s.recording_tex  = 0;
    s.recording.retire();
    s.recording_has_translate = false;
    s.recording_translate[0] = 0;
    s.recording_translate[1] = 0;
    s.recording_translate[2] = 0;
}

extern "C" void mcle_glbridge_end_list(void) {
    ThreadState& s = t_state;
    const int id = s.recording_list;
    if (id == 0) {
        // No list was begun, nothing recorded is kept for the next one
        s.recording.retire();
        return;
    }
    s.recording.bytes = s.recording.blob.size();
    if (s.deferred_on) {
        s.deferred.emplace_back(id, std::move(s.recording));
    } else {
        std::unique_lock<std::shared_mutex> lk(g_lists_mu);
        g_lists[id] = std::move(s.recording);
    }
    s.recording_list = 0;
}

static unsigned long g_call_list_hits = 0;
static unsigned long g_call_list_misses = 0;
static int g_call_list_first_miss_id = -1;
static int g_call_list_first_hit_id  = -1;
static int g_call_list_last_hit_id   = -1;
// Hits by id range, the chunk lists have the lower ids
static unsigned long g_call_list_hits_low  = 0;
static unsigned long g_call_list_hits_high = 0;

extern "C" void mcle_glbridge_call_list(int id) {
    ThreadState& s = t_state;
    std::shared_lock<std::shared_mutex> lk(g_lists_mu);
    auto it = g_lists.find(id);
    if (it == g_lists.end()) {
        if (g_call_list_first_miss_id == -1) g_call_list_first_miss_id = id;
        g_call_list_misses++;
        return;
    }
    if (g_call_list_first_hit_id == -1) g_call_list_first_hit_id = id;
    g_call_list_last_hit_id = id;
    g_call_list_hits++;
    if (id < 4000000) g_call_list_hits_low++; else g_call_list_hits_high++;
    replay_list(s, it->second);
}

extern "C" void mcle_glbridge_call_list_stats(unsigned long* hits, unsigned long* misses,
                                                int* first_miss, int* first_hit, unsigned long* list_count) {
    if (hits)        *hits        = g_call_list_hits;
    if (misses)      *misses      = g_call_list_misses;
    if (first_miss)  *first_miss  = g_call_list_first_miss_id;
    if (first_hit)   *first_hit   = g_call_list_first_hit_id;
    if (list_count) {
        std::shared_lock<std::shared_mutex> lk(g_lists_mu);
        *list_count = (unsigned long)g_lists.size();
    }
}

extern "C" void mcle_glbridge_call_list_stats_ext(unsigned long* hits_low,
                                                    unsigned long* hits_high,
                                                    int* last_hit) {
    if (hits_low)  *hits_low  = g_call_list_hits_low;
    if (hits_high) *hits_high = g_call_list_hits_high;
    if (last_hit)  *last_hit  = g_call_list_last_hit_id;
}

extern "C" void mcle_glbridge_release_lists(int id, int range) {
    std::unique_lock<std::shared_mutex> lk(g_lists_mu);
    for (int i = 0; i < range; i++) g_lists.erase(id + i);
}

extern "C" void mcle_glbridge_deferred_lists_start(void) {
    t_state.deferred_on = true;
}

// Every list held back goes in under one lock, a later one for the same id
// over an earlier one
extern "C" void mcle_glbridge_deferred_lists_end(void) {
    ThreadState& s = t_state;
    if (!s.deferred_on) return;
    s.deferred_on = false;
    {
        std::unique_lock<std::shared_mutex> lk(g_lists_mu);
        for (auto& held : s.deferred) g_lists[held.first] = std::move(held.second);
    }
    s.deferred.clear();
}

extern "C" unsigned long long mcle_glbridge_list_count(void) {
    std::shared_lock<std::shared_mutex> lk(g_lists_mu);
    return static_cast<unsigned long long>(g_lists.size());
}

// Memory held by the lists, vertices included wherever they are now. If
// chunks get rebuilt and their old lists aren't released, this keeps growing.
extern "C" unsigned long long mcle_glbridge_list_bytes(void) {
    std::shared_lock<std::shared_mutex> lk(g_lists_mu);
    unsigned long long total = 0;
    for (const auto& kv : g_lists) {
        const DisplayList& dl = kv.second;
        total += sizeof(DisplayList);
        total += dl.draws.capacity() * sizeof(DrawCmd);
        total += dl.bytes;
    }
    return total;
}

extern "C" unsigned long long mcle_glbridge_total_draws(void) {
    std::shared_lock<std::shared_mutex> lk(g_lists_mu);
    unsigned long long total = 0;
    for (const auto& kv : g_lists) {
        total += kv.second.draws.size();
    }
    return total;
}

// Lists the replay of every list leaves out: the sky, stars, clouds and the
// halo ring, which renderSky and renderAdvancedClouds call themselves (or,
// for the ring, never do)
static std::unordered_set<int> g_skip_autoreplay_lists;

extern "C" void mcle_glbridge_skip_autoreplay(int id) {
    g_skip_autoreplay_lists.insert(id);
}

// Only the chunk lists are replayed that way. Everything else (mob model
// parts, items) is drawn by its own glCallList with the right matrix.
static int g_autoreplay_first = 0, g_autoreplay_end = 0;

extern "C" void mcle_glbridge_set_autoreplay_range(int first, int count) {
    g_autoreplay_first = first;
    g_autoreplay_end = first + count;
}

extern "C" void mcle_glbridge_replay_all_lists(void) {
    ThreadState& s = t_state;
    const bool ranged = g_autoreplay_end > g_autoreplay_first;
    std::shared_lock<std::shared_mutex> lk(g_lists_mu);
    for (auto& kv : g_lists) {
        if (ranged && (kv.first < g_autoreplay_first || kv.first >= g_autoreplay_end)) continue;
        if (g_skip_autoreplay_lists.count(kv.first)) continue;
        replay_list(s, kv.second);
    }
}
