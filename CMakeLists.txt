cmake_minimum_required(VERSION 3.21)
project(framerot VERSION 0.8.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(FRAMEROT_CODEC_ONLY "Build and run the codec engine tests without OBS" OFF)
option(FRAMEROT_OBS_TESTS "Build Linux OBS rendering tests" ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(RetroFFmpeg)
find_package(Threads REQUIRED)
if(MSVC)
    add_compile_options(/utf-8 /Zc:preprocessor)
endif()
set(framerot_math "")
if(NOT WIN32)
    set(framerot_math m)
endif()
if(NOT FRAMEROT_CODEC_ONLY)
find_package(libobs REQUIRED)

add_library(framerot MODULE
    src/framerot.c
    src/video-obs.c
    src/codec-worker.cpp
    src/video-roundtrip.c src/mpeg4-edit.c src/portable-sync.cpp
)
target_link_libraries(framerot PRIVATE OBS::libobs retro_ffmpeg Threads::Threads ${framerot_math})
set_target_properties(framerot PROPERTIES PREFIX "")
if(APPLE)
    set_target_properties(framerot PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION plugin
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.myrqyry.framerot"
        MACOSX_BUNDLE_BUNDLE_NAME "framerot")
    install(TARGETS framerot LIBRARY DESTINATION .)
else()
    install(TARGETS framerot LIBRARY DESTINATION bin/64bit RUNTIME DESTINATION bin/64bit)
endif()
endif()

enable_testing()
add_executable(test-mosh-compressed tests/test-mosh-compressed.c src/video-roundtrip.c src/mpeg4-edit.c src/portable-sync.cpp)
target_link_libraries(test-mosh-compressed PRIVATE retro_ffmpeg Threads::Threads ${framerot_math})
add_test(NAME mosh_codec_compressed COMMAND test-mosh-compressed)
add_executable(test-mosh-instrument tests/test-mosh-instrument.c src/video-roundtrip.c src/mpeg4-edit.c src/portable-sync.cpp)
target_link_libraries(test-mosh-instrument PRIVATE retro_ffmpeg Threads::Threads ${framerot_math})
add_test(NAME mosh_codec_instrument COMMAND test-mosh-instrument)
add_executable(test-mosh-worker tests/test-mosh-worker.cpp src/codec-worker.cpp src/video-roundtrip.c src/mpeg4-edit.c src/portable-sync.cpp)
target_link_libraries(test-mosh-worker PRIVATE retro_ffmpeg Threads::Threads ${framerot_math})
add_test(NAME mosh_codec_worker COMMAND test-mosh-worker)
add_executable(test-mosh-codec tests/test-mosh-codec.c src/video-roundtrip.c src/mpeg4-edit.c src/portable-sync.cpp)
target_link_libraries(test-mosh-codec PRIVATE retro_ffmpeg Threads::Threads ${framerot_math})
add_test(NAME mosh_codec_packets COMMAND test-mosh-codec)

add_executable(test-mosh-modes tests/test-mosh-modes.c src/video-roundtrip.c src/mpeg4-edit.c src/portable-sync.cpp)
target_link_libraries(test-mosh-modes PRIVATE retro_ffmpeg Threads::Threads ${framerot_math})
add_test(NAME mosh_codec_modes COMMAND test-mosh-modes)

add_executable(test-mosh-vectors tests/test-mosh-vectors.c src/video-roundtrip.c src/mpeg4-edit.c src/portable-sync.cpp)
target_link_libraries(test-mosh-vectors PRIVATE retro_ffmpeg Threads::Threads ${framerot_math})
add_test(NAME mosh_codec_motion_vectors COMMAND test-mosh-vectors)

add_executable(test-mosh-bitstream tests/test-mosh-bitstream.c src/video-roundtrip.c src/mpeg4-edit.c src/portable-sync.cpp)
target_link_libraries(test-mosh-bitstream PRIVATE retro_ffmpeg Threads::Threads ${framerot_math})
add_test(NAME mosh_codec_bitstream_rounding COMMAND test-mosh-bitstream)

add_executable(test-mosh-anchor tests/test-mosh-anchor.c src/video-roundtrip.c src/mpeg4-edit.c src/portable-sync.cpp)
target_link_libraries(test-mosh-anchor PRIVATE retro_ffmpeg Threads::Threads ${framerot_math})
add_test(NAME mosh_codec_keyframe_anchor COMMAND test-mosh-anchor)

add_executable(test-mosh-expansion tests/test-mosh-expansion.c src/video-roundtrip.c src/mpeg4-edit.c src/portable-sync.cpp)
target_link_libraries(test-mosh-expansion PRIVATE retro_ffmpeg Threads::Threads ${framerot_math})
add_test(NAME mosh_codec_rewind_and_peel COMMAND test-mosh-expansion)

if(NOT FRAMEROT_CODEC_ONLY AND FRAMEROT_OBS_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
add_executable(test-mosh-obs tests/test-mosh-obs.cpp)
target_link_libraries(test-mosh-obs PRIVATE OBS::libobs)
add_test(NAME mosh_obs_filter COMMAND test-mosh-obs $<TARGET_FILE:framerot> ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(mosh_obs_filter PROPERTIES TIMEOUT 25)

add_executable(test-mosh-obs-portrait tests/test-mosh-obs.cpp)
target_compile_definitions(test-mosh-obs-portrait PRIVATE MOSH_PORTRAIT=1)
target_link_libraries(test-mosh-obs-portrait PRIVATE OBS::libobs)
add_test(NAME mosh_obs_portrait COMMAND
    test-mosh-obs-portrait $<TARGET_FILE:framerot> ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(mosh_obs_portrait PROPERTIES TIMEOUT 25)

# Catch graphics-state leakage when a filter renders inside a larger scene.
# The old projection overwrite changed unrelated scene elements' positions.
add_executable(test-mosh-geometry tests/test-mosh-geometry.cpp)
target_link_libraries(test-mosh-geometry PRIVATE OBS::libobs)
add_test(NAME mosh_obs_scene_projection COMMAND
    test-mosh-geometry $<TARGET_FILE:framerot> ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(mosh_obs_scene_projection PROPERTIES TIMEOUT 30)

add_executable(test-mosh-geometry-camera tests/test-mosh-geometry.cpp)
target_compile_definitions(test-mosh-geometry-camera PRIVATE MOSH_LIVE_GEOMETRY=1)
target_link_libraries(test-mosh-geometry-camera PRIVATE OBS::libobs)
add_test(NAME mosh_obs_scene_projection_camera COMMAND
    test-mosh-geometry-camera $<TARGET_FILE:framerot> ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(mosh_obs_scene_projection_camera PROPERTIES TIMEOUT 30)

add_executable(test-mosh-mix tests/test-mosh-mix.cpp)
target_link_libraries(test-mosh-mix PRIVATE OBS::libobs)
add_test(NAME mosh_obs_mix_alpha COMMAND test-mosh-mix $<TARGET_FILE:framerot> ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(mosh_obs_mix_alpha PROPERTIES TIMEOUT 25)

# Opt-in documentation capture utility; it loads the actual plugin.
add_executable(capture-showcase EXCLUDE_FROM_ALL tools/capture-showcase.cpp)
target_link_libraries(capture-showcase PRIVATE OBS::libobs Threads::Threads)

add_executable(test-mosh-performance tests/test-mosh-performance.cpp)
target_link_libraries(test-mosh-performance PRIVATE OBS::libobs)
endif()
