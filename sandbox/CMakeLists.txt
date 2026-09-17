# Wave 1 · 沙箱核心构建配置
cmake_minimum_required(VERSION 3.16)
project(sandbox CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 沙箱核心库
add_library(sandbox STATIC
    src/cgroup.cpp
    src/rootfs.cpp
    src/runner.cpp
    src/seccomp.cpp
)

target_include_directories(sandbox PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 链接 libseccomp（seccomp-bpf 用户态库）
target_link_libraries(sandbox PUBLIC seccomp)

# 子目录：冒烟测试
add_subdirectory(tests/smoke)
