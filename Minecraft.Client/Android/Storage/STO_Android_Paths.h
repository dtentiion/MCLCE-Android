#pragma once

// Where the app's folders are, handed over from the activity before
// anything asks for them. STO_iOS_Paths.h's functions answer from these.

#ifdef __cplusplus
extern "C" {
#endif

// documents is the folder the game files are copied into, support and
// caches are the app's private ones
void mcle_android_set_dirs(const char* documents, const char* support, const char* caches);

#ifdef __cplusplus
}
#endif
