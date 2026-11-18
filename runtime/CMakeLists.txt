cmake_minimum_required(VERSION 3.20)
project(TCSCCRecomp VERSION 0.1.11 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(SCDA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(XENON_ROOT "${SCDA_ROOT}/external/XenonRecomp")
set(GENERATED "${SCDA_ROOT}/generated")

# SHA-256 of the TU3 flat image generated/ was recompiled from (tools/xex_dump).
file(STRINGS "${SCDA_ROOT}/config/expected_image.txt" SCDA_EXPECTED_IMAGE_SHA256 LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SCDA_ROOT}/config/expected_image.txt")

# Flags match UnleashedRecomp's recompiled library.
add_compile_options(/clang:-march=sandybridge -fno-strict-aliasing /fp:strict /Zi)
# PDBs let the crash handler map a host fault to the generated line (its comment is the PPC instruction).
add_link_options(/DEBUG)

# XMA decoder (src/xma_decoder.h): FFmpeg when external/ffmpeg/build is configured, else none
# (XMA contexts stay undecoded: silent).
set(FFMPEG_BUILD "${SCDA_ROOT}/external/ffmpeg/build")
if(EXISTS "${FFMPEG_BUILD}/config.h")
    include("${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg_xma.cmake")
else()
    add_library(scda_xma STATIC src/xma_null.c)
endif()

# This build's version (project VERSION; the in-game updater compares it with the latest GitLab release
# of SCDA_UPDATE_REPO; tools/installer/build.bat reads the same number). Release tag = v<VERSION>.
set(SCDA_UPDATE_REPO "octagram-labs-group/TCSCC-Recomp" CACHE STRING "GitLab group/project the in-game updater checks" FORCE)
set(SCDA_UPDATE_FORGE_REPO "8gramlabs/TCSCC-Recomp" CACHE STRING "git.8gramlabs.com owner/name the in-game updater also checks (empty = none)")

# SDL3 (zlib): native PlayStation / Switch / other gamepads (src/pad.cpp), linked static so an
# update that replaces scda.exe alone carries it. Source: external/SDL (github.com/libsdl-org/SDL
# tag release-3.4.18, unmodified; git clone --depth 1 --branch release-3.4.18
# https://github.com/libsdl-org/SDL external/SDL). Missing: XInput pads only.
set(SDL_DIR "${SCDA_ROOT}/external/SDL")
if(EXISTS "${SDL_DIR}/CMakeLists.txt")
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    foreach(off SDL_AUDIO SDL_VIDEO SDL_GPU SDL_RENDER SDL_CAMERA SDL_POWER SDL_DIALOG SDL_TRAY
            SDL_TESTS SDL_TEST_LIBRARY SDL_EXAMPLES SDL_INSTALL SDL_UNINSTALL)
        set(${off} OFF CACHE BOOL "" FORCE)
    endforeach()
    add_subdirectory("${SDL_DIR}" "${CMAKE_BINARY_DIR}/SDL" EXCLUDE_FROM_ALL)
    add_library(scda_sdl INTERFACE)
    target_link_libraries(scda_sdl INTERFACE SDL3::SDL3-static)
    target_compile_definitions(scda_sdl INTERFACE SCDA_HAVE_SDL3)
else()
    add_library(scda_sdl INTERFACE)
endif()

# Dear ImGui (MIT, vendored unmodified in thirdparty/imgui): the F1 settings menu.
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui")
add_library(scda_imgui STATIC "${IMGUI_DIR}/imgui.cpp" "${IMGUI_DIR}/imgui_draw.cpp" "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp" "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp" "${IMGUI_DIR}/backends/imgui_impl_win32.cpp")
target_include_directories(scda_imgui PUBLIC "${IMGUI_DIR}")
target_compile_options(scda_imgui PRIVATE -w)
# Its XInput-only gamepad feed is replaced by overlay.cpp's pad.h feed (PlayStation pads too).
target_compile_definitions(scda_imgui PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

# Recompiled PPC code (XenonRecomp output).
file(GLOB SCDA_PPC_SOURCES CONFIGURE_DEPENDS "${GENERATED}/ppc_recomp.*.cpp")
add_library(scda_ppc STATIC ${SCDA_PPC_SOURCES} "${GENERATED}/ppc_func_mapping.cpp")
target_include_directories(scda_ppc PUBLIC "${GENERATED}" "${XENON_ROOT}/thirdparty/simde")
target_precompile_headers(scda_ppc PRIVATE "${GENERATED}/ppc_recomp_shared.h")
target_compile_options(scda_ppc PRIVATE -Wno-unused-label -Wno-unused-variable)
target_compile_definitions(scda_ppc PRIVATE PPC_GUEST_TIMEBASE) # mftb = 50 MHz guest timebase (ppc_context.h)

add_executable(scda
    src/main.cpp
    src/title.cpp
    src/loader.cpp
    src/watchdog.cpp
    src/import_stub.cpp
    src/video.cpp
    src/pm4.cpp
    src/command_processor.cpp
    src/interrupts.cpp
    src/audio.cpp
    src/xma.cpp
    src/memory.cpp
    src/module.cpp
    src/thread_context.cpp
    src/xam.cpp
    src/content.cpp
    src/input.cpp
    src/pad.cpp
    src/keybinds.cpp
    src/prompts.cpp
    src/fov.cpp
    src/profiler.cpp
    src/timeline.cpp
    src/launch.cpp
    src/guest_time.cpp
    src/gpu/shader_cache.cpp
    src/gpu/draw.cpp
    src/gpu/draw_state.cpp
    src/gpu/smaa.cpp
    src/gpu/nis.cpp
    src/gpu/d3d11_renderer.cpp
    src/gpu/xenos/shader_recompiler.cpp
    src/gpu/xenos/shader_recompiler_raw.cpp
    src/gpu/xenos/translate.cpp
    src/gpu/shader_compiler.cpp
    src/settings.cpp
    src/host_window.cpp
    src/overlay.cpp
    src/updater.cpp
    src/imports.cpp
    src/fs.cpp
    src/kernel.cpp
    src/gfx_log.cpp
    src/camera.cpp
    src/lod.cpp
    src/l3d_trace.cpp
    src/pawn_trace.cpp
    src/pawn_hooks.cpp
    src/main_menu.cpp
    src/recomp_page.cpp
    src/player_card.cpp
    src/online/net_transport.cpp
    src/online/steam_net.cpp
    src/online/steam_bridge.cpp
    src/online/net_selftest.cpp
    src/online/xnet.cpp
    src/online/xsocket.cpp
    src/online/matchmaking.cpp
    src/online/xsession.cpp
    src/online_identity.cpp
    src/xma_xaudio.cpp
    src/net_sync_trace.cpp
    src/wait_trace.cpp
    src/imports_stubs.cpp
    scda.rc)
# SCDA guest hooks (menu_hooks, travel_trace, audio_trace) are not built (xma_xaudio, wait_trace are re-found for Conviction):
# they override SC4 guest addresses, which are unrelated code in Conviction. Re-find before re-adding.
set_target_properties(scda PROPERTIES OUTPUT_NAME tcscc)
target_include_directories(scda PRIVATE include src)
target_link_libraries(scda PRIVATE scda_ppc scda_imgui scda_sdl gdiplus winhttp shell32 bcrypt dbghelp winmm d3d11 dxgi d3dcompiler xinput synchronization xaudio2 scda_xma)
# Recompiled code runs on the host stack; the main guest thread recurses deeply.
target_link_options(scda PRIVATE /STACK:0x4000000)
# GUI app (no console window at launch); main() stays the entry. Logs go to tcscc.log.
target_link_options(scda PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
target_compile_definitions(scda PRIVATE SCDA_EXPECTED_IMAGE_SHA256="${SCDA_EXPECTED_IMAGE_SHA256}" NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

# Steam online (src/online/, ported from the TTT2 port): built when the Steamworks SDK sits in
# external/steamworks_sdk_165 (user-supplied, git-ignored). steam_api64.dll is delay-loaded and copied
# next to the executable; without it (or without a running Steam client) online uses the loopback
# transport (two instances on one PC).
set(SCDA_STEAMWORKS "${SCDA_ROOT}/external/steamworks_sdk_165/sdk" CACHE PATH "Steamworks SDK (sdk folder)")
if(EXISTS "${SCDA_STEAMWORKS}/public/steam/steam_api.h" AND EXISTS "${SCDA_STEAMWORKS}/redistributable_bin/win64/steam_api64.lib")
    target_include_directories(scda PRIVATE "${SCDA_STEAMWORKS}/public")
    target_compile_definitions(scda PRIVATE SCDA_HAVE_STEAM=1)
    target_link_libraries(scda PRIVATE "${SCDA_STEAMWORKS}/redistributable_bin/win64/steam_api64.lib" delayimp ws2_32)
    target_link_options(scda PRIVATE "/DELAYLOAD:steam_api64.dll")
    add_custom_command(TARGET scda POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SCDA_STEAMWORKS}/redistributable_bin/win64/steam_api64.dll"
                "$<TARGET_FILE_DIR:scda>/steam_api64.dll")
    message(STATUS "Steamworks SDK: ${SCDA_STEAMWORKS} (Steam online enabled)")
else()
    target_compile_definitions(scda PRIVATE SCDA_HAVE_STEAM=0)
    target_link_libraries(scda PRIVATE ws2_32)
    message(STATUS "Steamworks SDK not found - Steam online disabled (loopback only)")
endif()

# Tests: ctest --test-dir runtime/build
enable_testing()
add_executable(scda_tests tests/test_main.cpp tests/test_loader.cpp tests/test_watchdog.cpp tests/test_import_stub.cpp tests/test_video.cpp tests/test_pm4.cpp tests/test_command_processor.cpp tests/test_interrupts.cpp tests/test_fs.cpp tests/test_audio.cpp tests/test_xma.cpp tests/xma_fake_decoder.c tests/test_alloc_tracker.cpp tests/test_memory.cpp tests/test_module.cpp tests/test_thread_context.cpp tests/test_xam.cpp tests/test_time.cpp tests/test_shader_cache.cpp tests/test_draw.cpp tests/test_frame_queue.cpp tests/test_settings.cpp tests/test_d3d11_smoke.cpp tests/test_window_size.cpp tests/test_xenos_container.cpp tests/test_xenos_raw.cpp tests/test_shader_compiler.cpp tests/test_draw_state.cpp tests/test_input.cpp tests/test_prompts.cpp tests/test_launch.cpp tests/test_content.cpp tests/test_updater.cpp src/overlay.cpp src/recomp_page.cpp src/online_identity.cpp src/updater.cpp src/content.cpp src/input.cpp src/pawn_trace.cpp src/pad.cpp src/keybinds.cpp src/prompts.cpp src/fov.cpp src/launch.cpp src/gpu/draw_state.cpp src/gpu/shader_compiler.cpp src/gpu/xenos/shader_recompiler.cpp src/gpu/xenos/shader_recompiler_raw.cpp src/gpu/xenos/translate.cpp src/settings.cpp src/host_window.cpp src/gpu/d3d11_renderer.cpp src/gpu/smaa.cpp src/gpu/nis.cpp src/gpu/shader_cache.cpp src/gpu/draw.cpp src/guest_time.cpp src/thread_context.cpp src/xam.cpp src/kernel.cpp src/memory.cpp src/module.cpp src/fs.cpp src/audio.cpp src/xma.cpp src/interrupts.cpp src/video.cpp src/pm4.cpp src/command_processor.cpp src/loader.cpp src/watchdog.cpp src/import_stub.cpp src/title.cpp src/timeline.cpp "${SCDA_ROOT}/tools/stfs/stfs.cpp")
target_include_directories(scda_tests PRIVATE tests include src "${SCDA_ROOT}/tools/stfs" "${GENERATED}" "${XENON_ROOT}/thirdparty/simde")
target_compile_options(scda_tests PRIVATE /clang:-march=sandybridge)
target_link_libraries(scda_tests PRIVATE scda_imgui scda_sdl gdiplus winhttp shell32 bcrypt winmm d3d11 dxgi d3dcompiler xinput synchronization xaudio2)
target_compile_definitions(scda_tests PRIVATE SCDA_EXPECTED_IMAGE_SHA256="${SCDA_EXPECTED_IMAGE_SHA256}" SCDA_TESTS SCDA_ROOT="${SCDA_ROOT}" NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
add_test(NAME scda_tests COMMAND scda_tests)

# XenosRecomp fork (MIT): fmt header-only from XenonRecomp's thirdparty.
foreach(t scda scda_tests)
    target_include_directories(${t} PRIVATE "${XENON_ROOT}/thirdparty/fmt/include")
    target_compile_definitions(${t} PRIVATE FMT_HEADER_ONLY SCDA_VERSION="${PROJECT_VERSION}" SCDA_UPDATE_REPO="${SCDA_UPDATE_REPO}" SCDA_UPDATE_FORGE_REPO="${SCDA_UPDATE_FORGE_REPO}")
endforeach()
