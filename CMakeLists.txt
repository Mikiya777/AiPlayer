cmake_minimum_required(VERSION 3.22)

project(AiPlayer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Keep media processing and the desktop application in separate targets.
add_subdirectory(core)
# 核心练习可独立构建；现有 Qt 应用仍保留默认构建行为。
option(AIPLAYER_BUILD_QT "Build the Qt application" ON)
option(AIPLAYER_BUILD_GL_TESTS "Build local OpenGL regression tests" OFF)
option(AIPLAYER_BUILD_PLAYBACK_EXAMPLE "Build local audio playback example" OFF)

if(AIPLAYER_BUILD_QT OR AIPLAYER_BUILD_PLAYBACK_EXAMPLE)
    find_package(Qt6 6.5 CONFIG REQUIRED COMPONENTS Core Multimedia)
    find_package(ffmpeg CONFIG REQUIRED)
    add_library(aiplayer_audio_output STATIC qt/src/audio/QtAudioOutput.cpp)
    target_include_directories(aiplayer_audio_output PUBLIC core/src)
    target_link_libraries(aiplayer_audio_output PUBLIC aiplayer_core ffmpeg::ffmpeg
                         PRIVATE Qt6::Core Qt6::Multimedia)
endif()

if(AIPLAYER_BUILD_QT OR AIPLAYER_BUILD_GL_TESTS)
    find_package(Qt6 6.5 CONFIG REQUIRED COMPONENTS Gui OpenGL)
    find_package(ffmpeg CONFIG REQUIRED)
    find_package(inspireface CONFIG REQUIRED)
    add_library(aiplayer_video_output STATIC core/src/video/VideoRenderer.cpp core/src/effects/FaceStickerFilter.cpp)
    target_include_directories(aiplayer_video_output PUBLIC core/src)
    target_link_libraries(aiplayer_video_output PUBLIC aiplayer_core ffmpeg::ffmpeg Qt6::Gui Qt6::OpenGL inspireface::inspireface)
endif()

if(AIPLAYER_BUILD_QT)
    add_subdirectory(qt)
endif()

# 本地保留测试，公开源码无需携带测试文件即可构建。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/LocalTargets.cmake")
    include(tests/LocalTargets.cmake)
endif()
