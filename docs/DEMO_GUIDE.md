# MiniDB — Viva Demo Guide

A step-by-step script for demonstrating MiniDB live to the instructor. The whole
flow takes ~10–15 minutes and covers **every** required feature. Each step lists
the exact command, what the examiner will see, and talking points / likely
questions.

> Run everything from the project root: `MiniDB_Projects/Team_7/`.

---

## 0. One-time setup (before the viva)

```bash
make            # builds: minidb, minidb_tests, minidb_bench, minidb_condemo
```
Expect a clean, warning-free build. Keep the terminal maximized; font size up.

**Opening line:** "MiniDB is a relational database engine we built from scratch
in C++20 — storage, indexing, SQL execution, a cost-based optimizer,
transactions, and crash recovery — plus an LSM-tree storage engine as our
Track-C extension. No external libraries; just g++ and make."

---

## 1. Everything works — the test suite (1 min)

```bash
make test
```
**Shows:** `==== 33 passed, 0 failed, 0 skipped ====` — 33 tests across every
layer (storage, B+Tree, catalog, SQL, concurrency, recovery, LSM).

**Say:** "Each subsystem has unit + integration tests; this is our correctness
baseline. Now let's see it run live."

---

## 2. SQL, indexing & the optimizer (4 min) — the core demo

```bash
./minidb vivademo < docs/demos/demo_viva.sql
```
(Or run interactively: `./minidb mydb` and type statements yourself.)

Walk through the output and point out:

| What to point at | Talking point |
| ---------------- | ------------- |
| `Table 'users' created` | DDL; the `PRIMARY KEY` auto-builds a B+Tree index. |
| `SELECT ... WHERE age >= 35` | end-to-end query execution with a filter. |
| `EXPLAIN ... WHERE id = 7` → **IndexScan** `[index_cost=2 < seq_cost=12]` | **cost-based optimizer chooses the index** for a selective PK equality. |
| `EXPLAIN ... WHERE city = 'pune'` → **SeqScan** | no index on `city`, so the optimizer correctly falls back to a table scan. |
| `CREATE INDEX idx_age` then `EXPLAIN ... WHERE age = 40` → **IndexScan using idx_age** | **secondary index** built and then used. |
| `EXPLAIN ... JOIN orders ON id = uid` → `NestedLoopJoin(outer=orders, inner=users)` + `IndexNLJ` | **join-order selection** (picks the cheaper outer) and **index-nested-loop join**. |
| `COUNT/SUM/MIN/MAX`, `GROUP BY city` | aggregation. |
| `DELETE FROM users WHERE id = 4` then the row is gone from the index | mutation updates heap **and** indexes. |

**Key sentence for the examiner:** "`EXPLAIN` prints the actual costs we
compared, so every optimizer decision is auditable — index vs scan, and join
order."

**Likely question — "How does it know to use the index?"**
→ "The catalog keeps per-column statistics (row count, distinct values). For
`id = 7` the optimizer estimates index cost ≈ `1 + rows/NDV` = 2 versus a
sequential scan cost ≈ 12 rows, so it picks the index. On the un-indexed `city`
column there's no index to use, so it scans."

---

## 3. Storage engine & buffer pool (1 min)

The storage layer is exercised throughout, but to call it out explicitly:

```bash
make bench        # see section "3. Buffer pool hit rate" in the output
```
**Shows:** `hit_rate=99.0%` on a skewed access pattern with a 64-frame pool.

**Say:** "Data lives in 4 KB slotted pages in a heap file; a buffer pool with an
LRU replacer caches them. Under skewed access we get a 99% hit rate — the hot
pages stay resident. The buffer pool also enforces the write-ahead rule, which
leads into recovery."

(If asked for internals, open [storage/table_page.h](../src/storage/table_page.h)
and [storage/buffer_pool.cpp](../src/storage/buffer_pool.cpp).)

---

## 4. Transactions, locking & deadlock (2 min)

```bash
make condemo      # or: ./minidb_condemo
```
**Shows two live scenarios:**
- **Scenario 1:** Txn2's exclusive lock request **blocks** while Txn1 holds the
  row, and is granted only **after** Txn1 commits — demonstrating strict 2PL
  mutual exclusion and serialization.
- **Scenario 2:** Txn1 and Txn2 each hold one row and request the other's →
  **deadlock detected**, and the **youngest (Txn2) is aborted** while Txn1
  commits.

