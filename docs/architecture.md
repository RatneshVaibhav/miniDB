# MiniDB Architecture

This document is the deep-dive companion to the README. It explains **how the
system is layered, how a statement flows through it, what every on-disk byte
means, and how the subsystems cooperate** at runtime. It is written to be read
next to the source — every claim points at a file you can open.

---

## 1. Design philosophy

MiniDB is built around three rules that show up everywhere in the code:

1. **Strict layering.** A module may only call *downward*. `execution` calls
   `storage`, never the reverse. This keeps each layer independently testable
   (the 33 tests exercise layers in isolation) and makes the dependency graph a
   DAG with no cycles.
2. **The heap is the source of truth.** Indexes, statistics, and the in-memory
   catalog are all *derived* from heap pages + the catalog file. Anything
   derived can be rebuilt, which is why recovery and rollback are simple.
3. **One choke point per invariant.** Each correctness invariant is enforced at
   exactly one place: the write-ahead rule lives in the buffer pool, LSN
   assignment lives in the log manager, lock ordering lives in the lock manager.
   No invariant is "everyone's responsibility."

---

## 2. Module map and dependency layering

Each layer depends only on the layers beneath it:

```
┌───────────────────────────────────────────────────────────────────┐
│ cli/                  interactive REPL, table formatting, .crash    │
├───────────────────────────────────────────────────────────────────┤
│ engine/               Database facade: parse → optimize → execute,  │
│                       transaction lifecycle, recovery on open       │
├───────────────────────────────────────────────────────────────────┤
│ optimizer/   execution/      txn/            recovery/              │
│ cost model   Volcano ops     lock manager    WAL + ARIES-lite       │
│ planner      (iterators)     transactions    log records            │
├───────────────────────────────────────────────────────────────────┤
│ catalog/              schema, table metadata, statistics, persist   │
│ index/                disk-backed B+Tree, order-preserving keys      │
│ record/               tuple (de)serialization                       │
├───────────────────────────────────────────────────────────────────┤
│ storage/              TableHeap → BufferPool (LRU) → DiskManager     │
├───────────────────────────────────────────────────────────────────┤
│ common/               Value, RID, ids, Status, config (PAGE_SIZE)   │
└───────────────────────────────────────────────────────────────────┘

  lsm/   ── Track C extension: a standalone write-optimized KV engine
            (MemTable → SSTable → compaction + bloom filters) over its own
            files; benchmarked against the B+Tree storage.
```

| Module | Key files | Responsibility |
| ------ | --------- | -------------- |
| `common` | [types.h](../src/common/types.h), [rid.h](../src/common/rid.h), [config.h](../src/common/config.h) | `Value` (tagged union), `RID`, id typedefs, `PAGE_SIZE=4096` |
| `storage` | [page.h](../src/storage/page.h), [table_page.h](../src/storage/table_page.h), [table_heap.cpp](../src/storage/table_heap.cpp), [buffer_pool.cpp](../src/storage/buffer_pool.cpp), [lru_replacer.h](../src/storage/lru_replacer.h), [disk_manager.cpp](../src/storage/disk_manager.cpp) | page frames, slotted pages, heap files, buffer pool, disk I/O |
| `record` | [tuple.h](../src/record/tuple.h) | row ↔ byte-string serialization with a null bitmap |
| `catalog` | [schema.h](../src/catalog/schema.h), [catalog.cpp](../src/catalog/catalog.cpp) | tables, indexes, column stats, metadata persistence |
| `index` | [bplus_tree.cpp](../src/index/bplus_tree.cpp), [index_key.h](../src/index/index_key.h) | disk-backed B+Tree, key encoding |
| `sql` | [lexer.h](../src/sql/lexer.h), [parser.cpp](../src/sql/parser.cpp), [ast.h](../src/sql/ast.h) | tokenizer, recursive-descent parser, AST |
| `optimizer` | [optimizer.cpp](../src/optimizer/optimizer.cpp) | cost model, access-path & join-order choice, `EXPLAIN` text |
| `execution` | [executor.cpp](../src/execution/executor.cpp) | Volcano operators |
| `txn` | [transaction.h](../src/txn/transaction.h), [lock_manager.cpp](../src/txn/lock_manager.cpp) | transaction state, strict 2PL, deadlock detection |
| `recovery` | [log_record.h](../src/recovery/log_record.h), [log_manager.cpp](../src/recovery/log_manager.cpp), [recovery_manager.cpp](../src/recovery/recovery_manager.cpp) | WAL, ARIES-lite recovery |
| `lsm` | [memtable + lsm_engine.cpp](../src/lsm/lsm_engine.cpp), [sstable.cpp](../src/lsm/sstable.cpp), [bloom_filter.h](../src/lsm/bloom_filter.h) | LSM-tree extension |
| `engine` | [database.cpp](../src/engine/database.cpp) | wires everything; `Execute(sql)` |
| `cli` | [main.cpp](../src/cli/main.cpp) | REPL |

