#pragma once

// The activity's controller events, kept as the pad state INP_iOS_Bridge.h
// hands out.

#ifdef __cplusplus
extern "C" {
#endif

// A controller came or went. vendor is its USB vendor id.
void mcle_android_pad_changed(int device, int connected, int vendor);

// An android.view.KeyEvent key code going down or up
void mcle_android_pad_key(int device, int key_code, int down);

// Once at the top of each frame. A key tapped and let go between two frames
// stays down for the one after, so the game sees the press.
void mcle_android_pad_frame(void);

// lx ly rx ry lt rt brake gas hat_x hat_y as MotionEvent gives them (y down)
void mcle_android_pad_axes(int device, const float* axes, int count);

// 1 when the first pad is a PlayStation one, 0 when it isn't, -1 with no pad
int mcle_android_pad_is_playstation(void);

#ifdef __cplusplus
}
#endif
