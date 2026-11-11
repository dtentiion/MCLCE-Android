#pragma once

// Shared between the files of the render library, not part of its C API.

#include <GLES3/gl3.h>

namespace mcle_gl {

// The 4J vertex formats and their pixel shader (GLShaders.cpp)
extern const char* const kWorldVertSrc;
extern const char* const kWorldCompactVertSrc;
extern const char* const kWorldFragSrc;

// Three vertices over the whole target, uv 0..1 from the bottom left
extern const char* const kScreenVertSrc;
extern const char* const kGammaFragSrc;

extern const char* const kThumbVertSrc;
extern const char* const kThumbFragSrc;

// 0 when it doesn't compile or link, with the compiler's log in logcat
GLuint build_program(const char* label, const char* vert_src, const char* frag_src);

// Three vertices with no buffers and no depth test, blending or culling.
// Only inside a frame.
void draw_screen_triangle(GLuint program);

// A new size for the world layer's buffers, the window stays
void resize_layer(int width, int height);

} // namespace mcle_gl
