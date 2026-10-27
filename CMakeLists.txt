cmake_minimum_required(VERSION 3.24)
project(TtpOsu LANGUAGES CXX RC)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "ttp_osu requires MSVC and -A Win32")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(cmake/legacy_windows.cmake)
include(cmake/size_release.cmake)
include(cmake/realm.cmake)
include(cmake/version.cmake)
set(TTP_OSU_LOGO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/assets/TTPlayer.png")
set(TTP_OSU_ORIGINAL_LOGO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/assets/osu-menu-logo.png")
set(TTP_OSU_WELCOME_FILE "${CMAKE_CURRENT_SOURCE_DIR}/assets/osu-welcome-text.png")
set(TTP_OSU_ASSET_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets")
configure_file(src/visual_assets.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/visual_assets.rc" @ONLY)
add_library(ttp_osu SHARED src/plugin.cpp src/parser.cpp src/visual_scene.cpp src/visual_lazer.cpp
  src/library_api.cpp src/library_stable.cpp src/installation.cpp
  src/visual_api.cpp src/visual_renderer.cpp src/ttp_osu.def
  "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/visual_assets.rc")
ttp_osu_attach_realm(ttp_osu)
add_dependencies(ttp_osu ttp_osu_version)
target_include_directories(ttp_osu PUBLIC include PRIVATE src)
target_link_libraries(ttp_osu PRIVATE user32 gdi32 gdiplus ole32 version)
target_compile_definitions(ttp_osu PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0501 _CRT_SECURE_NO_WARNINGS)
target_compile_options(ttp_osu PRIVATE /W4 /permissive- /EHsc /GR- /utf-8 /arch:SSE2)
set_target_properties(ttp_osu PROPERTIES PREFIX "" OUTPUT_NAME "ttp_osu")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(TARGET ttp_osu POST_BUILD COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_legacy_imports.py" "$<TARGET_FILE:ttp_osu>"
  --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/5.1.2600.txt"
  --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/6.1.7600.txt"
  --report "$<TARGET_FILE_DIR:ttp_osu>/legacy-imports.json" VERBATIM)
install(TARGETS ttp_osu RUNTIME DESTINATION AddIn COMPONENT Runtime)
