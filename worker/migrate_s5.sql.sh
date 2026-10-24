#!/bin/bash
# Wave5 S5 台阶5：error_msg 列 + 外键迁移脚本（VM 上执行）
set -u
MYSQL() { mysql -uoj -poj123456 oj 2>/dev/null "$@"; }

echo "== 1. 孤儿数据检查 =="
ORPHAN_USER=$(MYSQL -N -e "SELECT COUNT(*) FROM submission s LEFT JOIN users u ON s.user_id=u.id WHERE s.user_id IS NOT NULL AND u.id IS NULL")
ORPHAN_PROB=$(MYSQL -N -e "SELECT COUNT(*) FROM testcase t LEFT JOIN problem p ON t.problem_id=p.id WHERE p.id IS NULL")
ORPHAN_OUT=$(MYSQL -N -e "SELECT COUNT(*) FROM outbox o LEFT JOIN submission s ON o.submission_id=s.id WHERE s.id IS NULL")
echo "orphan_user=$ORPHAN_USER orphan_problem=$ORPHAN_PROB orphan_outbox=$ORPHAN_OUT"

# 孤儿数据会让外键 ALTER 失败（外键是"从此以后"的约束，不追溯存量）。
# 若有孤儿：先定位看是什么，人工决定清理/修复，不自动删（守数据）。
if [ "$ORPHAN_USER" != "0" ] || [ "$ORPHAN_PROB" != "0" ] || [ "$ORPHAN_OUT" != "0" ]; then
  echo "!! 存在孤儿数据，中止迁移。请先排查清理。"
  exit 1
fi

echo "== 2. 老库加 error_msg 列（幂等：已存在则跳过）=="
COL_EXISTS=$(MYSQL -N -e "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='oj' AND TABLE_NAME='submission' AND COLUMN_NAME='error_msg'")
if [ "$COL_EXISTS" = "0" ]; then
  MYSQL -e "ALTER TABLE submission ADD COLUMN error_msg VARCHAR(512) NULL AFTER status" && echo "  ADD COLUMN error_msg OK"
else
  echo "  error_msg 列已存在，跳过"
fi

echo "== 3. 加外键与唯一约束（幂等：已存在则跳过）=="
for con in fk_submission_problem fk_submission_user fk_testcase_problem fk_outbox_submission; do
  N=$(MYSQL -N -e "SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS WHERE CONSTRAINT_SCHEMA='oj' AND CONSTRAINT_NAME='$con'")
  if [ "$N" = "0" ]; then
    case $con in
      fk_submission_problem) SQL="ALTER TABLE submission ADD CONSTRAINT fk_submission_problem FOREIGN KEY (problem_id) REFERENCES problem(id)" ;;
      fk_submission_user)    SQL="ALTER TABLE submission ADD CONSTRAINT fk_submission_user FOREIGN KEY (user_id) REFERENCES users(id)" ;;
      fk_testcase_problem)   SQL="ALTER TABLE testcase ADD CONSTRAINT fk_testcase_problem FOREIGN KEY (problem_id) REFERENCES problem(id)" ;;
      fk_outbox_submission)  SQL="ALTER TABLE outbox ADD CONSTRAINT fk_outbox_submission FOREIGN KEY (submission_id) REFERENCES submission(id)" ;;
    esac
    if MYSQL -e "$SQL"; then echo "  ADD $con OK"; else echo "  !! ADD $con FAILED"; exit 1; fi
  else
    echo "  $con 已存在，跳过"
  fi
done
UK=$(MYSQL -N -e "SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS WHERE CONSTRAINT_SCHEMA='oj' AND CONSTRAINT_NAME='uk_testcase_problem_ordinal'")
if [ "$UK" = "0" ]; then
  MYSQL -e "ALTER TABLE testcase ADD UNIQUE KEY uk_testcase_problem_ordinal (problem_id, ordinal)" && echo "  ADD unique key OK" || { echo "  !! ADD unique key FAILED"; exit 1; }
else
  echo "  unique key 已存在，跳过"
fi

echo "== 4. 验证 =="
MYSQL -e "SHOW CREATE TABLE submission\G" | grep -E "error_msg|CONSTRAINT"
MYSQL -e "SHOW CREATE TABLE testcase\G" | grep -E "UNIQUE|CONSTRAINT"
MYSQL -e "SHOW CREATE TABLE outbox\G" | grep "CONSTRAINT"
echo "MIGRATION DONE"
