# Wave 1 · 冒烟测试构建配置

# 测试 01：hello world 能在沙箱里跑通
add_executable(test_01_hello test_01_hello.cpp)
target_link_libraries(test_01_hello PRIVATE sandbox)

# 测试 02：seccomp 运行期白名单拦截 socket()
add_executable(test_02_seccomp test_02_seccomp.cpp)
target_link_libraries(test_02_seccomp PRIVATE sandbox)
