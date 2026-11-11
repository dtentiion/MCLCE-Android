// Android audio backend. Uses miniaudio (same library the console LCE uses
// in SoundEngine.cpp), and its implementation is compiled here for the
// game's SoundEngine, which plays everything.

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "MCLE_Android_Audio.h"

// Include stb_vorbis BEFORE miniaudio's implementation. miniaudio's
// OGG Vorbis path is enabled only when stb_vorbis is visible at
// implementation compile time; without this, ma_sound_init_from_file
// on a .ogg fails with MA_INVALID_FILE (-10). Matches how console
// LCE's SoundEngine.cpp wires the two together.
#include "stb_vorbis.h"

// Every engine goes through the two wrappers at the bottom of this file,
// so they can all be stopped while the app is in the background
#define ma_engine_init ma_engine_init_device
#define ma_engine_uninit ma_engine_uninit_device
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#undef ma_engine_init
#undef ma_engine_uninit

extern "C" ma_result ma_engine_init(const ma_engine_config* pConfig, ma_engine* pEngine);
extern "C" void ma_engine_uninit(ma_engine* pEngine);

#include <algorithm>
#include <mutex>
#include <vector>

// The game's SoundEngine starts a device. iOS suspends it with the app,
// here it'd play on.
namespace {
std::mutex g_engines_mu;
std::vector<ma_engine*> g_engines;
bool g_engines_paused = false;
}

extern "C" ma_result ma_engine_init(const ma_engine_config* pConfig, ma_engine* pEngine) {
    const ma_result result = ma_engine_init_device(pConfig, pEngine);
    if (result == MA_SUCCESS) {
        std::lock_guard<std::mutex> lk(g_engines_mu);
        g_engines.push_back(pEngine);
        if (g_engines_paused) ma_engine_stop(pEngine);
    }
    return result;
}

extern "C" void ma_engine_uninit(ma_engine* pEngine) {
    {
        std::lock_guard<std::mutex> lk(g_engines_mu);
        g_engines.erase(std::remove(g_engines.begin(), g_engines.end(), pEngine), g_engines.end());
    }
    ma_engine_uninit_device(pEngine);
}

void mcle_android_audio_set_paused(bool paused) {
    std::lock_guard<std::mutex> lk(g_engines_mu);
    if (g_engines_paused == paused) return;
    g_engines_paused = paused;
    for (ma_engine* engine : g_engines) {
        if (paused) ma_engine_stop(engine);
        else ma_engine_start(engine);
    }
}
