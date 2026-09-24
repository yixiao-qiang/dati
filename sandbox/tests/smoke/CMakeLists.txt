# Wave 1 · 冒烟测试构建配置

# 测试 00：状态枚举与默认值（纯逻辑，不需要 root）
add_executable(test_00_status test_00_status.cpp)
target_link_libraries(test_00_status PRIVATE sandbox)

# 测试 01：hello world 能在沙箱里跑通
add_executable(test_01_hello test_01_hello.cpp)
target_link_libraries(test_01_hello PRIVATE sandbox)

# 测试 02：seccomp 运行期白名单拦截 socket()
add_executable(test_02_seccomp test_02_seccomp.cpp)
target_link_libraries(test_02_seccomp PRIVATE sandbox)

# 测试 03：文件系统隔离（/etc/passwd 不可见）
add_executable(test_03_fs_isolation test_03_fs_isolation.cpp)
target_link_libraries(test_03_fs_isolation PRIVATE sandbox)

# 测试 04：MLE（内存超限被 OOM Kill）
add_executable(test_04_mle test_04_mle.cpp)
target_link_libraries(test_04_mle PRIVATE sandbox)

# 测试 05：TLE（死循环超时）
add_executable(test_05_tle test_05_tle.cpp)
target_link_libraries(test_05_tle PRIVATE sandbox)

# 测试 06：OLE（输出超限）
add_executable(test_06_ole test_06_ole.cpp)
target_link_libraries(test_06_ole PRIVATE sandbox)

# 测试 07：OLE 的 stderr 路径（只写 stderr 也须判 OLE）
add_executable(test_07_ole_stderr test_07_ole_stderr.cpp)
target_link_libraries(test_07_ole_stderr PRIVATE sandbox)

# 测试 08：网络隔离（沙箱内不能联网）
add_executable(test_08_net_ test_08_net_.cpp)
target_link_libraries(test_08_net_ PRIVATE sandbox)
