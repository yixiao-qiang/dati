# Wave 0 C++ 冒烟测试构建脚本
# 用法：
#   cmake -B build -S .
#   cmake --build build
# 产物：build/test_seccomp / build/test_pivot_root / build/test_cgroup_migrate
cmake_minimum_required(VERSION 3.22)
project(wave0_smoke CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- 0.3 seccomp：需要 libseccomp ----
find_library(SECCOMP_LIB seccomp)
find_path(SECCOMP_INC seccomp.h)

add_executable(test_seccomp 03_seccomp_socket.cpp)
if(SECCOMP_LIB AND SECCOMP_INC)
  target_include_directories(test_seccomp PRIVATE "${SECCOMP_INC}")
  target_link_libraries(test_seccomp PRIVATE "${SECCOMP_LIB}")
  message(STATUS "libseccomp: ${SECCOMP_LIB}")
else()
  message(WARNING "未找到 libseccomp —— 请先 apt install libseccomp-dev；test_seccomp 将无法构建")
endif()

# ---- 0.5 pivot_root ----
add_executable(test_pivot_root 05_pivot_root.cpp)

# ---- 0.6 cgroup 迁移 ----
add_executable(test_cgroup_migrate 06_cgroup_migrate.cpp)
