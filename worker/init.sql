-- Wave 2 · 判题系统数据库初始化
-- 用法：sudo mysql < init.sql
-- 设计决策（面试点）：
--   1. utf8mb4：用户代码可能含 emoji（4 字节），utf8（3 字节）存不下会报错
--   2. status 索引：worker 的幂等条件更新 WHERE status='pending' 高频命中
--   3. judge_started_at + retry_count：rescue 扫描"卡 10 分钟 + 最多救 3 次"的依据
--   4. 专用账号 oj（最小权限）：worker 不用 root——root 是 auth_socket 认证，
--      只能本机 root 用户经 sudo 登录；且生产上 worker 不应持有全库权限

CREATE DATABASE IF NOT EXISTS oj
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

USE oj;

-- 题目表（Wave 3 台阶4-3：限额从 DB 读取，替代 worker 硬编码 1000ms/256MB/64MB）
-- 限额属于题目：同一道题限额固定；submission.problem_id 外键记录"做的哪道题"
CREATE TABLE IF NOT EXISTS problem (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    title VARCHAR(128) NOT NULL,                 -- 题目标题
    time_limit_ms INT NOT NULL DEFAULT 1000,     -- 时间限额（毫秒）
    mem_limit_mb INT NOT NULL DEFAULT 256,       -- 内存限额（MB）
    output_limit_mb INT NOT NULL DEFAULT 64      -- 输出限额（MB）
) ENGINE=InnoDB;

-- 学习期默认题：无题目管理页面，提交默认挂 id=1
INSERT IGNORE INTO problem (id, title, time_limit_ms, mem_limit_mb, output_limit_mb)
VALUES (1, 'A+B', 1000, 256, 64);

-- 判题提交表：一条记录 = 用户一次提交，从 pending 走到某个终态
CREATE TABLE IF NOT EXISTS submission (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,        -- 提交 ID（Redis 消息载荷）
    problem_id BIGINT NOT NULL DEFAULT 1,        -- 关联题目（限额从 problem 表读）
    code MEDIUMTEXT NOT NULL,                    -- 用户源码（server 限制 64KB，MEDIUMTEXT 16MB 富余）
    language VARCHAR(16) NOT NULL DEFAULT 'cpp', -- 语言（白名单 cpp/c）
    status VARCHAR(20) NOT NULL DEFAULT 'pending', -- 状态机当前位置（pending/judging/终态/dead 死信）
    judge_started_at DATETIME NULL,              -- worker 抢到任务的时刻（rescue 判死依据）
    retry_count INT NOT NULL DEFAULT 0,          -- 已被 rescue 重置次数（>=3 进死信）
    result VARCHAR(20) NULL,                     -- 终态结果（accepted/tle/mle/...，与终态 status 一致）
    stdout MEDIUMTEXT NULL,                      -- 判题输出（worker 截断后写入）
    stderr MEDIUMTEXT NULL,                      -- 判题错误输出
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    KEY idx_status (status),                     -- 条件更新走索引，不扫全表
    KEY idx_problem (problem_id),
    passed_cases INT NOT NULL DEFAULT 0,      -- 已通过的测试点数
    total_cases INT NOT NULL DEFAULT 0       -- 总测试点数
) ENGINE=InnoDB;

-- 专用账号：oj / oj123456（学习期口令，生产必须换强口令 + 最小权限）
-- 只授 oj 库权限，不授全局
CREATE USER IF NOT EXISTS 'oj'@'localhost' IDENTIFIED BY 'oj123456';
GRANT ALL PRIVILEGES ON oj.* TO 'oj'@'localhost';
FLUSH PRIVILEGES;

-- ============================================================
-- Outbox 表（Wave 2 任务 2.8，台阶 3 用）
--
-- 解决的问题：判题写完 MySQL 后若直接"再发一条 Redis 通知"，
-- 两步之间崩溃 → DB 是终态但前端永远等不到通知（不一致）。
-- Outbox 把"要发的事件"与业务状态放同一事务落库，再由投递器
-- 独立读出投递——本地事务 + 最终一致。
--
-- 为什么必须 delivered 标记：投递成功但不标 1 → 下次扫描重复投递。
-- ============================================================
CREATE TABLE IF NOT EXISTS outbox (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    submission_id BIGINT NOT NULL,            -- 关联的提交
    event VARCHAR(32) NOT NULL,               -- 事件类型：judged / failed
    payload MEDIUMTEXT NULL,                  -- 事件内容（JSON 文本）
    delivered TINYINT NOT NULL DEFAULT 0,     -- 0=待投递 1=已投递（防重复投递的唯一依据）
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    KEY idx_undelivered (delivered, id)       -- 投递器扫描：WHERE delivered=0 ORDER BY id
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS testcase(
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    problem_id BIGINT NOT NULL,
    input MEDIUMTEXT NOT NULL,
    expected_output MEDIUMTEXT NOT NULL,
    ordinal INT NOT NULL DEFAULT 0,
    score INT NOT NULL DEFAULT 1,
    KEY idx_problem (problem_id,ordinal)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS users(
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(32) NOT NULL UNIQUE,
    password_hash VARCHAR(128) NOT NULL,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;
