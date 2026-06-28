# MiniDB — Project Report

**Course:** Advanced Database Management Systems — Capstone Project
**Deliverable:** Final System Report & Benchmark Report
**Extension Track:** Track C — Modern Storage (LSM-Tree)

> Team details (name, members, Scaler emails, roll numbers) are recorded in
> [README.md](README.md). The companion documents are
> [docs/architecture.md](docs/architecture.md) (internals) and
> [docs/design-notes.md](docs/design-notes.md) (decisions & trade-offs).

---

## 1. Executive Summary

MiniDB is a working relational database engine written from scratch in C++20
(~4,100 lines of source, ~720 lines of tests, **no external dependencies**). It
integrates all six required subsystems — a paged storage engine with a buffer
pool, a disk-backed B+Tree, SQL query execution, a cost-based optimizer,
serializable transactions with deadlock detection, and write-ahead-logging crash
recovery — and adds an **LSM-tree storage engine** as the Track-C extension,
benchmarked head-to-head against the B+Tree.

The system is **verified end-to-end**: a 33-test suite covering every layer
passes warning-free (`make test`), an interactive SQL REPL runs scripted demos
including a live crash-and-recover sequence, and a benchmark harness
(`make bench`) reproduces the results in §6.

**Headline results.** The optimizer's index-scan choice is **~35× faster** than a
sequential scan on point queries; the LSM store is **~3.8× more compact** than
the B+Tree while the B+Tree is **~3.5× faster** on point reads; compaction
reclaims **5×** space after heavy overwrite; the buffer pool sustains a **99%**
hit rate under skewed access.

---

## 2. Requirements Traceability

Every required feature maps to concrete code and a verifying test.

| Required feature | Implementation | Test(s) |
| ---------------- | -------------- | ------- |
| Page-based heap files | [storage/table_page.h](src/storage/table_page.h), [table_heap.cpp](src/storage/table_heap.cpp) | `storage.slotted_page_*`, `storage.table_heap_*` |
| Page manager / disk I/O | [storage/disk_manager.cpp](src/storage/disk_manager.cpp) | `storage.disk_manager_roundtrip` |
| Buffer pool (+ replacement) | [storage/buffer_pool.cpp](src/storage/buffer_pool.cpp), [lru_replacer.h](src/storage/lru_replacer.h) | `storage.buffer_pool_*`, `storage.lru_replacer_order` |
| B+Tree: search / insert / delete | [index/bplus_tree.cpp](src/index/bplus_tree.cpp) | `bplustree.*` (5 tests) |
| Primary + secondary index | [catalog/catalog.cpp](src/catalog/catalog.cpp) | `sql.secondary_index_used`, `catalog.*` |
| SELECT / WHERE | [execution/executor.cpp](src/execution/executor.cpp) | `sql.create_insert_select_where` |
| JOIN | `NestedLoopJoinExecutor` (incl. index-NLJ) | `sql.join_two_tables` |
| INSERT / DELETE | `InsertExecutor`, `DeleteExecutor` | `sql.delete_rows` |
| Cost-based optimizer (selectivity, join order) | [optimizer/optimizer.cpp](src/optimizer/optimizer.cpp) | `sql.primary_key_index_point_lookup`, `sql.optimizer_prefers_seqscan_for_unindexed` |
| Table scan vs index scan choice | `Optimizer::BuildScan` | same as above (+`EXPLAIN`) |
| Serializable isolation / 2PL | [txn/lock_manager.cpp](src/txn/lock_manager.cpp) | `concurrency.shared_locks_*`, `concurrency.exclusive_lock_blocks_*` |
| Deadlock handling | wait-for graph in `LockManager::DetectVictim` | `concurrency.deadlock_detected_one_victim` |
| Write-ahead logging | [recovery/log_manager.cpp](src/recovery/log_manager.cpp) | exercised by all `recovery.*` |
| Crash recovery (committed preserved) | [recovery/recovery_manager.cpp](src/recovery/recovery_manager.cpp) | `recovery.committed_data_survives_crash_via_wal`, `recovery.uncommitted_transaction_is_rolled_back` |
| **Track C: LSM (MemTable/SSTable/compaction)** | [lsm/](src/lsm/) | `lsm.*` (5 tests) |

