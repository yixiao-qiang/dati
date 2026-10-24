USE oj;
WITH first_ac AS (
    SELECT user_id, problem_id, MIN(created_at) AS fa
    FROM submission
    WHERE status = 'accepted' AND user_id IS NOT NULL
    GROUP BY user_id, problem_id
),
penalty AS (
    SELECT s.user_id, s.problem_id, COUNT(*) AS fails
    FROM submission s
    JOIN first_ac f ON s.user_id = f.user_id AND s.problem_id = f.problem_id
    WHERE s.created_at < f.fa AND s.status != 'accepted'
    GROUP BY s.user_id, s.problem_id
)
SELECT u.id, u.username,
       COUNT(f.problem_id) AS ac_count,
       COALESCE(SUM(p.fails * 20), 0) AS penalty_minutes
FROM users u
LEFT JOIN first_ac f ON f.user_id = u.id
LEFT JOIN penalty p ON p.user_id = u.id AND p.problem_id = f.problem_id
GROUP BY u.id, u.username
ORDER BY ac_count DESC, penalty_minutes ASC, u.id ASC;
