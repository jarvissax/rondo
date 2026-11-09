# Rondo's native code, built for the Y1 (Android 4.2.2, armeabi-v7a) with NDK r23c: tools/native/build.sh
cmake_minimum_required(VERSION 3.18)
project(rondo_native C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-fno-exceptions -fno-rtti -Wall -Wextra -Wno-unused-parameter)

# The Y1's own libraries and the AOSP 4.2.2 headers that describe them (tools/native/sysroot.sh)
set(Y1_SYSROOT ${CMAKE_CURRENT_SOURCE_DIR}/sysroot)
set(Y1_LIB ${Y1_SYSROOT}/system-lib)
foreach(lib gui ui binder utils cutils log hardware media stagefright stagefright_foundation surfaceflinger hardware_legacy netutils fmjni mtkbtextadpa2dp)
  add_library(y1_${lib} SHARED IMPORTED)
  set_target_properties(y1_${lib} PROPERTIES IMPORTED_LOCATION ${Y1_LIB}/lib${lib}.so)
endforeach()

# A target that talks to Android's private native libraries: the headers, the configuration those
# libraries were built with, and the firmware's libraries to link and resolve against.
function(y1_private target)
  target_include_directories(${target} SYSTEM PRIVATE
    ${Y1_SYSROOT}/aosp/native ${Y1_SYSROOT}/aosp/core ${Y1_SYSROOT}/aosp/hardware ${Y1_SYSROOT}/aosp/av
    ${Y1_SYSROOT}/aosp/hardware_legacy)
  target_compile_options(${target} PRIVATE -include ${Y1_SYSROOT}/aosp/config/AndroidConfig.h
    -Wno-deprecated-register -Wno-unused-private-field -Wno-overloaded-virtual)
  target_link_options(${target} PRIVATE -Wl,-rpath-link,${Y1_LIB})
endfunction()

add_subdirectory(tests)

# Tools that run on the Y1 (tools/perf drives them)
add_executable(inject tools/inject.cpp)

# Third-party libraries, built for the Y1 by tools/native/deps.sh
set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party/y1)
foreach(lib freetype harfbuzz png16 turbojpeg sqlite3 yyjson)
  add_library(tp_${lib} STATIC IMPORTED)
  set_target_properties(tp_${lib} PROPERTIES IMPORTED_LOCATION ${TP}/lib/lib${lib}.a)
endforeach()

# The core (N2): settings, the card library, later the server client
add_library(rondo_core STATIC core/config.cpp core/fold.cpp core/tags.cpp core/library.cpp core/playlists.cpp core/lyrics.cpp core/prefs.cpp core/sound.cpp)
target_include_directories(rondo_core SYSTEM PUBLIC ${TP}/include)
target_link_libraries(rondo_core PUBLIC tp_sqlite3 tp_yyjson dl)

add_executable(libtest apps/libtest/main.cpp)
target_link_libraries(libtest PRIVATE rondo_core)

# The UI engine (N1): portable C++ over GLES2
add_library(rondo_ui STATIC ui/gfx.cpp ui/text.cpp ui/image.cpp ui/frame_log.cpp ui/theme.cpp ui/keyboard.cpp ui/icons.cpp ui/covers.cpp ui/qr.cpp core/cover.cpp)
target_include_directories(rondo_ui SYSTEM PUBLIC ${TP}/include ${TP}/include/freetype2 ${TP}/include/harfbuzz)
target_link_libraries(rondo_ui PUBLIC rondo_core tp_harfbuzz tp_freetype tp_png16 tp_turbojpeg z GLESv2 m)

# The Y1's platform layer: framebuffer, vsync, input
add_library(rondo_platform_y1 STATIC platform/y1/display.cpp platform/y1/input.cpp platform/y1/usb_drive.cpp platform/y1/video_surface.cpp platform/y1/power.cpp platform/y1/video_frame.cpp platform/y1/haptics.cpp)
y1_private(rondo_platform_y1)
target_link_libraries(rondo_platform_y1 PUBLIC rondo_ui y1_gui y1_ui y1_media y1_binder y1_utils y1_cutils EGL GLESv2 dl)

add_executable(uitest apps/uitest/main.cpp)
target_link_libraries(uitest PRIVATE rondo_ui rondo_platform_y1)
target_link_options(uitest PRIVATE -Wl,-rpath-link,${Y1_LIB})
add_executable(churn tools/churn.cpp)

