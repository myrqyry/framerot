cmake_minimum_required(VERSION 3.21)
project(framerot VERSION 0.6.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)

find_package(libobs REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(VIDEO_FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)

add_library(framerot MODULE
    src/framerot.c
    src/video-obs.c
    src/video-roundtrip.c
)
target_link_libraries(framerot PRIVATE OBS::libobs PkgConfig::VIDEO_FFMPEG m)
set_target_properties(framerot PROPERTIES PREFIX "")
install(TARGETS framerot LIBRARY DESTINATION bin/64bit)

enable_testing()
add_executable(test-mosh-codec tests/test-mosh-codec.c src/video-roundtrip.c)
target_link_libraries(test-mosh-codec PRIVATE PkgConfig::VIDEO_FFMPEG m)
add_test(NAME mosh_codec_packets COMMAND test-mosh-codec)

add_executable(test-mosh-modes tests/test-mosh-modes.c src/video-roundtrip.c)
target_link_libraries(test-mosh-modes PRIVATE PkgConfig::VIDEO_FFMPEG m)
add_test(NAME mosh_codec_modes COMMAND test-mosh-modes)

add_executable(test-mosh-vectors tests/test-mosh-vectors.c src/video-roundtrip.c)
target_link_libraries(test-mosh-vectors PRIVATE PkgConfig::VIDEO_FFMPEG m)
add_test(NAME mosh_codec_motion_vectors COMMAND test-mosh-vectors)

add_executable(test-mosh-bitstream tests/test-mosh-bitstream.c src/video-roundtrip.c)
target_link_libraries(test-mosh-bitstream PRIVATE PkgConfig::VIDEO_FFMPEG m)
add_test(NAME mosh_codec_bitstream_rounding COMMAND test-mosh-bitstream)

add_executable(test-mosh-anchor tests/test-mosh-anchor.c src/video-roundtrip.c)
target_link_libraries(test-mosh-anchor PRIVATE PkgConfig::VIDEO_FFMPEG m)
add_test(NAME mosh_codec_keyframe_anchor COMMAND test-mosh-anchor)

add_executable(test-mosh-expansion tests/test-mosh-expansion.c src/video-roundtrip.c)
target_link_libraries(test-mosh-expansion PRIVATE PkgConfig::VIDEO_FFMPEG m)
add_test(NAME mosh_codec_rewind_and_peel COMMAND test-mosh-expansion)

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