**Status: all required features implemented and passing (33/33 tests).**

---

## 3. System Architecture (summary)

```
 CLI ─► Engine ─► Parser ─► Optimizer ─► Volcano Executors
                                │            │
                          Catalog/Stats   B+Tree
                                │            │
              LockManager(2PL)  └── Storage: TableHeap → BufferPool → Disk
              WAL + ARIES-lite recovery
              LSM engine (extension)
```

The engine parses SQL into an AST, the optimizer compiles it into a Volcano
operator tree choosing access paths and join order by cost, and the executors
pull tuples through the B+Tree / heap via the buffer pool. Every mutation is
WAL-logged before its page can be evicted; commits force the log; recovery runs
on startup. Full detail and diagrams are in
[docs/architecture.md](docs/architecture.md).

---

## 4. Implementation Highlights

- **Storage.** 4 KB slotted pages with **append-only slots**, giving each tuple a
  stable `RID` — the property that makes deterministic redo, stable secondary
  indexes, and RID-level locking all work.
- **Indexing.** Disk-backed B+Tree with linked leaves and a **32-byte
  order-preserving key encoding**, so the whole tree compares keys with a single
  `memcmp` regardless of column type.
- **Execution & optimization.** A uniform iterator interface; the optimizer makes
  **auditable** decisions (`EXPLAIN` prints the compared costs) and uses one join
  operator that does plain- or index-nested-loop depending on available indexes.
- **Concurrency.** Strict 2PL for serializability; **precise** deadlock resolution
  via wait-for-graph cycle detection, aborting the youngest transaction.
- **Recovery.** Simplified **ARIES** (Analysis → Redo → Undo) under steal/no-force
  buffering; a `NEWPAGE` log record keeps the heap page-chain crash-durable; an
  idempotent page-LSN guard makes redo safe to repeat.
- **Design leverage.** Indexes are treated as **derived data** and rebuilt from
  the heap on load / rollback / recovery — removing the single hardest piece of
  full ARIES while staying provably correct.

Rationale and the alternatives considered are documented in
[docs/design-notes.md](docs/design-notes.md).

---

## 5. Benchmark Report

### 5.1 Experimental setup

| Item | Value |
| ---- | ----- |
| Harness | `./minidb_bench [N]` ([benchmarks/bench_main.cpp](benchmarks/bench_main.cpp)) |
| Dataset size (N) | 100,000 keys/rows (storage); 20,000 rows (query); 2,000 queries |
| Compiler | g++ 13.3.0, `-O2 -std=c++20` |
| Platform | Linux x86-64, 12 cores (single-threaded benchmark) |
| Methodology | wall-clock via `std::chrono::steady_clock`; reads use 10,000 random probes; fixed RNG seeds for repeatability |

> Numbers are hardware-dependent; rerun `make bench` to regenerate for your
> machine. The values below are a representative run at N=100,000.

### 5.2 Result 1 — LSM-tree vs B+Tree storage

| Engine    | Write throughput (Kops/s) | Read latency (µs/op) | On-disk size (KB) |
| --------- | ------------------------- | -------------------- | ----------------- |
| B+Tree    | 534.4                     | 0.96                 | 8140.0            |
| LSM-tree  | 510.5                     | 3.34                 | 2137.6            |

**LSM space amplification** (writing the same 100,000 keys 5×):

| Stage | Size | SSTables |
| ----- | ---- | -------- |
| Before compaction | 8789.3 KB | 25 |
| After compaction  | 1757.8 KB | 1 |

→ **5.0× space reclaimed** by dropping obsolete versions and tombstones.

### 5.3 Result 2 — Index scan vs sequential scan (optimizer benefit)

| Access path                       | Latency (µs/query) |
| --------------------------------- | ------------------ |
| Point query on PK (index scan)    | 111.0              |
| Point query on non-key (seq scan) | 3900.2             |

→ **35.1× speedup** from the optimizer correctly choosing the index.

