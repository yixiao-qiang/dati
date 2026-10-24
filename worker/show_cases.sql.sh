#!/bin/bash
mysql -uoj -poj123456 oj -e "
SELECT p.id, p.title, t.ordinal, LEFT(t.expected_output, 40) AS exp_head
FROM problem p JOIN testcase t ON t.problem_id=p.id
ORDER BY p.id, t.ordinal;
" 2>/dev/null
