# Cross-compiles for 64-bit Windows with zig (pip install ziglang):
#
#   cmake -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
#       -DCMAKE_TOOLCHAIN_FILE=tools/zig-windows/toolchain.cmake \
#       -DCMAKE_POLICY_VERSION_MINIMUM=3.5
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER ${CMAKE_CURRENT_LIST_DIR}/zcc)
set(CMAKE_CXX_COMPILER ${CMAKE_CURRENT_LIST_DIR}/zcxx)
set(CMAKE_AR ${CMAKE_CURRENT_LIST_DIR}/zar)
set(CMAKE_RANLIB ${CMAKE_CURRENT_LIST_DIR}/zranlib)
set(CMAKE_RC_COMPILER ${CMAKE_CURRENT_LIST_DIR}/zrc)
