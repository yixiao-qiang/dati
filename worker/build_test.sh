#!/bin/bash
# Wave5 S5：编译 worker（worker_tests）+ 跑测试 + 编译主程序
cd ~/work/dati/worker || exit 1
cmake -B build > /tmp/wb1.txt 2>&1; echo "cmake RC=$?" >> /tmp/wb1.txt
cmake --build build --target worker_tests >> /tmp/wb1.txt 2>&1; echo "build_tests RC=$?" >> /tmp/wb1.txt
./build/worker_tests >> /tmp/wb1.txt 2>&1; echo "run_tests RC=$?" >> /tmp/wb1.txt
cmake --build build >> /tmp/wb1.txt 2>&1; echo "build_all RC=$?" >> /tmp/wb1.txt
cat /tmp/wb1.txt
