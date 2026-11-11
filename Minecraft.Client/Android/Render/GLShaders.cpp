// GLSL ES 3.00 sources for the renderer and the program builder.

#include <android/log.h>

#include <vector>

#include "GLInternal.h"

namespace mcle_gl {

// VERTEX_TYPE_PF3_TF2_CB4_NB4_XW1 and its TEXGEN variant, 32 bytes a vertex.
// u_light: 0 and 1 the light directions in eye space, 2 and 3 their colours,
// 4 the ambient colour, 5 the flags (x lit, y the light layer, zw
// StateSetVertexTextureUV's u - 1 and v - 1), 6..8 the modelview's 3x3
// columns for the normal.
// u_texgen_mats: the modelview, then matTexGenView and matTexGenObj.
const char* const kWorldVertSrc = R"(#version 300 es
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec4 a_normal;
layout(location = 4) in vec2 a_tex2;

uniform mat4  u_mvp;
uniform mat4  u_tex_matrix;
uniform vec4  u_color;
uniform vec4  u_mv_row2;
uniform vec4  u_light[9];
uniform float u_texgen;
uniform mat4  u_texgen_mats[3];

out vec4  v_color;
out vec2  v_uv;
out vec2  v_lightmap_uv;
out float v_fog_dist;
out float v_uvw;

void main() {
    vec4 p = vec4(a_pos, 1.0);
    gl_Position = u_mvp * p;
    // Tesselator::color packs (r<<24)|(g<<16)|(b<<8)|a, so the bytes in
    // memory are A, B, G, R
    vec4 vc = a_color.wzyx;
    // GL_LIGHTING: the lit colour takes the vertex colour's place and its
    // alpha is the lit one
    if (u_light[5].x > 0.5) {
        vec3 n = mat3(u_light[6].xyz, u_light[7].xyz, u_light[8].xyz) * a_normal.xyz;
        float len = length(n);
        if (len > 0.0) n /= len;
        float d0 = max(0.0, dot(u_light[0].xyz, n));
        float d1 = max(0.0, dot(u_light[1].xyz, n));
        vec4 lit = clamp(u_light[4] + d0 * u_light[2] + d1 * u_light[3], 0.0, 1.0);
        vc = vec4(lit.rgb * vc.rgb, lit.a);
    }
    v_color = vc * u_color;
    vec4 tc = u_tex_matrix * vec4(a_uv, 0.0, 1.0);
    v_uv  = tc.xy;
    v_uvw = 1.0;
    // Each coordinate is the eye position against its eye plane plus the
    // vertex against its object plane, with q kept for the divide
    if (u_texgen > 0.5) {
        vec4 gen = u_texgen_mats[1] * (u_texgen_mats[0] * p) + u_texgen_mats[2] * p;
        tc = u_tex_matrix * gen;
        v_uv  = tc.xy;
        v_uvw = tc.w;
    }
    // The lightmap at the vertex's tex2, or at the one set for vertices
    // without one (theirs is -512, below any set one)
    v_lightmap_uv = u_light[5].y > 0.5 ? fract(max(u_light[5].zw, a_tex2 / 256.0)) : vec2(0.5, 0.5);
    v_fog_dist = abs(dot(u_mv_row2, p));
}
)";

// VERTEX_TYPE_COMPRESSED, 16 bytes a vertex: position * 1024, colour as
// 5:6:5 biased by -32768, uv * 8192, then the block and sky light * 16.
// u_chunk: 0 the matrix's last column with the chunk's translation in it,
// 1 x the same for u_mv_row2's w.
const char* const kWorldCompactVertSrc = R"(#version 300 es
layout(location = 0) in vec3  a_pos;
layout(location = 1) in float a_pcol;
layout(location = 2) in vec2  a_uv;
layout(location = 3) in vec2  a_tex2;

uniform mat4 u_mvp;
uniform mat4 u_tex_matrix;
uniform vec4 u_color;
uniform vec4 u_mv_row2;
uniform vec4 u_chunk[2];

out vec4  v_color;
out vec2  v_uv;
out vec2  v_lightmap_uv;
out float v_fog_dist;
out float v_uvw;

void main() {
    vec3 pos = a_pos / 1024.0;
    vec2 uv  = a_uv / 8192.0;
    gl_Position = u_mvp[0] * pos.x + u_mvp[1] * pos.y + u_mvp[2] * pos.z + u_chunk[0];
    vec4 tc = u_tex_matrix * vec4(uv, 0.0, 1.0);
    int bits = int(floor(a_pcol + 0.5)) + 32768;
    float r = float((bits >> 11) & 0x1F) / 31.0;
    float g = float((bits >>  5) & 0x3F) / 63.0;
    float b = float( bits        & 0x1F) / 31.0;
    v_color = vec4(r, g, b, 1.0) * u_color;
    v_uv    = tc.xy;
    v_uvw   = 1.0;
    // + 8 puts each level in the middle of its lightmap cell
    v_lightmap_uv = (a_tex2 + 8.0) / 256.0;
    v_fog_dist = abs(dot(u_mv_row2.xyz, pos) + u_chunk[1].x);
}
)";

