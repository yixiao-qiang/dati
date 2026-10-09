# Wave 2 · 可靠队列 Worker 构建配置
#
# 职责：消费 Redis Stream 判题任务 → MySQL 条件更新幂等 → judge_main 判题 → 更新终态 → XACK
# 技术栈：hiredis（Redis C 客户端）+ mysqlclient（MySQL C API）
# 注意：不链接 sandbox 库——判题走 judge_main 子进程（崩溃隔离 + 权限隔离，架构决策 3）
#
# 构建方式（VM）：
#   cd ~/work/dati/worker && cmake -B build && cmake --build build
#
# 运行：
#   JUDGE_MAIN=$PWD/../sandbox/tests/smoke/build/judge_main ./build/judge_worker
#   （JUDGE_MAIN 指向判题入口，与 server 的约定一致；未配置则 worker 启动即拒绝）
cmake_minimum_required(VERSION 3.16)
project(judge_worker CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 可执行目标：judge_worker
# 四个模块分工（台阶 1）：
#   queue.cpp  - Redis Stream 封装（XADD 生产 / XREADGROUP 消费 / XACK）
#   db.cpp     - MySQL 封装（条件更新幂等 / 状态读写）
#   judge.cpp  - 判题流水线（沙箱外编译 + 子进程调 judge_main）
#   main.cpp   - worker 主循环（连接 → 循环消费 → 幂等判断 → 判题 → 更新 → ACK）
add_executable(judge_worker
    src/main.cpp
    src/queue.cpp
    src/db.cpp
    src/judge.cpp
)

# 链接库：
#   hiredis     - Redis C 客户端（头文件 <hiredis/hiredis.h>，系统默认 include 路径）
#   mysqlclient - MySQL C API（头文件 <mysql/mysql.h>，系统默认 include 路径）
#   两者均为 apt 包：libhiredis-dev / libmysqlclient-dev
target_link_libraries(judge_worker PRIVATE hiredis mysqlclient)
