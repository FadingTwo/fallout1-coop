# Builds for 64-bit Linux with zig (pip install ziglang) against an old
# glibc (2.31: Ubuntu 20.04, Debian 11 and later), so release builds run on
# older distributions too. libc++ is linked in; SDL2 stays the system's.
#
#   cmake -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
#       -DCMAKE_TOOLCHAIN_FILE=tools/zig-linux/toolchain.cmake \
#       -DSDL2_INCLUDE_DIRS=... -DSDL2_LIBRARIES=.../libSDL2.so
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER ${CMAKE_CURRENT_LIST_DIR}/zcc)
set(CMAKE_CXX_COMPILER ${CMAKE_CURRENT_LIST_DIR}/zcxx)
set(CMAKE_AR ${CMAKE_CURRENT_LIST_DIR}/zar)
set(CMAKE_RANLIB ${CMAKE_CURRENT_LIST_DIR}/zranlib)