---

## 3. The component interaction picture

```
                          ┌───────────────────────────┐
        SQL string  ─────►│        Database            │
                          │  (engine facade)           │
                          └──┬─────────┬─────────┬──────┘
                  parse      │  plan   │ execute │  txn ctl / recover
              ┌──────────────▼┐  ┌─────▼──────┐  │
              │   Parser       │  │ Optimizer  │  │
              │  → Statement   │  │ → operator │  │
              └────────────────┘  │   tree     │  │
                                  └─────┬──────┘  │
                                        │ pulls    │
                                  ┌─────▼───────────▼─────────────────┐
                                  │           Executors                │
                                  │  SeqScan IndexScan Join Agg …       │
                                  └──┬───────────┬───────────┬──────────┘
                   shared/excl locks │           │ key→RID   │ tuple bytes
                              ┌──────▼─────┐ ┌────▼─────┐ ┌───▼──────────┐
                              │ LockManager│ │  B+Tree  │ │  TableHeap   │
                              └──────┬─────┘ └────┬─────┘ └───┬──────────┘
                                     │            │           │ fetch/unpin
                                     │            └────────┬──┘
                                     │                ┌────▼──────┐  flush-log-≤-pageLSN
                                     │                │ BufferPool│──────────────┐
                                     │                └────┬──────┘              │
                                     │                ┌────▼──────┐         ┌────▼──────┐
                                     │                │ DiskManager│        │ LogManager │
                                     │                │  <db>.db   │        │  <db>.wal  │
                                     │                └───────────┘        └───────────┘
                              (released at commit/abort — strict 2PL)
```

Two cross-cutting edges are the heart of correctness:
- **BufferPool → LogManager** (`Flush(pageLSN)` before evicting a dirty page) is
  the write-ahead rule.
- **Executors → LockManager** (shared lock on read, exclusive on write) plus
  **engine → LockManager** (`UnlockAll` at commit/abort) is strict 2PL.

---

## 4. Lifecycle of a query — three worked examples

### 4.1 `SELECT name FROM users WHERE id = 2` (index path)

```
text ─► Lexer ─► [SELECT][name][FROM][users][WHERE][id][=][2]
     ─► Parser ─► Statement{ SELECT, table=users, items=[name],
                              where=[ id = 2 ] }
     ─► Optimizer:
          • Catalog.GetStats(users)              → num_rows, NDV(id)
          • PickIndexablePred → id has PK index, op == EQ
          • index_cost = 1 + rows/NDV  vs  seq_cost = rows
          • index_cost < seq_cost  ⇒  IndexScan(low=high=key(2))
          • wrap in Projection(name)
     ─► Execute (engine creates/uses a Transaction):
          Projection.Next()
            └► IndexScan.Next()
                 • B+Tree.Search(key(2)) → RID(p,s)
                 • BufferPool.FetchPage(p); TablePage.GetTuple(s) → bytes
                 • Tuple::Deserialize(schema, bytes)
                 • LockManager.LockShared(txn, RID)   ← strict 2PL
                 • residual predicate re-checked (id == 2)
            • Projection keeps the `name` column
     ─► autocommit: LogManager.Append(COMMIT); Flush(); LockManager.UnlockAll()
```

`EXPLAIN` prints exactly the optimizer's reasoning:
```
IndexScan(users using users_pk on id = 2)  est_rows=2 [index_cost=2 < seq_cost=4]
Projection
```

### 4.2 `SELECT name, amount FROM users JOIN orders ON id = uid`

```
Optimizer:
  • resolve join columns: id∈users, uid∈orders
  • costA (users outer) = rows(users) × inner_access(orders, uid)
    costB (orders outer) = rows(orders) × inner_access(users, id)
    inner_access(T, col) = has_index(col) ? 1 + rows/NDV : rows
  • choose the cheaper ordering
  • build NestedLoopJoin(outer_scan, factory)
        factory(key) = IndexScan(inner, key) if inner has an index on the
                       join column  (index-nested-loop)
                     = SeqScan(inner)         otherwise
Execute:
  for each outer row:
    build/seek inner using the outer row's join key
    for each inner row with equal key: emit (outer ⧺ inner)
```

The combined output schema is *outer columns followed by inner columns*;
projection then resolves `name`/`amount` by name.

### 4.3 `DELETE FROM t WHERE id < 10` (mutation + WAL)

