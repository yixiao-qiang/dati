#!/bin/bash
# 删除孤儿 outbox 行（submission 9003 已被删，delivered=1 已投递，无业务价值的测试残留）
# 先再次确认该行确实孤儿，再删（守数据：不按 id 盲删）
mysql -uoj -poj123456 oj -e "
DELETE FROM outbox
WHERE id=33
  AND submission_id=9003
  AND NOT EXISTS (SELECT 1 FROM submission s WHERE s.id=outbox.submission_id);
SELECT ROW_COUNT() AS deleted;
" 2>/dev/null
