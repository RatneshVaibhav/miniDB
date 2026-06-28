CREATE TABLE accounts (id INT PRIMARY KEY, owner VARCHAR(16), balance INT);
INSERT INTO accounts VALUES (1,'alice',1000),(2,'bob',500),(3,'carol',750);
SELECT * FROM accounts;
BEGIN;
INSERT INTO accounts VALUES (4,'dave',9999);
SELECT COUNT(*) FROM accounts;
.crash
SELECT * FROM accounts;
SELECT COUNT(*) FROM accounts;
