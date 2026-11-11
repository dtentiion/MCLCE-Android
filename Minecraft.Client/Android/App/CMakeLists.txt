# The app's native library: the shell the activity loads, the menu host,
# and the game behind it.

set(MCLE_APP_SOURCES
  MCLE_Android_App.cpp
  MCLE_Android_Audio.cpp
  MCLE_Android_Files.cpp
  MCLE_Android_Settings.cpp
  MCLEHost.cpp
  MCLEHostSupport.cpp
  ../Input/INP_Android_Controller.cpp
  ../Storage/STO_Android_Paths.cpp
  ${IOS_PLATFORM_DIR}/Input/INP_iOS_Bridge.cpp
)

add_library(mcle SHARED ${MCLE_APP_SOURCES})

target_include_directories(mcle PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${ANDROID_PLATFORM_DIR}/Input
  ${ANDROID_PLATFORM_DIR}/Storage
  ${IOS_PLATFORM_DIR}/App
  ${IOS_PLATFORM_DIR}
  ${IOS_PLATFORM_DIR}/Render
  ${IOS_PLATFORM_DIR}/Input
  ${IOS_PLATFORM_DIR}/Storage
  ${THIRD_PARTY_DIR}/miniaudio
  ${UPSTREAM_DIR}/Minecraft.Client/Windows64/4JLibs/inc
)

target_link_libraries(mcle PRIVATE
  ruffle_android
  mcle_android_render
  mcle_ios_game
  # Every object of the game library, its statics register themselves
  -Wl,--whole-archive mcle_world_probe -Wl,--no-whole-archive
  android
  log
  EGL
  GLESv3
  z
)

target_link_options(mcle PRIVATE
  # A few symbols are in the game library twice, once as a stand-in and
  # once for real. The first one is taken, as Apple's linker does.
  -Wl,--allow-multiple-definition
  # compat_shims.cpp asks for an MSVC runtime library by pragma
  -Wl,--no-dependent-libraries
  # Only the JNI entry points are exported, so what nothing reaches is
  # dropped, along with the few things only that dead code refers to
  # (-dead_strip does the same for the iOS build)
  -Wl,--exclude-libs,ALL
  -Wl,--gc-sections
  # Loads on devices with 16 KB pages too
  -Wl,-z,max-page-size=16384
)
