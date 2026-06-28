CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(20), age INT);
CREATE TABLE orders (oid INT PRIMARY KEY, uid INT, amount INT);
INSERT INTO users VALUES (1,'alice',30),(2,'bob',25),(3,'carol',40),(4,'dave',22);
INSERT INTO orders VALUES (10,1,150),(11,1,200),(12,2,75),(13,3,300);
SELECT id, name, age FROM users WHERE age >= 30;
EXPLAIN SELECT name FROM users WHERE id = 2;
SELECT name FROM users WHERE id = 2;
SELECT name, amount FROM users JOIN orders ON id = uid;
SELECT COUNT(*), SUM(amount), MIN(amount), MAX(amount) FROM orders;
DELETE FROM users WHERE id = 4;
SELECT COUNT(*) FROM users;
.tables
