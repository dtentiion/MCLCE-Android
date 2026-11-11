#include "INP_Android_Controller.h"
#include "INP_iOS_Bridge.h"

// 4J_Input.h leans on Win32 types. Pull compat shims first.
#include "iOS_WinCompat.h"
#include "4J_Input.h"  // for _360_JOY_BUTTON_* macros

#include <android/keycodes.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <utility>

extern "C" unsigned char mcle_settings_get(int setting); // MCLE_Android_Settings.cpp

namespace {

constexpr int kMaxPads = 4;
constexpr int kSonyVendor = 0x054c;

// What a controller last reported, before it's turned into a pad state
struct Pad {
    bool connected = false;
    int device = -1;
    int vendor = 0;
    uint32_t keys = 0;
    // Keys that went down since the last frame, and the ones let go again
    // before a frame saw them down
    uint32_t fresh = 0;
    uint32_t let_go = 0;
    uint32_t release = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
    int hat_x = 0, hat_y = 0;
};

std::mutex g_mu;
Pad g_pads[kMaxPads];
// The pad last used is the first player's. Some phones list devices as
// gamepads that never send anything, and one of those can come first.
int g_first = 0;

// The slot a player's pad is in
int slot_of(int index) {
    if (index == 0) return g_first;
    return index == g_first ? 0 : index;
}

Pad* find_pad(int device) {
    for (Pad& pad : g_pads) {
        if (pad.connected && pad.device == device) return &pad;
    }
    return nullptr;
}

// An event can come from a controller that was plugged in before the app
// was told about it
Pad* find_or_add_pad(int device) {
    if (Pad* pad = find_pad(device)) return pad;
    for (Pad& pad : g_pads) {
        if (!pad.connected) {
            pad = Pad();
            pad.connected = true;
            pad.device = device;
            return &pad;
        }
    }
    return nullptr;
}

uint32_t key_bit(int key_code) {
    switch (key_code) {
    case AKEYCODE_BUTTON_A: return _360_JOY_BUTTON_A;
    case AKEYCODE_BUTTON_B: return _360_JOY_BUTTON_B;
    case AKEYCODE_BUTTON_X: return _360_JOY_BUTTON_X;
    case AKEYCODE_BUTTON_Y: return _360_JOY_BUTTON_Y;
    case AKEYCODE_BUTTON_L1: return _360_JOY_BUTTON_LB;
    case AKEYCODE_BUTTON_R1: return _360_JOY_BUTTON_RB;
    case AKEYCODE_BUTTON_L2: return _360_JOY_BUTTON_LT;
    case AKEYCODE_BUTTON_R2: return _360_JOY_BUTTON_RT;
    case AKEYCODE_BUTTON_THUMBL: return _360_JOY_BUTTON_LTHUMB;
    case AKEYCODE_BUTTON_THUMBR: return _360_JOY_BUTTON_RTHUMB;
    case AKEYCODE_BUTTON_START: return _360_JOY_BUTTON_START;
    case AKEYCODE_BUTTON_SELECT: return _360_JOY_BUTTON_BACK;
    // What a pad without a key layout of its own sends for the same button
    case AKEYCODE_BACK: return _360_JOY_BUTTON_BACK;
    case AKEYCODE_DPAD_UP: return _360_JOY_BUTTON_DPAD_UP;
    case AKEYCODE_DPAD_DOWN: return _360_JOY_BUTTON_DPAD_DOWN;
    case AKEYCODE_DPAD_LEFT: return _360_JOY_BUTTON_DPAD_LEFT;
    case AKEYCODE_DPAD_RIGHT: return _360_JOY_BUTTON_DPAD_RIGHT;
    default: return 0;
    }
}

mcle_ios_pad_state snapshot(const Pad& pad) {
    mcle_ios_pad_state s = {};
    s.connected = 1;
    s.buttons = pad.keys & ~(_360_JOY_BUTTON_LT | _360_JOY_BUTTON_RT);

    if (pad.hat_y < 0) s.buttons |= _360_JOY_BUTTON_DPAD_UP;
    if (pad.hat_y > 0) s.buttons |= _360_JOY_BUTTON_DPAD_DOWN;
    if (pad.hat_x < 0) s.buttons |= _360_JOY_BUTTON_DPAD_LEFT;
    if (pad.hat_x > 0) s.buttons |= _360_JOY_BUTTON_DPAD_RIGHT;

    // Y is up-positive in the pad state
    s.lx = pad.lx;
    s.ly = -pad.ly;
    s.rx = pad.rx;
    s.ry = -pad.ry;
    // A pad with digital triggers only sends the keys
    s.lt = (pad.keys & _360_JOY_BUTTON_LT) ? 1.0f : pad.lt;
    s.rt = (pad.keys & _360_JOY_BUTTON_RT) ? 1.0f : pad.rt;

    mcle_ios_pad_state_finish(&s, mcle_settings_get(11 /*ControlSouthPaw*/));

    return s;
}

} // namespace

