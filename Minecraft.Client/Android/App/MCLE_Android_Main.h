#pragma once

// The app's frame thread stands in for the main queue: the menus, the
// movie player and the renderer all run on it.

#include <cstdint>
#include <functional>

// Runs on the frame thread at the top of its next frame
void mcle_main_async(std::function<void()> work);

// The same once the time is up
void mcle_main_after(int64_t milliseconds, std::function<void()> work);

// True on the frame thread
bool mcle_main_is_current(void);

// Seconds on a clock that only goes forward (CACurrentMediaTime)
double mcle_media_time(void);