# The server client (N2): libcurl over OpenSSL and nghttp2, trusting the roots 1.0.x bundles
if(EXISTS ${TP}/lib/libcurl.a)
  file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../app/app/src/main/res/raw/isrg_root_x1.pem ROOT_X1)
  file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../app/app/src/main/res/raw/isrg_root_yr.pem ROOT_YR)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/roots.cpp
    "namespace rondo { extern const char kRootCerts[] = R\"PEM(${ROOT_X1}\n${ROOT_YR})PEM\"; }\n")
  foreach(lib curl nghttp2 ssl crypto)
    add_library(tp_${lib} STATIC IMPORTED)
    set_target_properties(tp_${lib} PROPERTIES IMPORTED_LOCATION ${TP}/lib/lib${lib}.a)
  endforeach()
  add_library(rondo_net STATIC core/api.cpp core/net.cpp core/bundle.cpp ${CMAKE_CURRENT_BINARY_DIR}/roots.cpp)
  target_compile_definitions(rondo_net PUBLIC CURL_STATICLIB)
  target_link_libraries(rondo_net PUBLIC rondo_core tp_curl tp_nghttp2 tp_ssl tp_crypto z)
  target_link_libraries(rondo_ui PUBLIC rondo_net)  # rondo-media fetches the server's artwork
  add_executable(apitest apps/apitest/main.cpp)
  target_link_libraries(apitest PRIVATE rondo_net)
endif()

# Rondo 2.0's shell: 1.0.x's screens, ported 1:1 (native/shell, native/apps/rondo)
add_library(rondo_shell STATIC shell/kit.cpp shell/library_screens.cpp shell/main_menu.cpp shell/now_playing.cpp shell/lyrics_view.cpp shell/music_list.cpp shell/playlist_screens.cpp shell/settings_screens.cpp shell/usb_screen.cpp shell/wifi_screens.cpp shell/music_screen.cpp shell/streams.cpp shell/music_screens.cpp shell/video_screen.cpp shell/video_player.cpp shell/shorts_screen.cpp shell/bluetooth_screens.cpp shell/fm_screens.cpp shell/power_screens.cpp shell/server_screens.cpp shell/dashboard_server.cpp shell/dashboard_screen.cpp shell/main_queue.cpp shell/sound_screens.cpp shell/resume.cpp shell/update_screen.cpp shell/safety_screens.cpp)
target_link_libraries(rondo_shell PUBLIC rondo_ui)
add_executable(rondo apps/rondo/main.cpp)
target_link_libraries(rondo PRIVATE rondo_shell rondo_platform_y1)
target_link_options(rondo PRIVATE -Wl,-rpath-link,${Y1_LIB})

# The player (N3): rondo-player, FFmpeg's decoders (LGPL, shared) to OpenSL ES
foreach(lib avformat avcodec avutil swresample)
  add_library(ff_${lib} SHARED IMPORTED)
  set_target_properties(ff_${lib} PROPERTIES IMPORTED_LOCATION ${TP}/lib/lib${lib}.so)
endforeach()
add_library(rondo_player STATIC player/engine.cpp player/host.cpp player/http_source.cpp player/video.cpp player/fm.cpp player/click.cpp player/dsp.cpp player/loudness.cpp player/amplifier.cpp)
y1_private(rondo_player)
target_include_directories(rondo_player SYSTEM PRIVATE ${TP}/include)
target_link_libraries(rondo_player PUBLIC ff_avformat ff_avcodec ff_swresample ff_avutil OpenSLES y1_fmjni y1_media y1_stagefright y1_stagefright_foundation y1_gui y1_binder y1_utils rondo_net)
target_link_libraries(rondo PRIVATE rondo_player)

# rondo-core: the system services (services/core.h), forked at startup like rondo-player
add_library(rondo_services STATIC services/host.cpp services/core_main.cpp services/wifi.cpp services/bluetooth.cpp services/clock.cpp)
y1_private(rondo_services)
target_include_directories(rondo_services SYSTEM PRIVATE ${TP}/include)
target_link_libraries(rondo_services PUBLIC rondo_net y1_hardware_legacy y1_netutils y1_cutils tp_sqlite3 y1_mtkbtextadpa2dp y1_media y1_binder y1_utils)
target_link_libraries(rondo PRIVATE rondo_services)
