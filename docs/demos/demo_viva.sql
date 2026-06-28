-- ============================================================================
-- MiniDB VIVA DEMO  —  run with:  ./minidb vivademo < docs/demos/demo_viva.sql
-- Covers: DDL, INSERT, SELECT+WHERE, primary & secondary index usage (EXPLAIN),
--         JOIN, aggregates/GROUP BY, DELETE.
-- ============================================================================

-- 1) DDL: create tables (primary keys auto-build a B+Tree index)
CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(20), age INT, city VARCHAR(16));
CREATE TABLE orders (oid INT PRIMARY KEY, uid INT, amount INT);

-- 2) INSERT (enough rows that index vs scan costs differ clearly)
INSERT INTO users VALUES
 (1,'alice',30,'pune'),(2,'bob',25,'delhi'),(3,'carol',40,'pune'),
 (4,'dave',22,'delhi'),(5,'erin',35,'mumbai'),(6,'frank',28,'pune'),
 (7,'grace',33,'delhi'),(8,'heidi',45,'mumbai'),(9,'ivan',27,'pune'),
 (10,'judy',38,'delhi'),(11,'mallory',29,'mumbai'),(12,'oscar',41,'pune');

INSERT INTO orders VALUES
 (101,1,150),(102,1,200),(103,2,75),(104,3,300),(105,3,50),
 (106,5,500),(107,8,120),(108,9,90),(109,12,260),(110,1,40);

-- 3) SELECT + WHERE  (range predicate)
SELECT id, name, age FROM users WHERE age >= 35;

-- 4) OPTIMIZER: primary-key equality -> chooses an INDEX SCAN
EXPLAIN SELECT name FROM users WHERE id = 7;
SELECT name FROM users WHERE id = 7;

-- 5) OPTIMIZER: predicate on a NON-indexed column -> chooses a SEQ SCAN
EXPLAIN SELECT id FROM users WHERE city = 'pune';
SELECT id, name FROM users WHERE city = 'pune';

-- 6) SECONDARY INDEX: build one, then watch the optimizer use it
CREATE INDEX idx_age ON users (age);
EXPLAIN SELECT name FROM users WHERE age = 40;
SELECT name FROM users WHERE age = 40;

-- 7) JOIN (optimizer picks join order; inner side uses its index when possible)
EXPLAIN SELECT name, amount FROM users JOIN orders ON id = uid;
SELECT name, amount FROM users JOIN orders ON id = uid;

-- 8) AGGREGATES + GROUP BY
SELECT COUNT(*), SUM(amount), MIN(amount), MAX(amount) FROM orders;
SELECT city, COUNT(*) FROM users GROUP BY city;

-- 9) DELETE (removes from heap AND every index), then verify
DELETE FROM users WHERE id = 4;
SELECT COUNT(*) FROM users;
SELECT name FROM users WHERE id = 4;

-- 10) list tables
.tables
