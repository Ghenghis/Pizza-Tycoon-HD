# Cross-compile for 64-bit Windows from Linux or WSL2 with MinGW-w64:
#   sudo apt-get install g++-mingw-w64-x86-64-posix
#   cmake -B build/win64 -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake ...
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
# Dependencies built by scripts/build-windows-cross.sh live in WIN_DEPS_PREFIX.
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32 $ENV{WIN_DEPS_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Ship without MinGW runtime DLLs.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ -static")