**Say:** "We use strict two-phase locking at row granularity for serializable
isolation. Deadlocks are caught precisely with a wait-for graph — we only abort
when a real cycle exists, and we pick the youngest transaction as the victim."

**Note for the examiner:** "The CLI is a single session, so we demonstrate
concurrency with this threaded harness; the same logic is also covered by the
`concurrency` unit tests (`./minidb_tests concurrency`)."

---

## 5. Crash recovery / WAL (2 min) — the showstopper

```bash
./minidb crashdemo < docs/demos/demo_recovery.sql
```
This inserts 3 committed rows, opens a transaction and inserts a 4th
(uncommitted), then issues `.crash` (drops all in-memory state **without**
flushing — simulating power loss) and reopens.

**Point out:** before the crash the count is **4**; after recovery the count is
**3** — the committed rows survived (replayed from the WAL) and the uncommitted
4th row was rolled back.

**Say:** "Every change is written to a write-ahead log before its page can hit
disk, and commits force the log. On restart we run simplified ARIES — analysis,
redo, undo. Committed work is preserved even though nothing was flushed at crash
time; the uncommitted transaction is undone."

**Interactive variant (more dramatic):**
```bash
./minidb demo
minidb> CREATE TABLE t (id INT PRIMARY KEY, v INT);
minidb> INSERT INTO t VALUES (1,100),(2,200);
minidb> SELECT * FROM t;
minidb> .crash
minidb> SELECT * FROM t;        -- rows are still there, recovered from the WAL
minidb> .exit
```

---

## 6. Extension Track C — LSM vs B+Tree (2 min)

```bash
make bench        # section 1 of the output
```
**Point at the table:** LSM is **~3.8× smaller** on disk; the B+Tree is
**~3.5× faster** on point reads; **compaction reclaims 5×** space after heavy
overwrite.

**Say:** "Our extension is an LSM-tree storage engine — MemTable, immutable
SSTables with bloom filters, and size-tiered compaction. It's the write-/space-
optimized counterpart to the B+Tree. The benchmark quantifies the classic
trade-off: the B+Tree wins on read latency, the LSM wins on storage footprint,
and compaction bounds space amplification."

(Internals: [src/lsm/](../src/lsm/), results & analysis in [../REPORT.md](../REPORT.md).)

---

## Quick reference (cheat-sheet to keep open)

| Goal | Command |
| ---- | ------- |
| Build everything | `make` |
| All tests pass | `make test` |
| Core SQL + optimizer demo | `./minidb vivademo < docs/demos/demo_viva.sql` |
| Interactive shell | `./minidb mydb` |
| Concurrency + deadlock | `make condemo` |
| Crash + recovery | `./minidb crashdemo < docs/demos/demo_recovery.sql` |
| Benchmarks (LSM/B+Tree, index, buffer pool) | `make bench` |
| Reset all demo data | `make clean` (or `rm -f *.db *.wal *.meta`) |

---

## Feature → requirement coverage (so nothing is missed)

| PDF requirement | Demonstrated in |
| --------------- | --------------- |
| Page-based heap files, page manager, buffer pool | §3 (bench hit rate) + tests |
| B+Tree search/insert/delete, index utilization | §2 (EXPLAIN IndexScan) |
| SELECT / WHERE / JOIN / INSERT / DELETE | §2 |
| Cost-based optimizer (selectivity, join order, scan vs index) | §2 (EXPLAIN) |
| Serializable isolation, 2PL, deadlocks | §4 (condemo) |
| WAL + crash recovery (committed preserved) | §5 |
| Extension Track C (LSM) + benchmark vs B+Tree | §6 (bench) |

---

## If something goes wrong (recovery during the demo)

- **Stale data from a previous run** → `make clean`, then rebuild and retry.
- **A demo file path error** → ensure you're in `MiniDB_Projects/Team_7/`.
- **Want a clean DB name** → demos use throwaway names (`vivademo`, `crashdemo`);
  pass any name to `./minidb <name>`; delete with `rm -f <name>.db <name>.wal <name>.meta`.
- **Examiner wants to type their own SQL** → `./minidb scratch`, then `.help`
  lists the commands; `.exit` to leave.

---

## Suggested speaking order (if time is tight, do these three)

1. `make test` → "everything passes."
2. `./minidb vivademo < docs/demos/demo_viva.sql` → SQL + optimizer (the heart).
3. `./minidb crashdemo < docs/demos/demo_recovery.sql` → crash recovery (the wow).

Then `make condemo` and `make bench` if time allows.
