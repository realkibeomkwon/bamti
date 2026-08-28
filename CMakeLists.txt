cmake_minimum_required(VERSION 3.25)
project(bamti LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(bamti WIN32
  src/main.cpp
  src/host.cpp
  src/paths.cpp
  src/log.cpp
  src/watchdog.cpp
  src/menu_bar.cpp
  src/clock_renderer.cpp
  src/theme.cpp
  src/pipe_server.cpp
  src/taskbar_controller.cpp
  src/fullscreen.cpp
  src/task_list.cpp
  src/dock.cpp
  src/popup_surface.cpp
  src/start_menu.cpp
  src/spotlight.cpp
)

target_include_directories(bamti PRIVATE src)

target_compile_definitions(bamti PRIVATE
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  STRICT
)

target_compile_options(bamti PRIVATE
  /utf-8
  /W4
  /permissive-
  /EHsc
  /sdl
  $<$<CONFIG:Release>:/O2>
  $<$<CONFIG:Release>:/Oi>
  $<$<CONFIG:Release>:/Gy>
  $<$<CONFIG:Release>:/Gw>
  $<$<CONFIG:Release>:/GL>
)

target_link_options(bamti PRIVATE
  $<$<CONFIG:Release>:/LTCG>
  $<$<CONFIG:Release>:/OPT:REF>
  $<$<CONFIG:Release>:/OPT:ICF>
  /DELAYLOAD:powrprof.dll
)

target_link_libraries(bamti PRIVATE
  dwmapi
  d2d1
  dwrite
  shell32
  user32
  gdi32
  uxtheme
  ole32
  advapi32
  shcore
  comctl32
  propsys
  uuid
  msimg32
  windowscodecs
  powrprof
  imm32
  shlwapi
  winmm
  delayimp
)

set_property(TARGET bamti PROPERTY VS_DPI_AWARE "PerMonitor")
if(MSVC)
  set_property(TARGET bamti PROPERTY VS_GLOBAL_ApplicationManifest "${CMAKE_CURRENT_SOURCE_DIR}/src/bamti.manifest")
endif()
