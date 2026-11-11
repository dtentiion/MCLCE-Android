cmake_minimum_required(VERSION 3.22)

project(MCLE_Android
  VERSION 0.0.1
  LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT ANDROID)
  message(FATAL_ERROR "This builds for Android only, configure it through Gradle or the NDK toolchain file.")
endif()

# The game code is the iOS port's, pulled in as a submodule
set(MCLE_IOS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ios")
set(UPSTREAM_DIR "${MCLE_IOS_DIR}/upstream")
set(IOS_PLATFORM_DIR "${MCLE_IOS_DIR}/Minecraft.Client/iOS")
set(THIRD_PARTY_DIR "${MCLE_IOS_DIR}/third_party")
set(ANDROID_PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Minecraft.Client/Android")

if(NOT EXISTS "${UPSTREAM_DIR}/Minecraft.Client/Common")
  message(FATAL_ERROR "Submodules missing. Run: git submodule update --init --recursive")
endif()

# Same switches as the iOS build, the upstream patches are written against them
add_compile_definitions(
  _IOS=1
  PLATFORM_IOS=1
  _LARGE_WORLDS=1
  __APPLE_IOS__=1
)

# char is signed on iOS and Windows and the game counts on it, here it
# wouldn't be
add_compile_options(-fsigned-char)

# The few Apple headers that code includes, done over bionic
include_directories(BEFORE "${ANDROID_PLATFORM_DIR}/compat")

# The shared libraries are built from the source lists in the iOS tree's
# CMake files, so a file added there is picked up here
set(MCLE_SHARED_INCLUDES
  ${UPSTREAM_DIR}/Minecraft.World
  ${UPSTREAM_DIR}/Minecraft.World/x64headers
  ${UPSTREAM_DIR}/Minecraft.Client
  ${UPSTREAM_DIR}/Minecraft.Client/Common
  ${UPSTREAM_DIR}/Minecraft.Client/Common/GameRules
  ${UPSTREAM_DIR}/Minecraft.Client/Common/DLC
  ${UPSTREAM_DIR}/Minecraft.Client/Common/Network
  ${UPSTREAM_DIR}/Minecraft.Client/Common/Audio
  ${UPSTREAM_DIR}/Minecraft.Client/Common/Tutorial
  ${UPSTREAM_DIR}/Minecraft.Client/Common/Colours
  ${UPSTREAM_DIR}/include
  ${IOS_PLATFORM_DIR}
  ${IOS_PLATFORM_DIR}/compat_headers
)

function(mcle_shared_library name)
  add_library(${name} STATIC ${ARGN})
  target_include_directories(${name} PRIVATE ${MCLE_SHARED_INCLUDES})
  target_compile_options(${name} PRIVATE
    -include "${IOS_PLATFORM_DIR}/iOS_stdafx.h"
    -fdeclspec
    -w
  )
  target_compile_definitions(${name} PRIVATE _LIB _LARGE_WORLDS _CONTENT_PACKAGE)
  set_target_properties(${name} PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
  )
endfunction()

file(STRINGS "${IOS_PLATFORM_DIR}/WorldProbe/CMakeLists.txt" WORLD_LINES
  REGEX "^ *\\$\\{UPSTREAM_DIR\\}/[^ ]+\\.cpp *$")
set(WORLD_SOURCES "")
foreach(line IN LISTS WORLD_LINES)
  string(STRIP "${line}" line)
  string(REPLACE "\${UPSTREAM_DIR}" "${UPSTREAM_DIR}" line "${line}")
  list(APPEND WORLD_SOURCES "${line}")
endforeach()
# crt_compat.cpp fills in for the MSVC runtime, and bionic's FILE is opaque
list(FILTER WORLD_SOURCES EXCLUDE REGEX "/crt_compat\\.cpp$")
mcle_shared_library(mcle_world_probe
  ${WORLD_SOURCES}
  ${IOS_PLATFORM_DIR}/WorldProbe/probe_stub.cpp
  ${IOS_PLATFORM_DIR}/WorldProbe/link_stubs.cpp
  ${IOS_PLATFORM_DIR}/iOS_Threading.cpp
)
target_compile_definitions(mcle_world_probe PRIVATE MCLE_PROBE_BUILD=1)

# The Mach exception handler has nothing to catch here
file(STRINGS "${IOS_PLATFORM_DIR}/Game/CMakeLists.txt" GAME_LINES
  REGEX "^ *MCLE[A-Za-z0-9_]+\\.cpp *$")
set(GAME_SOURCES "")
foreach(line IN LISTS GAME_LINES)
  string(STRIP "${line}" line)
  if(NOT line STREQUAL "MCLE_iOS_MachHandler.cpp")
    list(APPEND GAME_SOURCES "${IOS_PLATFORM_DIR}/Game/${line}")
  endif()
endforeach()
mcle_shared_library(mcle_ios_game ${GAME_SOURCES})

# The movie player. scripts/build-ruffle.sh builds it ahead of this.
set(RUFFLE_ANDROID_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ruffle_android")
add_library(ruffle_android STATIC IMPORTED GLOBAL)
set_target_properties(ruffle_android PROPERTIES
  IMPORTED_LOCATION "${RUFFLE_ANDROID_DIR}/target/aarch64-linux-android/release/libruffle_android.a"
  INTERFACE_INCLUDE_DIRECTORIES "${THIRD_PARTY_DIR}/ruffle_ios/include"
)

add_subdirectory(Minecraft.Client/Android/Render)
add_subdirectory(Minecraft.Client/Android/App)
