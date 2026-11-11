// Image decoder for the texture loaders. Each console plugs its own decoder
// into RenderManager.LoadTextureData, here it's stb_image. PNG bytes in,
// RGBA bytes out, free with the matching mcle_png_decode_free().

#include <android/log.h>

#include <climits>
#include <cstdio>
#include <mutex>

#include <sys/stat.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma clang diagnostic pop

extern "C" int mcle_png_decode_rgba8(const void* data, unsigned long length,
                                       unsigned char** out_rgba,
                                       int* out_w, int* out_h) {
    if (!data || length == 0 || !out_rgba || !out_w || !out_h) return 0;
    *out_rgba = NULL; *out_w = 0; *out_h = 0;
    if (length > (unsigned long)INT_MAX) return 0;

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory((const stbi_uc*)data, (int)length,
                                                  &w, &h, &channels, 4);
    if (!pixels) return 0;
    if (w <= 0 || h <= 0) {
        stbi_image_free(pixels);
        return 0;
    }

    // R, G, B, A bytes, top row first, with the colour premultiplied by
    // alpha. The textures and BufferedImage's pixels are expected that way.
    if (channels == 2 || channels == 4) {
        const size_t count = (size_t)w * (size_t)h;
        for (size_t i = 0; i < count; i++) {
            unsigned char* p = pixels + i * 4;
            const unsigned int a = p[3];
            if (a == 255) continue;
            p[0] = (unsigned char)((p[0] * a + 127) / 255);
            p[1] = (unsigned char)((p[1] * a + 127) / 255);
            p[2] = (unsigned char)((p[2] * a + 127) / 255);
        }
    }

    *out_rgba = pixels;
    *out_w    = w;
    *out_h    = h;
    return 1;
}

extern "C" void mcle_png_decode_free(unsigned char* pixels) {
    stbi_image_free(pixels);
}

extern "C" const char* ios_documents_dir(void);

// The game's log line: logcat, and two files in the documents folder.
// crash_log.txt is the one the tools already pull, mcle_log.txt has these
// lines and nothing else.
extern "C" int mcle_log_msg(const char* msg) {
    if (!msg) return 0;
    __android_log_print(ANDROID_LOG_INFO, "mcle", "[MCLE/up] %s", msg);

    static std::mutex mu;
    static FILE* legacy = nullptr;
    static FILE* clean  = nullptr;
    std::lock_guard<std::mutex> lk(mu);
    if (!legacy || !clean) {
        // Not known until the app has set its folders up
        const char* root = ios_documents_dir();
        if (root && *root) {
            char path[1024];
            if (!legacy) {
                snprintf(path, sizeof(path), "%s/crash_log.txt", root);
                legacy = fopen(path, "a");
            }
            if (!clean) {
                snprintf(path, sizeof(path), "%s/mcle_log.txt", root);
                // It's only ever added to. Past a few megabytes the one
                // there is kept as the old one and a new one started.
                struct stat st;
                if (stat(path, &st) == 0 && st.st_size > 8 * 1024 * 1024) {
                    char old[1024];
                    snprintf(old, sizeof(old), "%s/mcle_log_old.txt", root);
                    rename(path, old);
                }
                clean = fopen(path, "a");
            }
        }
    }
    if (legacy) {
        fprintf(legacy, "[mcle] %s\n", msg);
        fflush(legacy);
    }
    if (clean) {
        fprintf(clean, "[mcle] %s\n", msg);
        fflush(clean);
    }
    return 1;
}
