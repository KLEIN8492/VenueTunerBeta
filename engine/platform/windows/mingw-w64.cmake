# CMake toolchain: cross-compile for Windows x64 with MinGW-w64 (posix threads, for std::thread).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# One self-contained .exe: no libstdc++/libgcc/winpthread DLLs to ship.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
# JUCE 8 relies on <cstring> arriving transitively on Windows (true with MSVC, not with GCC 13).
set(CMAKE_CXX_FLAGS_INIT "-include cstring -include intrin.h")
# Case shims (JUCE includes e.g. <Dbghelp.h>; MinGW has <dbghelp.h> and Linux is case-sensitive).
include_directories(BEFORE SYSTEM "${CMAKE_CURRENT_LIST_DIR}/mingw-shims")
add_compile_definitions(JUCE_USE_WINDOWS_MEDIA_FORMAT=0)
