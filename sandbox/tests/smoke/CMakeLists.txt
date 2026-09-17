# Wave 1 · 冒烟测试构建配置

# 测试 01：hello world 能在沙箱里跑通
add_executable(test_01_hello test_01_hello.cpp)
target_link_libraries(test_01_hello PRIVATE sandbox)