// u_fog: 0 the colour with GL_FOG's enable in w, 1 start, end, 1 for GL_EXP
// and the density. u_alpha_ref is 0 with the alpha test off.
const char* const kWorldFragSrc = R"(#version 300 es
precision highp float;

uniform sampler2D u_tex;
uniform sampler2D u_lightmap;
uniform vec4  u_fog[2];
uniform float u_alpha_ref;
uniform float u_texgen;

in vec4  v_color;
in vec2  v_uv;
in vec2  v_lightmap_uv;
in float v_fog_dist;
in float v_uvw;

out vec4 o_color;

// PIXEL_SHADER_TYPE_PROJECTION divides by q first, and past u = 1 it reads
// the top level
vec4 texel() {
    if (u_texgen < 0.5) return texture(u_tex, v_uv);
    vec2 uv  = v_uv / v_uvw;
    vec4 top = textureLod(u_tex, uv, 0.0);
    vec4 mip = texture(u_tex, uv);
    return uv.x > 1.0 ? top : mip;
}

void main() {
    vec4 c = v_color * texel();
    if (c.a < u_alpha_ref) discard;
    c.rgb *= texture(u_lightmap, v_lightmap_uv).rgb;
    float t;
    if (u_fog[1].z > 0.5) {
        t = 1.0 - clamp(exp(-u_fog[1].w * v_fog_dist), 0.0, 1.0);
    } else {
        t = clamp((v_fog_dist - u_fog[1].x) / (u_fog[1].y - u_fog[1].x), 0.0, 1.0);
    }
    c.rgb = mix(c.rgb, u_fog[0].rgb, t * u_fog[0].w);
    o_color = c;
}
)";

const char* const kScreenVertSrc = R"(#version 300 es
out vec2 v_uv;
void main() {
    vec2 uv = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    v_uv = uv;
}
)";

// PostProcesser's gamma shader, pow(rgb, 1/gamma). u_params.x is the gamma,
// y is set for the UI layer, which is premultiplied so the curve goes on the
// colour under its alpha.
const char* const kGammaFragSrc = R"(#version 300 es
precision highp float;
uniform sampler2D u_scene;
uniform vec2 u_params;
in vec2 v_uv;
out vec4 o_color;
void main() {
    vec4 color = texture(u_scene, v_uv);
    if (u_params.y > 0.5) {
        if (color.a > 0.0) {
            color.rgb = pow(clamp(color.rgb / color.a, 0.0, 1.0), vec3(1.0 / u_params.x)) * color.a;
        }
    } else {
        color.rgb = pow(max(color.rgb, 0.0), vec3(1.0 / u_params.x));
    }
    o_color = color;
}
)";

// The thumbnail is read back bottom row first, so v is turned over here and
// the rows come out top down
const char* const kThumbVertSrc = R"(#version 300 es
out vec2 v_uv;
void main() {
    vec2 uv = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    v_uv = vec2(uv.x, 1.0 - uv.y);
}
)";

// 16 taps a pixel over the square being shrunk to 64x64
const char* const kThumbFragSrc = R"(#version 300 es
precision highp float;
uniform sampler2D u_src;
in vec2 v_uv;
out vec4 o_color;
void main() {
    vec3 sum = vec3(0.0);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            vec2 o = (vec2(float(x), float(y)) + 0.5) / 4.0 - 0.5;
            sum += texture(u_src, v_uv + o / 64.0).rgb;
        }
    }
    o_color = vec4(sum / 16.0, 1.0);
}
)";

static GLuint compile_shader(const char* label, GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) return shader;
    GLint len = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
    std::vector<char> log((size_t)(len > 1 ? len : 1), '\0');
    glGetShaderInfoLog(shader, (GLsizei)log.size(), nullptr, log.data());
    __android_log_print(ANDROID_LOG_INFO, "mcle", "[mcle_gl] %s %s shader compile failed: %s", label,
                        type == GL_VERTEX_SHADER ? "vertex" : "fragment", log.data());
    glDeleteShader(shader);
    return 0;
}

GLuint build_program(const char* label, const char* vert_src, const char* frag_src) {
    GLuint vert = compile_shader(label, GL_VERTEX_SHADER, vert_src);
    GLuint frag = vert ? compile_shader(label, GL_FRAGMENT_SHADER, frag_src) : 0;
    if (!vert || !frag) {
        if (vert) glDeleteShader(vert);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glLinkProgram(program);
    // The program keeps what it linked
    glDetachShader(program, vert);
    glDetachShader(program, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok == GL_TRUE) return program;
    GLint len = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
    std::vector<char> log((size_t)(len > 1 ? len : 1), '\0');
    glGetProgramInfoLog(program, (GLsizei)log.size(), nullptr, log.data());
    __android_log_print(ANDROID_LOG_INFO, "mcle", "[mcle_gl] %s program link failed: %s", label, log.data());
    glDeleteProgram(program);
    return 0;
}

} // namespace mcle_gl