### 5.4 Result 3 — Buffer pool hit rate

Skewed access (5,000 fetches over 50 hot pages of 200, 64-frame pool):
**99.0% hit rate** (4,950 hits / 50 misses).

### 5.5 Analysis

- **Write throughput is comparable** at this scale: the B+Tree edges ahead (~5%)
  because both fit largely in the buffer pool, so the LSM's sequential-write
  advantage is masked by the cost of building/flushing SSTables. The gap would
  widen in the LSM's favor as data outgrows RAM and B+Tree writes become
  random I/O.
- **Read latency favors the B+Tree (~3.5×)**: a point read is one logarithmic
  descent through cached pages, whereas an LSM read may probe multiple SSTables
  (bloom filters cut, but do not eliminate, this read amplification).
- **Storage strongly favors the LSM (~3.8× smaller)**: sorted, densely packed
  SSTables versus B+Tree nodes that are ~70% full on average; compaction further
  bounds amplification under overwrite (the 5× result).
- **The optimizer earns its keep**: 35× on point lookups validates that the
  cost model picks the right access path — the central optimizer requirement.
- **The buffer pool works**: 99% hits on a skewed workload confirms the LRU
  replacer keeps the hot set resident.

This is the textbook **read-optimized (B+Tree) vs write-/space-optimized (LSM)**
trade-off, reproduced and quantified on one codebase — the goal of Track C.

---

## 6. Testing & Verification

`make test` runs **33 tests**, all passing, warning-free:

| Suite | # | What it proves |
| ----- | - | -------------- |
| `storage` | 5 | slotted page ops, disk roundtrip, LRU order, eviction+reload, 500-row heap scan/delete |
| `bplustree` | 5 | 5,000-key splits, random insert, sorted range scan, delete, varchar keys |
| `catalog` | 2 | create/insert/index/search, persist + reload rebuilds index |
| `sql` | 7 | WHERE, index vs seq choice, join, delete, aggregates/GROUP BY, secondary index |
| `concurrency` | 3 | shared compatibility, X-lock blocking, deadlock → one victim |
| `recovery` | 4 | committed survives crash (multi-page WAL redo), uncommitted rolled back, explicit commit, clean reopen |
| `lsm` | 5 | put/get/overwrite/tombstone, flush, newest-wins, compaction reclaim, bloom no-false-negatives |

**Live demonstrations** ([docs/demos/](docs/demos/)):
- `demo_core.sql` — CREATE/INSERT/SELECT+WHERE, JOIN, aggregates, DELETE, and
  `EXPLAIN` showing an index-scan decision.
- `demo_recovery.sql` — insert + open transaction, `.crash`, then automatic
  recovery showing committed rows preserved and the uncommitted row rolled back.

---

## 7. Challenges & Lessons

- **Crash-durable heap structure.** The hardest bug class was structural: under
  steal buffering, a lost page-chain link silently truncates scans after
  recovery. Solved with a `NEWPAGE` WAL record that rebuilds the chain during
  redo — a concrete lesson in why ARIES logs structure, not just data.
- **Keeping recovery simple.** Realizing indexes are *derived* and can be rebuilt
  from the heap collapsed a large amount of would-be recovery/undo logic into one
  linear scan — the highest-leverage design decision in the project.
- **Deterministic redo.** Choosing append-only slots so an `RID` is stable made
  physical redo replay to the exact slot without storing byte offsets — a small
  storage decision that simplified the entire recovery path.

---

## 8. Conclusion

MiniDB meets every core requirement of the capstone and delivers a complete
Track-C extension, verified by an automated test suite, live demos, and a
reproducible benchmark. The emphasis throughout was **correctness and
explainability over feature count** — each subsystem is small enough to defend in
the viva, each invariant is enforced at a single auditable point, and each design
trade-off is documented with the alternatives considered. The result is a
coherent, working database whose internals can be read, run, broken (via
`.crash`), and recovered — exactly the understanding of database engineering the
project sets out to demonstrate.

**Build & run:** `make && make test && make bench`, then `./minidb mydb`.
