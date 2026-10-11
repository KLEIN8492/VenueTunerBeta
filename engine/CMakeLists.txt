cmake_minimum_required(VERSION 3.22)
# Must be set before project() or CMake ignores it (binaries would require the build Mac's OS).
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
project(VenueTuner VERSION 0.1.0 LANGUAGES C CXX)
# Windows: link the C++ runtime statically so the engine needs no Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Same output path with Visual Studio (multi-config) as with Ninja/Make: build-*/VenueTunerEngine(.exe)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.4
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# IXWebSocket declares an old cmake_minimum_required; CMake 4 needs this floor.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(USE_TLS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG v11.4.5
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(ixwebsocket)

FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Catch2)

# ---- core library: everything except main(), shared by the engine and tests ----
file(GLOB_RECURSE VT_CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM VT_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
add_library(vt_core STATIC ${VT_CORE_SOURCES})

target_include_directories(vt_core PUBLIC src)
target_compile_definitions(vt_core PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_STANDALONE_APPLICATION=1
  JUCE_MODAL_LOOPS_PERMITTED=1
  JUCE_DISPLAY_SPLASH_SCREEN=0)

# Audio back-ends per platform. macOS: CoreAudio. Windows: WASAPI (built in) and ASIO when the
# Steinberg ASIO SDK is given (-DVT_ASIO_SDK_DIR=...; most pro interfaces ship an ASIO driver).
# Linux: ALSA and JACK (JACK is loaded at run time, so it is optional on the target machine).
if(WIN32)
  target_compile_definitions(vt_core PUBLIC JUCE_WASAPI=1 JUCE_DIRECTSOUND=0 NOMINMAX)
  set(VT_ASIO_SDK_DIR "" CACHE PATH "Steinberg ASIO SDK folder (optional)")
  if(VT_ASIO_SDK_DIR)
    target_compile_definitions(vt_core PUBLIC JUCE_ASIO=1)
    target_include_directories(vt_core PUBLIC "${VT_ASIO_SDK_DIR}/common")
    message(STATUS "VenueTuner: ASIO enabled (${VT_ASIO_SDK_DIR})")
  endif()
  if(MINGW) # MSVC picks these up from JUCE's #pragma comment(lib); MinGW needs them spelled out
    target_link_libraries(vt_core PUBLIC winmm ksuser uuid ole32 oleaut32 ws2_32 version shlwapi
                                         dbghelp wininet rpcrt4 imm32 comctl32 setupapi avrt)
  endif()
elseif(UNIX AND NOT APPLE)
  target_compile_definitions(vt_core PUBLIC JUCE_ALSA=1 JUCE_JACK=1)
endif()
# Exact float comparisons in this code are intentional (identity/pass-through filters, change
# detection of parameters), so -Wfloat-equal from JUCE's strict warning set is only noise.
# Linked after JUCE's warning flags so it comes later on the command line and wins.
add_library(vt_warning_tweaks INTERFACE)
if(MSVC)
  target_compile_options(vt_warning_tweaks INTERFACE /wd4100 /wd4244 /wd4267 /bigobj)
else()
  target_compile_options(vt_warning_tweaks INTERFACE -Wno-float-equal)
endif()

target_link_libraries(vt_core
  PRIVATE
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    vt_warning_tweaks
  PUBLIC
    juce::juce_core
    juce::juce_events
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_dsp
    juce::juce_audio_formats
    ixwebsocket)


# JUCE modules are compiled into vt_core; expose their headers/defines to dependents.
target_include_directories(vt_core INTERFACE
  $<TARGET_PROPERTY:vt_core,INCLUDE_DIRECTORIES>)
target_compile_definitions(vt_core INTERFACE
  $<TARGET_PROPERTY:vt_core,COMPILE_DEFINITIONS>)

add_executable(VenueTunerEngine src/main.cpp)
target_link_libraries(VenueTunerEngine PRIVATE vt_core)

# ---- tests ----
enable_testing()
file(GLOB VT_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
add_executable(vt_tests ${VT_TEST_SOURCES})
target_link_libraries(vt_tests PRIVATE vt_core Catch2::Catch2WithMain)
include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
if(NOT CMAKE_CROSSCOMPILING) # discovery runs the test binary, impossible when cross-compiling
  catch_discover_tests(vt_tests)
endif()
