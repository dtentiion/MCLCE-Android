#pragma once

// What the menu host asks of the app shell, the things UIKit did for the
// iOS port's view controller.

#include <cstdint>
#include <string>
#include <vector>

#include "MCLEHostSupport.h"

namespace platform {

// The screen in density-independent units, and how many pixels one is
// (self.view.bounds.size and the screen's nativeScale)
host::Size view_size();
float view_scale();

// The four surfaces, each an ANativeWindow: the world's, the movies', the
// one the game draws items over the movies in, and the one for what a
// movie has over those
void* world_layer();
void* swf_layer();
void* custom_draw_layer();
void* swf_top_layer();

// 1 with a PlayStation pad as the first pad, 0 with another, -1 with none
int pad_is_playstation();

// The yellow splash line by the logo (CXuiCtrlSplashPulser). The centre is
// in view_size units, the rotation in radians.
void splash_set_text(const std::string& text);
void splash_set_hidden(bool hidden);
void splash_set_alpha(float alpha);
void splash_set_transform(float centre_x, float centre_y, float rotation, float scale);

// The white Windows64 clears to outside a game, behind the intro's logos
void intro_backdrop_set_shown(bool shown);

// Seconds from the frame before to the one being drawn
float frame_seconds();

} // namespace platform
