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
)

target_include_directories(sandbox PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 子目录：冒烟测试
add_subdirectory(tests/smoke)