```
Optimizer: BuildScan(t, [id < 10])  → IndexScan or SeqScan producing RIDs
Execute (DeleteExecutor.Init drains the child):
  for each (RID, tuple):
    LockManager.LockExclusive(txn, RID)          ← X lock
    TableHeap.DeleteTuple(RID, txn):
        read before-image, mark slot deleted,
        LogManager.Append(MARK_DELETE{rid, old_tuple, prev_lsn})  ← WAL first
        page.SetLSN(lsn); txn.undo_log.push_back(rec)
    for each index: BPlusTree.Remove(key(tuple))
Catalog.MarkStatsDirty(t)     ← stats recomputed lazily next time
```

Every mutation writes its log record **before** the page can be evicted, and the
before/after image in that record is what makes both crash-undo and runtime
rollback possible.

---

## 5. On-disk formats (byte by byte)

### 5.1 Data file `<db>.db` — slotted heap page (4096 bytes)

```
offset:  0        8        12       16        20            free_ptr        4096
         ┌────────┬────────┬────────┬────────┬───── slot dir ──►│  free  │◄── tuples ──┐
         │ pageLSN│ next_pid│ #slots │free_ptr│ (off,len)(off,len)│ space  │ tupN..tup0  │
         └────────┴────────┴────────┴────────┴──────────────────┴────────┴─────────────┘
         8 bytes  4 bytes  4 bytes  4 bytes   8 bytes each                 grow leftward
```

- The slot directory grows **rightward** from offset 20; tuple bytes grow
  **leftward** from 4096. They meet at the free region. Free space for a new
  tuple = `free_ptr − (20 + #slots×8)`; an insert needs `tuple_size + 8`.
- `next_pid` chains pages into a heap file.
- A deleted slot is set to `{0,0}` (tombstone); its index is never reused, so a
  tuple's `RID` is stable for life. See [table_page.h](../src/storage/table_page.h).

A **tuple** ([tuple.h](../src/record/tuple.h)) inside a slot:
```
[ null-bitmap: ceil(ncols/8) bytes ] then, per non-null column:
   BOOLEAN → 1B   INTEGER → 4B   BIGINT → 8B   VARCHAR → [u32 len][bytes]
```

### 5.2 B+Tree node page (4096 bytes)

```
common header:  [pageLSN:8][is_leaf:1 @8][#keys:4 @12][next_leaf:4 @16]   data @24

leaf:      | key[0..k) : 32B each | rid[0..k) : 8B each (page_id+slot) |
internal:  | child[0..k] : 4B each | key[0..k) : 32B each              |
```

Capacities are sized so a node fits in 4 KB with one slot of overflow headroom
before a split (`LEAF_MAX=100`, `INTERNAL_MAX=110`). Keys use the 32-byte
**order-preserving encoding** in [index_key.h](../src/index/index_key.h): integers
become 8-byte big-endian with the sign bit flipped (so unsigned `memcmp` matches
signed order); varchars are raw bytes, left-aligned and zero-padded. One
`memcmp` therefore orders any index correctly.

### 5.3 Write-ahead log `<db>.wal`

A flat stream of length-framed records: `[u32 total][payload]`. Each payload
([log_record.h](../src/recovery/log_record.h)):
```
lsn:8 | prev_lsn:8 | txn_id:8 | type:1 | table_oid:4 | rid(page:4,slot:4)
      | old_tuple(len-prefixed) | new_tuple(len-prefixed)
```
`prev_lsn` chains a transaction's records for undo. Record types: `BEGIN`,
`INSERT` (after-image in `new_tuple`), `MARK_DELETE` (before-image in
`old_tuple`), `NEWPAGE` (structural: `rid.page_id`=new page, `rid.slot`=prev
page), `COMMIT`, `ABORT`. The length frame lets the reader stop cleanly at a
torn tail write.

### 5.4 Catalog metadata `<db>.meta`

A small human-readable text file: a `next_oid`, then per table its name, oid,
heap head page, schema columns, and index definitions (name, key column,
primary flag). Index B+Tree **roots are not stored** — indexes are rebuilt from
heap data on load (§7).

### 5.5 LSM SSTable `<dir>/sst_N.sst`

```
[magic:4][count:4] then count records sorted by key:
   [keylen:4][key][type:1 (0=value,1=tombstone)][vallen:4][value]
```
On `Open`, the full `key → file-offset` index and a bloom filter are
reconstructed in memory by scanning once. See [sstable.cpp](../src/lsm/sstable.cpp).

---

## 6. How the subsystems cooperate at runtime

