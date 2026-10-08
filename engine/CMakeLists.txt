cmake_minimum_required(VERSION 3.22)
# Must be set before project() or CMake ignores it (binaries would require the build Mac's OS).
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
project(VenueTuner VERSION 0.1.0 LANGUAGES C CXX)

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
target_link_libraries(vt_core
  PRIVATE
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
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
catch_discover_tests(vt_tests)
