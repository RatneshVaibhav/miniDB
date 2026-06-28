# MiniDB — A Working Relational Database Engine

> Advanced DBMS Capstone Project. A from-scratch relational database in C++20
> integrating storage, indexing, query processing, a cost-based optimizer,
> transactions, recovery, and an **LSM-tree storage extension (Track C)**.

---

## Team Information

**Team Name:** Team 7 &nbsp;·&nbsp; **PR title:** `TEAM_7`

| Full Name        | Roll Number | Scaler Email ID                  | GitHub                                              |
| ---------------- | ----------- | -------------------------------- | --------------------------------------------------- |
| Ratnesh Vaibhav  | 24BCS10413  | ratnesh.24bcs10413@sst.scaler.com | [@RatneshVaibhav](https://github.com/RatneshVaibhav) |
| Tanmay Mittal    | 24BCS10491  | tanmay.24bcs10491@sst.scaler.com  | [@tanmay933](https://github.com/tanmay933)          |
| Kesab Maharana   | 24BCS10653  | kesab.24bcs10653@sst.scaler.com   | [@Kesab2909](https://github.com/Kesab2909)          |
| Varun Uday Shet  | 24BCS10518  | varun.24bcs10518@sst.scaler.com   | [@varunudayshet](https://github.com/varunudayshet)  |

---

## 1. Project Overview

**Problem statement.** Build a coherent, working relational database engine that
demonstrates how real database internals fit together — not a feature-rich clone,
but a correct, explainable system.

**Goals.**
- Page-based storage with a buffer pool.
- A disk-backed B+Tree for primary and secondary indexes.
- SQL execution: `SELECT` (with `WHERE`, `JOIN`, aggregates), `INSERT`, `DELETE`.
- A cost-based optimizer that chooses between table scan and index scan and
  selects a join order.
- Serializable transactions via strict two-phase locking with deadlock detection.
- Write-ahead logging and ARIES-style crash recovery.

**Chosen extension track:** **Track C — Modern Storage (LSM-Tree).** An
LSM-tree key-value engine (MemTable → SSTables → compaction, with per-table
bloom filters) benchmarked against the B+Tree storage.

---

## 2. System Architecture

```
                       ┌──────────────────────────────┐
                       │            CLI / REPL          │   src/cli
                       └───────────────┬────────────────┘
                                       │ SQL text
                       ┌───────────────▼────────────────┐
                       │        Database (engine)         │   src/engine
                       │  parse → optimize → execute      │
                       │  txn lifecycle, recovery on open │
                       └───┬───────────┬───────────┬──────┘
          ┌────────────────┘           │           └─────────────────┐
          ▼                            ▼                              ▼
┌──────────────────┐      ┌─────────────────────────┐     ┌────────────────────┐
│  SQL Front-End   │      │   Execution + Optimizer  │     │  Txn / Concurrency │
│  lexer→parser→AST│      │  Volcano operators       │     │  LockManager (2PL) │
│  src/sql         │      │  cost model, join order  │     │  deadlock detection│
└──────────────────┘      │  src/execution,optimizer │     │  src/txn           │
                          └────────────┬─────────────┘     └─────────┬──────────┘
                                       │                              │
              ┌────────────────────────┼──────────────────────────┐  │
              ▼                         ▼                          ▼  ▼
   ┌────────────────────┐   ┌────────────────────┐    ┌────────────────────────┐
   │      Catalog       │   │      B+Tree         │    │     Recovery / WAL      │
   │  schema + stats    │   │  index (key→RID)    │    │  LogManager, ARIES-lite │
   │  src/catalog       │   │  src/index          │    │  src/recovery           │
   └─────────┬──────────┘   └─────────┬──────────┘    └────────────┬───────────┘
             └────────────────────────┼─────────────────────────────┘
                                       ▼
                       ┌──────────────────────────────┐
                       │         Storage Engine         │   src/storage
                       │ TableHeap → BufferPool(LRU)    │
                       │   → DiskManager (page file)    │
                       └──────────────────────────────┘

   Extension (Track C):  ┌─────────────────────────────┐
                         │  LSM Engine                  │   src/lsm
                         │  MemTable → SSTable → Compact│
                         │  + BloomFilter               │
                         └─────────────────────────────┘
```

**Major modules.** `common` (types, `Value`, `RID`), `storage` (page, heap,
buffer pool, disk), `index` (B+Tree), `catalog` (schema, stats), `record`
(tuple (de)serialization), `sql` (lexer/parser/AST), `optimizer`, `execution`,
`txn` (transactions + lock manager), `recovery` (WAL + recovery), `lsm`
(extension), `engine` (facade), `cli`.

**Data flow.** SQL text → `Parser` → `Statement` AST → `Optimizer` builds a
Volcano operator tree (with the chosen access paths/join order) → executors pull
tuples from `TableHeap`/B+Tree through the `BufferPool` → results stream back.
Every mutation is WAL-logged before its page is evicted; commits force the log.

---

## 3. Storage Layer

- **Page format** ([src/storage/table_page.h](src/storage/table_page.h)): a 4 KB
  **slotted page**. Header holds the page LSN, a next-page pointer (heap
  chaining), slot count, and a free-space pointer. The slot directory grows
  forward from the header while tuple bytes grow backward from the page end.
  Inserts are **append-only** (slot indices are stable), which lets recovery
  replay inserts deterministically.
- **Heap files** ([src/storage/table_heap.h](src/storage/table_heap.h)): a
  singly-linked chain of slotted pages with a forward iterator for sequential
  scans. New-page allocations are WAL-logged (`NEWPAGE`) so the chain is
  crash-durable.
- **Buffer pool** ([src/storage/buffer_pool.h](src/storage/buffer_pool.h)):
  fixed frame array, page table, pin counts, dirty flags, and an
  [LRU replacer](src/storage/lru_replacer.h). Enforces the **write-ahead rule**
  (flush log ≤ page LSN before writing a dirty page) and exposes hit/miss stats.
- **Disk manager** ([src/storage/disk_manager.h](src/storage/disk_manager.h)):
  fixed-size page I/O by page id over one database file, with `fsync`.

---

## 4. Indexing

- **B+Tree** ([src/index/bplus_tree.cpp](src/index/bplus_tree.cpp)): disk-backed,
  pages held through the buffer pool. Internal and leaf node layouts live over
  raw page bytes; **leaves are linked** for range scans.
- **Node structure.** Internal nodes hold `k` keys and `k+1` children; leaves
  hold `k` (key, RID) pairs plus a next-leaf pointer. Keys use a 32-byte
  **order-preserving encoding** ([src/index/index_key.h](src/index/index_key.h))
  so a single `memcmp` orders integers and strings correctly.
- **Operations.** Search, insert (with node split + parent propagation up to a
  new root), delete, and range scan. Both **primary-key** (auto-created) and an
  optional **secondary** index (`CREATE INDEX`) are supported.
- **Search path.** Descend from the root choosing the child whose separator
  bounds the key, until a leaf; then locate the key within the leaf.

---

## 5. Query Execution

- **Parser** ([src/sql/parser.cpp](src/sql/parser.cpp)): a recursive-descent
  parser over a hand-written tokenizer. Supports `CREATE TABLE/INDEX`,
  `INSERT`, `SELECT` (projection, `*`, aggregates, `WHERE` conjunctions,
  one `JOIN ... ON`, `GROUP BY`), `DELETE`, `BEGIN/COMMIT/ROLLBACK`, `EXPLAIN`.
- **Query plan generation** ([src/optimizer/optimizer.cpp](src/optimizer/optimizer.cpp)):
  the optimizer turns the AST into an operator tree, pushing single-table
  predicates into scans and choosing access paths/join order by cost.
- **Operator execution** ([src/execution/executor.cpp](src/execution/executor.cpp)):
  a **Volcano (iterator) model** — `Init()` then repeated `Next()`. Operators:
  `SeqScan`, `IndexScan`, `Filter`, `NestedLoopJoin` / index-nested-loop,
  `Projection`, `Aggregate` (`COUNT/SUM/MIN/MAX/AVG`, optional `GROUP BY`),
  `Insert`, `Delete`.

---

## 6. Optimizer

- **Cost estimation.** Costs are expressed in estimated rows/pages touched:
  sequential scan ≈ table row count; index scan ≈ probe + matched rows.
- **Selectivity estimation** (from [catalog statistics](src/catalog/catalog.cpp)):
  equality predicate → `1/NDV`; range predicate → a fixed fraction. Stats
  (row count, per-column NDV, min/max) are recomputed lazily after mutations.
- **Access-path choice.** The optimizer builds an `IndexScan` when an indexed
  column has a usable equality/range predicate **and** the estimated index cost
  beats the sequential-scan cost; otherwise a `SeqScan`. `EXPLAIN` prints the
  decision and the compared costs.
- **Join ordering.** For a two-table join it estimates `outer_rows × inner_access`
  for both orderings (favoring the side whose join column is indexed, enabling
  index-nested-loop) and picks the cheaper. `EXPLAIN` shows both costs.

---

## 7. Transactions & Concurrency

- **Locking strategy** ([src/txn/lock_manager.cpp](src/txn/lock_manager.cpp)):
  row-granularity **shared/exclusive** locks under **strict two-phase locking** —
  locks are acquired as rows are read/written and released together at
  commit/abort.
- **Isolation guarantees.** Strict 2PL yields **serializable** isolation.
- **Deadlock handling.** On every blocked acquisition the manager builds a
  **wait-for graph** and runs DFS cycle detection; if a cycle exists it aborts
  the **youngest** transaction (highest id) and signals it, which then rolls
  back and releases its locks so the survivor proceeds.

---

## 8. Recovery

- **WAL design** ([src/recovery/log_manager.cpp](src/recovery/log_manager.cpp)):
  an append-only log with monotonic LSNs, the write-ahead rule, and
  **force-log-at-commit**.
- **Log records** ([src/recovery/log_record.h](src/recovery/log_record.h)):
  `BEGIN`, `INSERT` (after-image), `MARK_DELETE` (before-image), `NEWPAGE`
  (structural), `COMMIT`, `ABORT`, each carrying a per-transaction `prev_lsn`
  chain for undo.
- **Crash recovery procedure** ([src/recovery/recovery_manager.cpp](src/recovery/recovery_manager.cpp)),
  simplified **ARIES**, runs on startup: **Analysis** (find committed vs
  in-flight txns) → **Redo** (repeat history for all logged changes, guarded by
  page LSN for idempotence) → **Undo** (roll back in-flight txns in reverse).
  Indexes are derived data and are **rebuilt from the recovered heaps**.

---

## 9. Extension Track — Track C: LSM-Tree Storage

- **Motivation.** Heap + B+Tree storage is read-optimized but pays random-I/O
  on writes. An LSM-tree turns writes into sequential flushes, trading some read
  amplification for high write throughput and compact storage — the modern
  write-optimized design used by RocksDB/Cassandra.
- **Design** ([src/lsm/](src/lsm/)): writes land in a sorted in-memory
  **MemTable**; when it fills it is flushed to an immutable, sorted **SSTable**
  (data records + in-memory key index + a **bloom filter**). Reads check the
  MemTable, then SSTables newest→oldest, gated by bloom filters. **Size-tiered
  compaction** merges SSTables, keeping the newest version of each key and
  dropping tombstones to reclaim space.
- **Results** (see §10): vs the B+Tree the LSM store is ~**3.8× more compact**
  and compaction reclaims **~5×** space after heavy overwrite; the B+Tree gives
  ~**4.5× lower read latency**. Both sustain comparable write throughput at this
  scale.

---

## 10. Benchmarks

**Experimental setup.** `make bench` (`./minidb_bench [N]`), default `N=100000`.
Single-threaded, g++ 13 `-O2`, results below from `N=60000` on the dev machine.
Numbers vary by hardware; rerun locally for your report.

### 1. LSM-tree vs B+Tree storage (60,000 keys)

| Engine    | Write (Kops/s) | Read (µs/op) | Size (KB) |
| --------- | -------------- | ------------ | --------- |
| B+Tree    | 566.6          | 0.67         | 4884.0    |
| LSM-tree  | 501.1          | 3.02         | 1278.2    |

**LSM space amplification** (5× overwrite of 60,000 keys):
`5273.6 KB across 25 SSTables` → after compaction `1054.7 KB across 1 SSTable`
(**5.0× reclaimed**).

### 2. Index scan vs sequential scan (12,000 rows, 2,000 queries)

| Access path                  | Latency (µs/query) |
| ---------------------------- | ------------------ |
| Point query on PK (index)    | 70.6               |
| Point query on non-key (seq) | 2312.2             |

→ **~33× speedup** from the optimizer choosing an index scan.

### 3. Buffer pool hit rate

Skewed access to 50 of 200 pages with a 64-frame pool: **99.0%** hit rate
(`4950 hits / 50 misses`), demonstrating LRU effectiveness.

**Analysis.** The B+Tree wins on point-read latency (one logarithmic descent
through cached pages); the LSM wins on storage footprint and write
sequentiality, with compaction bounding space amplification — exactly the
read-vs-write/space trade-off the two designs are known for.

---

## 11. Limitations

- **Missing features.** No `UPDATE` statement (model as delete + insert), no
  `ORDER BY`/`HAVING`, `WHERE` supports `AND` (not `OR`), joins are limited to
  two tables, and string index keys are truncated to 32 bytes.
- **Scalability limits.** Single-process, single database file; B+Tree `delete`
  tolerates underfull nodes (no merge/redistribute); deleted heap slots are
  tombstoned (no in-page compaction); SSTable key indexes are fully in memory.
- **Design simplifications.** Indexes are not WAL-logged — they are rebuilt from
  the heap on startup and after rollback (correct, but O(rows) on open). The CLI
  is a single session, so concurrent transactions are exercised via the test
  harness/benchmark rather than two REPLs.
- **Future improvements.** B+Tree node merging, `UPDATE`/`ORDER BY`, multi-way
  join planning (DP), MVCC, leveled LSM compaction, and SQL-over-LSM tables.

---

## 12. How to Run

**Dependencies.** A C++20 compiler (g++ 13+) and GNU `make`. No external
libraries.

**Build.**
```bash
make            # builds: minidb (CLI), minidb_tests, minidb_bench
make test       # run the full test suite (33 tests)
make bench      # run the benchmark harness
make run        # launch the interactive REPL
make clean
```

**Interactive example.**
```bash
./minidb mydb            # data persists in mydb.db / mydb.wal / mydb.meta
minidb> CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(20), age INT);
minidb> INSERT INTO users VALUES (1,'alice',30),(2,'bob',25);
minidb> SELECT name FROM users WHERE id = 1;
minidb> EXPLAIN SELECT name FROM users WHERE id = 1;
minidb> .crash            -- simulate power loss, then auto-recover
minidb> .exit
```

**Scripted demos** (in [docs/demos/](docs/demos/)):
```bash
./minidb demo < docs/demos/demo_core.sql        # CRUD, joins, aggregates, EXPLAIN
./minidb demo < docs/demos/demo_recovery.sql    # crash + WAL recovery
```

See [docs/architecture.md](docs/architecture.md) and
[docs/design-notes.md](docs/design-notes.md) for deeper internals and the
design rationale used in the viva.
