# Android renderer, OpenGL ES 3.0 on EGL. GLContext has the context, the
# layers and the GL bridge the game code draws through.

add_library(mcle_android_render STATIC
  GLContext.cpp
  GLShaders.cpp
  RND_Android_GL.cpp
  png_decode.cpp
  ${IOS_PLATFORM_DIR}/Render/buffered_image_load.cpp
)

target_include_directories(mcle_android_render PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${IOS_PLATFORM_DIR}/Render
  ${IOS_PLATFORM_DIR}
  ${UPSTREAM_DIR}/Minecraft.Client/Windows64/4JLibs/inc
)

target_include_directories(mcle_android_render PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/stb
)

target_link_libraries(mcle_android_render PUBLIC
  GLESv3
  EGL
  android
  log
)
