SELECT a, sum(b) FROM t GROUP BY a HAVING sum(b) > 100;
