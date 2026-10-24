-- Wave 5 台阶 3：排行榜验证造数据（幂等重跑版）
-- 预期：rankA AC=2 罚时40 / rankB AC=1 罚时20 / rankC AC=0 罚时0
USE oj;

-- 清理旧测试数据（幂等）
DELETE s FROM submission s JOIN users u ON s.user_id = u.id
  WHERE u.username IN ('rankA','rankB','rankC');
DELETE FROM users WHERE username IN ('rankA','rankB','rankC');

INSERT INTO users (username, password_hash) VALUES
('rankA', 'x'), ('rankB', 'x'), ('rankC', 'x');

-- rankA: 题1 一次 AC；题2 失败2次后 AC → AC=2, 罚时=2*20=40
INSERT INTO submission (problem_id, code, language, status, user_id, created_at) VALUES
(1, 'a', 'cpp', 'accepted', (SELECT id FROM users WHERE username='rankA'), NOW() - INTERVAL 10 MINUTE),
(2, 'b', 'cpp', 'wrong_answer', (SELECT id FROM users WHERE username='rankA'), NOW() - INTERVAL 9 MINUTE),
(2, 'c', 'cpp', 'tle', (SELECT id FROM users WHERE username='rankA'), NOW() - INTERVAL 8 MINUTE),
(2, 'd', 'cpp', 'accepted', (SELECT id FROM users WHERE username='rankA'), NOW() - INTERVAL 7 MINUTE);

-- rankB: 题1 失败1次后 AC → AC=1, 罚时=1*20=20
INSERT INTO submission (problem_id, code, language, status, user_id, created_at) VALUES
(1, 'e', 'cpp', 'compile_error', (SELECT id FROM users WHERE username='rankB'), NOW() - INTERVAL 5 MINUTE),
(1, 'f', 'cpp', 'accepted', (SELECT id FROM users WHERE username='rankB'), NOW() - INTERVAL 4 MINUTE);

-- rankC: 只有失败，无 AC → AC=0, 罚时=0
INSERT INTO submission (problem_id, code, language, status, user_id, created_at) VALUES
(1, 'g', 'cpp', 'wrong_answer', (SELECT id FROM users WHERE username='rankC'), NOW() - INTERVAL 3 MINUTE);

-- 老提交：user_id NULL（认证上线前）——即使 accepted 也不进榜
INSERT INTO submission (problem_id, code, language, status, user_id, created_at) VALUES
(1, 'h', 'cpp', 'accepted', NULL, NOW() - INTERVAL 2 MINUTE);