void mcle_android_pad_changed(int device, int connected, int vendor) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (connected) {
        if (Pad* pad = find_or_add_pad(device)) pad->vendor = vendor;
    } else if (Pad* pad = find_pad(device)) {
        *pad = Pad();
        if (!g_pads[g_first].connected) {
            g_first = 0;
            for (int i = 0; i < kMaxPads; i++) {
                if (g_pads[i].connected) {
                    g_first = i;
                    break;
                }
            }
        }
    }
}

void mcle_android_pad_key(int device, int key_code, int down) {
    const uint32_t bit = key_bit(key_code);
    if (!bit) return;
    std::lock_guard<std::mutex> lk(g_mu);
    Pad* pad = find_or_add_pad(device);
    if (!pad) return;
    if (down) {
        g_first = (int)(pad - g_pads);
        pad->keys |= bit;
        pad->fresh |= bit;
    } else if (pad->fresh & bit) {
        pad->let_go |= bit;
    } else {
        pad->keys &= ~bit;
    }
}

void mcle_android_pad_frame(void) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (Pad& pad : g_pads) {
        pad.keys &= ~pad.release;
        pad.release = pad.let_go;
        pad.let_go = 0;
        pad.fresh = 0;
    }
}

void mcle_android_pad_axes(int device, const float* axes, int count) {
    if (!axes || count < 10) return;
    std::lock_guard<std::mutex> lk(g_mu);
    Pad* pad = find_or_add_pad(device);
    if (!pad) return;
    pad->lx = axes[0];
    pad->ly = axes[1];
    pad->rx = axes[2];
    pad->ry = axes[3];
    // The triggers are LTRIGGER and RTRIGGER on some pads, BRAKE and GAS on others
    pad->lt = std::max(axes[4], axes[6]);
    pad->rt = std::max(axes[5], axes[7]);
    pad->hat_x = axes[8] > 0.5f ? 1 : (axes[8] < -0.5f ? -1 : 0);
    pad->hat_y = axes[9] > 0.5f ? 1 : (axes[9] < -0.5f ? -1 : 0);
    const float kUsed = 0.5f;
    if (std::fabs(pad->lx) > kUsed || std::fabs(pad->ly) > kUsed || std::fabs(pad->rx) > kUsed ||
        std::fabs(pad->ry) > kUsed || pad->lt > kUsed || pad->rt > kUsed || pad->hat_x || pad->hat_y) {
        g_first = (int)(pad - g_pads);
    }
}

int mcle_android_pad_is_playstation(void) {
    std::lock_guard<std::mutex> lk(g_mu);
    const Pad& pad = g_pads[g_first];
    if (!pad.connected) return -1;
    return pad.vendor == kSonyVendor ? 1 : 0;
}

extern "C" void mcle_ios_input_init(void) {}
extern "C" void mcle_ios_input_shutdown(void) {}

extern "C" int mcle_ios_input_poll(int index, mcle_ios_pad_state* out) {
    if (!out || index < 0 || index >= kMaxPads) return 0;
    std::lock_guard<std::mutex> lk(g_mu);
    const Pad& pad = g_pads[slot_of(index)];
    if (!pad.connected) {
        *out = {};
        return 0;
    }
    *out = snapshot(pad);
    return 1;
}
