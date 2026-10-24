#!/bin/bash
mysql -uoj -poj123456 oj -e "
SELECT o.id, o.submission_id, o.event, o.delivered, o.created_at
FROM outbox o LEFT JOIN submission s ON o.submission_id=s.id WHERE s.id IS NULL;
" 2>/dev/null