### 6.1 The page LSN — the thread tying storage to recovery
Every page reserves its first 8 bytes for a **page LSN** (the LSN of the last log
record applied to it). It powers two mechanisms:
- **Write-ahead rule:** before the buffer pool writes a dirty page, it calls
  `LogManager::Flush(page.LSN)` so the page's log is on disk first
  ([buffer_pool.cpp](../src/storage/buffer_pool.cpp), `GetVictimFrame`).
- **Idempotent redo:** during recovery a record is only re-applied if
  `page.LSN < record.lsn`, so replaying the log twice is safe.

### 6.2 Transactions across the layers
A `Transaction` ([transaction.h](../src/txn/transaction.h)) carries: its lock
set (released atomically at end — strict 2PL), its WAL `prev_lsn`, and an
in-memory **undo log** (the records it produced). The heap appends to all three
as it mutates data; the engine consumes them at commit/abort.

### 6.3 Statistics feedback loop
Mutations call `Catalog::MarkStatsDirty`. The next time the optimizer asks for
stats, `Catalog::Analyze` scans the heap once to recompute row count, per-column
NDV, and min/max. So plans always reflect roughly-current data without
maintaining counters on the hot path.

---

## 7. Concurrency & recovery interplay (end to end)

```
NORMAL RUN                              CRASH                 REOPEN (recovery)
──────────                              ─────                 ─────────────────
BEGIN  → log BEGIN                                            Database::Open:
INSERT → log INSERT, stamp page LSN                            1. Catalog.Load()
   (page may be evicted: log flushed                             (schemas back)
    first via write-ahead rule)         power loss            2. RecoveryManager:
COMMIT → log COMMIT, Flush() (force)    SimulateCrash():         • Analysis: who
   → LockManager.UnlockAll()             drop buffer pool +        committed?
                                         catalog WITHOUT        • Redo: replay ALL
(uncommitted txn has BEGIN+INSERT        flushing; <db>.wal       data records
 in the log but no COMMIT)               survives                 (page-LSN guard)
                                                              • Undo: reverse the
                                                                 in-flight txns
                                                              3. Rebuild indexes
                                                                 from recovered heap
                                                              4. FlushAll()
```

- **Steal/no-force** buffering means dirty *uncommitted* pages can reach disk and
  committed pages can still be in memory at the crash — that is exactly why both
  Redo and Undo are needed.
- `Database::SimulateCrash` ([database.cpp](../src/engine/database.cpp)) models
  power loss by resetting the in-memory objects **without** flushing; the
  destructor's normal flush is skipped via a `crashed_` guard.
- Because indexes are rebuilt from the heap, recovery never has to log or repair
  B+Tree structure — see the rationale in [design-notes.md](design-notes.md).

---

## 8. The Track-C LSM extension at a glance

```
        Put/Delete                         Get(key)
            │                                  │
            ▼                                  ▼ checked newest → oldest
   ┌──────────────────┐            ┌──────────────────────────────┐
   │  MemTable (RAM)   │            │ 1. MemTable                   │
   │  sorted std::map  │            │ 2. SSTable[0] (newest) ─┐bloom│
   └────────┬──────────┘            │ 3. SSTable[1]           │gate │
            │ size ≥ limit          │    …                    │     │
            ▼ flush (sequential)    │ n. SSTable[k] (oldest) ─┘     │
   ┌──────────────────┐            └──────────────────────────────┘
   │ SSTable (immut.)  │  +bloom filter, +in-RAM key→offset index
   └────────┬──────────┘
            │ count > trigger
            ▼ size-tiered compaction
   merge all → keep newest version per key, drop tombstones → 1 SSTable
```

This is a standalone key-value engine (it does not replace the SQL heap), which
is the cleanest way to benchmark the write-optimized LSM design against the
read-optimized B+Tree on the same workload — see README §10 for results.

---

## 9. Where to start reading the code

1. [common/types.h](../src/common/types.h) and [common/rid.h](../src/common/rid.h) — the vocabulary.
2. [storage/table_page.h](../src/storage/table_page.h) — how a row physically lives on a page.
3. [storage/buffer_pool.cpp](../src/storage/buffer_pool.cpp) — paging + the write-ahead rule.
4. [index/bplus_tree.cpp](../src/index/bplus_tree.cpp) — the index, including split logic.
5. [optimizer/optimizer.cpp](../src/optimizer/optimizer.cpp) + [execution/executor.cpp](../src/execution/executor.cpp) — planning and the Volcano operators.
6. [recovery/recovery_manager.cpp](../src/recovery/recovery_manager.cpp) — Analysis/Redo/Undo.
7. [engine/database.cpp](../src/engine/database.cpp) — the conductor that ties it together.
