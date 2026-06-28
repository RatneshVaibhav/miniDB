# MiniDB — Complete Viva Notes

A plain-English explanation of the **whole project**: what every folder does, how
the pieces work, the end-to-end flow, the key concepts, and a big set of likely
viva questions with answers. Read this once and you can explain any part of the
system.

---

## Table of contents
1. [What is MiniDB (in one minute)](#1-what-is-minidb-in-one-minute)
2. [The big picture — how the layers fit](#2-the-big-picture--how-the-layers-fit)
3. [End-to-end flow of a query (step by step)](#3-end-to-end-flow-of-a-query-step-by-step)
4. [Folder-by-folder explanation](#4-folder-by-folder-explanation)
5. [Core concepts in simple words](#5-core-concepts-in-simple-words)
6. [Worked examples (insert, select, crash)](#6-worked-examples)
7. [Demo viva questions & answers](#7-demo-viva-questions--answers)

---

## 1. What is MiniDB (in one minute)

MiniDB is a small **relational database engine** built from scratch in C++. You
talk to it in SQL (`CREATE TABLE`, `INSERT`, `SELECT`, `DELETE`, …) and it does
everything a real database does, just simpler:

- stores rows on **disk in fixed-size pages**,
- keeps a **cache (buffer pool)** of hot pages in memory,
- builds a **B+Tree index** so lookups are fast,
- **parses** your SQL and runs it through an **optimizer** that decides the
  cheapest way to run it,
- supports **transactions** (all-or-nothing) with **locking** for safe
  concurrent access,
- survives **crashes** using a **write-ahead log** and recovery,
- and has an extra **LSM-tree** storage engine (our Track-C extension) compared
  against the B+Tree.

The guiding idea: **keep each part simple and correct, and be able to explain
every decision.**

---

## 2. The big picture — how the layers fit

Think of it as a stack. Each layer only uses the layer below it.

```
You type SQL
      │
   ┌──▼──────────────┐
   │   CLI (cli/)     │  the prompt: reads SQL, prints result tables
   └──┬──────────────┘
   ┌──▼──────────────┐
   │ Engine (engine/) │  the "conductor": parse → optimize → execute,
   │                  │  manages transactions, runs recovery on startup
   └──┬───────────────┘
      │ uses ▼
   ┌─────────────────────────────────────────────────────────┐
   │ SQL (sql/)   Optimizer (optimizer/)   Execution (execution/) │
   │ turns text   picks the cheapest plan  runs the plan operators │
   │ into an AST  (index vs scan, joins)   (scan, join, aggregate) │
   └──┬──────────────────────────────────────────────────────┬───┘
      │                                                        │
   ┌──▼───────────┐   ┌──────────────┐   ┌──────────────┐   ┌─▼──────────┐
   │ Catalog       │   │ Index        │   │ Txn          │   │ Recovery    │
   │ (catalog/)    │   │ (index/)     │   │ (txn/)       │   │ (recovery/) │
   │ table info,   │   │ B+Tree       │   │ locks, 2PL,  │   │ WAL +       │
   │ stats         │   │ key → row    │   │ deadlock     │   │ crash redo/ │
   │               │   │              │   │              │   │ undo        │
   └──┬────────────┘   └──────┬───────┘   └──────────────┘   └─────┬──────┘
      │                       │                                     │
   ┌──▼─────────────────────────────────────────────────────────▼──┐
   │ Storage (storage/):  Heap files → Buffer Pool (LRU) → Disk file │
   └────────────────────────────────────────────────────────────────┘
            (Record (record/) packs a row into bytes; Common (common/)
             holds shared types like Value and RID)

   LSM (lsm/): a separate write-optimized storage engine for the benchmark.
```

**One rule to remember:** the **heap (the actual table data on disk) is the
single source of truth.** Indexes, statistics, and caches are all *derived* from
it — which is why crash recovery and rollback are simple (anything derived can be
rebuilt).

---

## 3. End-to-end flow of a query (step by step)

Take `SELECT name FROM users WHERE id = 7;`

1. **CLI** ([cli/main.cpp](src/cli/main.cpp)) reads the line, waits for the `;`,
   and hands the text to the engine.
2. **Engine** ([engine/database.cpp](src/engine/database.cpp)) starts a
   transaction and calls the parser.
3. **Lexer + Parser** ([sql/](src/sql/)) turn the text into a **token list**
   (`SELECT`, `name`, `FROM`, `users`, `WHERE`, `id`, `=`, `7`) and then into an
   **AST** — a `Statement` object describing "this is a SELECT of column `name`
   from table `users` with the filter `id = 7`".
4. **Optimizer** ([optimizer/optimizer.cpp](src/optimizer/optimizer.cpp)) asks
   the **Catalog** for statistics (how many rows, how many distinct ids). It sees
   `id` is the primary key (has a B+Tree index) and the filter is an equality, so
   it estimates: *index lookup ≈ 2 page reads vs full scan ≈ all rows.* It
   chooses an **Index Scan** and builds a small tree of **operators**.
5. **Execution** ([execution/executor.cpp](src/execution/executor.cpp)) runs the
   operators using the **Volcano model** (each operator has `Init()` then
   `Next()`):
   - `IndexScan` asks the **B+Tree** for the row id where `id = 7` → gets a
     **RID** (page number + slot).
   - It asks the **Buffer Pool** for that page, reads the **tuple bytes**, and
     **deserializes** them into a row.
   - It takes a **shared lock** on that row (for isolation).
   - `Projection` keeps only the `name` column.
6. The engine collects the rows, **commits** the transaction (writes a COMMIT to
   the log and releases locks), and returns the result.
7. **CLI** prints the result as an ASCII table.

For an **INSERT/DELETE** the same path runs, but the operator also: writes a
**WAL log record** (before the page is allowed to hit disk), updates the
**B+Tree index(es)**, and takes an **exclusive lock**.

---

## 4. Folder-by-folder explanation

### `common/` — shared vocabulary
The basic types everyone uses.
- **`Value`** (`types.h`): one SQL value — an integer, bigint, boolean, or
  string (VARCHAR), plus a "is NULL" flag. It knows how to compare itself to
  another value (`Compare`) and print itself (`ToString`).
- **`RID`** (`rid.h`): a *Record ID* = `(page_id, slot)`. It's the address of a
  row: "page 5, slot 3". Indexes point to RIDs; locks lock RIDs.
- **`config.h`**: constants like `PAGE_SIZE = 4096` and id type names.
- **`exception.h`**: `DBException` (normal errors shown to the user) and
  `TransactionAbortException` (thrown when a transaction must roll back, e.g. a
  deadlock victim).

### `storage/` — putting bytes on disk and caching them
This is the foundation. Four pieces:
- **`DiskManager`** (`disk_manager.cpp`): reads/writes **whole 4 KB pages** to a
  single file by page number. `AllocatePage()` grows the file, `ReadPage`/
  `WritePage` do I/O, `Flush` forces it to disk.
- **`Page`** (`page.h`): a 4 KB block of memory plus bookkeeping (pin count,
  dirty flag). The first 8 bytes of every page store its **LSN** (used by
  recovery).
- **`TablePage`** (`table_page.h`): interprets a page as a **slotted page** for
  storing rows. A small header + a **slot directory** (each slot = offset+length
  of a row); rows are packed from the end of the page toward the middle.
  `InsertTuple` adds a row (append-only — slot numbers never change),
  `GetTuple`/`DeleteTuple` read/tombstone a row.
- **`LRUReplacer`** (`lru_replacer.h`): decides which cached page to evict — the
  **Least Recently Used** one. `Victim`, `Pin`, `Unpin`.
- **`BufferPool`** (`buffer_pool.cpp`): the **cache** of pages in memory. `FetchPage`
  loads+pins a page, `NewPage` allocates one, `UnpinPage` releases it,
  `FlushPage`/`FlushAll` write dirty pages out. **Important:** before it writes a
  dirty page to disk it first flushes the log (the *write-ahead rule*).
- **`TableHeap`** (`table_heap.cpp`): a **heap file** = a linked list of slotted
  pages that holds one table's rows. `InsertTuple` finds a page with room (or
  allocates one), `GetTuple`/`DeleteTuple` by RID, and an **iterator**
  (`Begin`/`End`) walks every row for a full scan. It also writes WAL records for
  inserts/deletes and has `Redo*`/`Restore*` helpers used by recovery.

### `record/` — turning a row into bytes
- **`Tuple`** (`tuple.h`): a row in memory = a list of `Value`s. `Serialize`
  packs them into a byte string using the table's schema (a small **null bitmap**
  + fixed-width numbers + length-prefixed strings); `Deserialize` unpacks them
  back. This is what actually gets stored in a slot on a page.

### `catalog/` — the database's "table of contents"
- **`Schema`** (`schema.h`): the shape of a table — its columns (name + type) and
  which column is the primary key. `IndexOf("age")` returns the column's position.
- **`Catalog`** (`catalog.cpp`): remembers **all tables and indexes**. Key jobs:
  - `CreateTable` — make a heap file, store the schema, auto-build the primary-key
    index.
  - `CreateIndex` — build a secondary B+Tree index over a column.
  - `GetStats`/`Analyze` — compute **statistics** (row count, number of distinct
    values, min/max per column) by scanning the heap; the optimizer uses these.
  - `Persist`/`Load` — save/restore table definitions to a small `.meta` file so
    schemas survive a restart. Indexes are **rebuilt from the heap** on load.
  - `RebuildAllIndexes` — used after a rollback/recovery to make indexes match the
    heap again.

### `index/` — fast lookups with a B+Tree
- **`IndexKey`** (`index_key.h`): turns any `Value` into a fixed **32-byte
  order-preserving key** so the tree can compare keys with a single `memcmp`
  (integers are stored big-endian with the sign bit flipped; strings as raw
  bytes).
- **`BPlusTree`** (`bplus_tree.cpp`): a **disk-backed B+Tree** mapping key → RID.
  - `Search` — find one key.
  - `Insert` — add a key; if a node overflows it **splits** and the split can grow
    a new root.
  - `RangeScan` — visit all keys in `[low, high]` in order (leaves are linked
    together for this).
  - `Remove` / `Remove(key, rid)` — delete an entry.
  - **Unique vs non-unique**: a *primary key* index rejects duplicates; a
    *secondary* index allows many rows to share a key value (e.g., many students
    in `branch = 'cse'`).

### `sql/` — understanding the query text
- **`Lexer`** (`lexer.h`): the **tokenizer** — chops the SQL string into tokens
  (words, numbers, strings, symbols). It also skips `--` comments and accepts
  both `'single'` and `"double"` quoted strings.
- **`ast.h`**: the **Abstract Syntax Tree** node types — `Statement` (one parsed
  command), `Predicate` (a `WHERE` comparison), `SelectItem`, `JoinClause`, etc.
- **`Parser`** (`parser.cpp`): a **recursive-descent parser** — reads the tokens
  and builds a `Statement`. `ParseSelect`, `ParseInsert`, `ParseDelete`,
  `ParseCreate`, `ParseWhere` handle each clause. Throws a clear error on bad
  syntax.

### `optimizer/` — choosing the cheapest plan
- **`Optimizer`** (`optimizer.cpp`): converts a parsed `Statement` into a tree of
  **executors**, making **cost-based decisions**:
  - `EstimateRows` — uses stats to guess how many rows a filter returns
    (equality ≈ `rows / distinct_values`; range ≈ 30%).
  - `BuildScan` — **table scan vs index scan**: if a filtered column has an index
    and using it is cheaper, build an `IndexScan`; otherwise a `SeqScan`.
  - `PlanSelect` — for a **join**, it estimates both orderings
    (`outer_rows × inner_cost`) and picks the cheaper, preferring the side whose
    join column is indexed (so it can do an **index-nested-loop join**).
  - Everything it decides is printed by `EXPLAIN` with the compared costs.

### `execution/` — actually running the plan
- **`Executor`** (`executor.cpp`): the base class with the **Volcano interface**:
  `Init()` (set up) and `Next()` (give me the next row). Operators are stacked
  and pull rows from each other:
  - `SeqScanExecutor` — read every row of a heap, apply filters.
  - `IndexScanExecutor` — use a B+Tree to fetch only matching rows.
  - `FilterExecutor` — drop rows that fail a predicate.
  - `NestedLoopJoinExecutor` — for each left row, find matching right rows (uses
    an index on the inner table when available).
  - `ProjectionExecutor` — keep only the requested columns.
  - `AggregateExecutor` — `COUNT/SUM/MIN/MAX/AVG`, with optional `GROUP BY`.
  - `InsertExecutor` / `DeleteExecutor` — write rows + update indexes + WAL.
  - `EvalPredicate` — checks one `WHERE` comparison against a row.

### `txn/` — transactions and safe concurrency
- **`Transaction`** (`transaction.h`): the state of one running transaction — its
  id, the **locks it holds**, its place in the WAL chain (`prev_lsn`), and an
  **undo log** (the changes it made, used to roll back).
- **`LockManager`** (`lock_manager.cpp`): hands out **row locks** in two modes —
  **Shared (read)** and **Exclusive (write)**.
  - **Strict 2PL**: a transaction acquires locks as it touches rows and releases
    them **all at once** at commit/abort → gives **serializable** isolation.
  - **Deadlock detection**: when a transaction would block, it builds a
    **wait-for graph** and looks for a cycle; if found, it **aborts the youngest**
    transaction so the others can continue (`DetectVictim`).

### `recovery/` — surviving crashes (WAL + ARIES)
- **`LogRecord`** (`log_record.h`): one entry in the log — BEGIN, INSERT (with the
  new row), DELETE (with the old row), COMMIT, ABORT, or NEWPAGE (a structural
  change). Each can serialize/deserialize itself.
- **`LogManager`** (`log_manager.cpp`): the **Write-Ahead Log**. `Append` adds a
  record and gives it an **LSN** (a sequence number); `Flush` forces records to
  disk. Rule: **log first, then data** (write-ahead), and **force the log at
  commit** (so a committed transaction is durable).
- **`RecoveryManager`** (`recovery_manager.cpp`): runs on startup. Simplified
  **ARIES** in three phases:
  1. **Analysis** — read the log; which transactions committed (winners) vs not
     (losers)?
  2. **Redo** — replay *all* logged changes so the on-disk state is rebuilt
     (safe to repeat because of the page LSN check).
  3. **Undo** — reverse the losers' changes.
  Then it **rebuilds indexes** from the recovered heap.

### `lsm/` — the Track-C extension (LSM-tree storage)
A second storage engine optimized for **writes**, compared against the B+Tree.
- **`LSMEngine`** (`lsm_engine.cpp`): writes go into an in-memory sorted
  **MemTable**; when it fills, it is flushed to disk as an immutable, sorted
  **SSTable**. Reads check the MemTable, then SSTables newest→oldest.
- **`SSTable`** (`sstable.cpp`): a sorted file of key→value records with an
  in-memory key index. `Get`, `ScanAll`.
- **`BloomFilter`** (`bloom_filter.h`): a tiny probabilistic filter per SSTable
  that quickly says "this key is definitely NOT here" so reads skip useless files.
- **Compaction** (`Compact`): merges SSTables, keeps the newest value per key, and
  **drops deletions (tombstones)** to reclaim space.

### `engine/` — the conductor
- **`Database`** (`database.cpp`): ties everything together. `Execute(sql)` does
  parse → optimize → run, wrapped in a transaction. `Commit`/`Abort` manage the
  transaction end. `Open` runs **recovery** on startup. `SimulateCrash` throws
  away memory without flushing (for the `.crash` demo). It returns an
  `ExecutionResult` (columns + rows, or a status message).

### `cli/` — the interactive shell
- **`main.cpp`**: the `minidb>` prompt. Reads SQL until a `;`, sends it to the
  engine, and prints results as tables. Supports meta-commands `.tables`,
  `.crash`, `.help`, `.exit`, and an optional buffer-pool-size argument for the
  steal/recovery demo.

---

## 5. Core concepts in simple words

**Page / slotted page.** Disk is read/written in fixed 4 KB chunks called pages.
A *slotted page* stores variable-length rows: a directory of slots at the top
says where each row sits; rows grow in from the bottom. A row's address is
`(page, slot)` = its RID.

**Buffer pool.** RAM is small, disk is big. The buffer pool keeps recently used
pages in memory so we don't hit disk every time. When it's full it **evicts** the
least-recently-used page (LRU). A page is **pinned** while in use so it can't be
evicted.

**Heap file.** The unordered collection of pages that holds a table's rows,
linked page→page. Good for "scan everything"; slow for "find one row" — which is
why we add an index.

**B+Tree index.** A balanced tree that keeps keys **sorted** and maps each key to
a RID. Lookups take *O(log n)*, and because leaves are linked, **range queries**
(`age BETWEEN 20 AND 30`) are fast too. When a node gets too full it **splits**,
and splits can bubble up to create a new root — that's how it stays balanced.

**Tuple serialization.** A row in memory (list of values) must become a flat byte
string to live on a page. We write a null bitmap, then each value; strings are
length-prefixed. Reading reverses this.

**Catalog & statistics.** The catalog is metadata: what tables/columns/indexes
exist. Statistics (row counts, distinct values) let the optimizer *estimate* costs
without running the query.

**Parsing.** Turning text into structure. The **lexer** makes tokens; the
**parser** builds an **AST** (a tree describing the command). If the SQL is
malformed, parsing fails with a clear message.

**Volcano (iterator) model.** Every operator looks the same: `Init()` then call
`Next()` repeatedly to pull one row at a time. You build a query by stacking
operators (scan → filter → join → aggregate → project). Rows *stream* through
without building big temporary tables.

**Cost-based optimizer.** The same query can run many ways. The optimizer
estimates the cost of each option (using statistics) and picks the cheapest —
e.g., **index scan** for a selective `id = 7`, **table scan** for `city = 'pune'`
when `city` has no index, and the cheaper **join order** for a join.

**Transaction / ACID.** A transaction is a group of operations that must be
**all-or-nothing** (Atomic), leave data valid (Consistent), not interfere with
others (Isolated), and survive crashes once committed (Durable).

**Locking & 2PL.** To keep concurrent transactions from corrupting each other,
each row read takes a **shared** lock and each write takes an **exclusive** lock.
**Strict two-phase locking** holds all locks until the transaction ends →
**serializable** isolation (the strongest level).

**Deadlock.** Two transactions each waiting for a lock the other holds → stuck
forever. We detect the cycle in a **wait-for graph** and **abort the youngest**
one to break it.

**WAL (Write-Ahead Log) & LSN.** Before changing a page on disk, we first write a
**log record** describing the change, stamped with a sequence number (**LSN**). On
**commit** we force the log to disk. So even if the data pages are lost, the log
tells us what happened.

**Steal & No-Force (why we need redo *and* undo).**
- *No-Force*: at commit we do **not** flush the data pages (faster). If a crash
  loses them, **REDO** replays the committed changes from the log.
- *Steal*: the buffer pool **may** write a dirty *uncommitted* page to disk to
  free space. If a crash happens before that transaction commits, **UNDO** removes
  those changes.

**ARIES recovery.** On restart: **Analysis** (who committed?), **Redo** (replay
everything to rebuild state), **Undo** (reverse the uncommitted). The page LSN
makes redo *idempotent* (safe to repeat).

**LSM-tree (the extension).** Instead of updating data in place (B+Tree), an
LSM-tree **buffers writes in memory** and flushes them as **sorted files
(SSTables)**. Writes are fast and storage is compact; reads may check several
files (a **bloom filter** skips the ones that can't have the key). **Compaction**
periodically merges files and throws away old/deleted data.

---

## 6. Worked examples

### Inserting a row — `INSERT INTO users VALUES (7,'grace',33);`
1. Parser builds an INSERT `Statement`.
2. `InsertExecutor` turns the values into a `Tuple` and serializes it to bytes.
3. `TableHeap.InsertTuple` finds a page with room, writes the bytes into a slot →
   gets RID `(page, slot)`.
4. It writes a **WAL INSERT record** (with the row + RID) and stamps the page's
   LSN.
5. The row's key is inserted into every **B+Tree index** for the table.
6. An **exclusive lock** is taken on the new RID.
7. On commit, the log is forced and locks released.

### Reading with a filter — `SELECT name FROM users WHERE id = 7;`
See [section 3](#3-end-to-end-flow-of-a-query-step-by-step) — index scan → fetch
page → deserialize → project.

### Crash & recovery — `.crash` then reopen
1. Committed rows had their **log** forced (but maybe not their pages).
2. `.crash` drops memory without flushing → only the WAL survives on disk.
3. On reopen, recovery does **Analysis → Redo → Undo**: committed rows are
   replayed back (redo), uncommitted ones are removed (undo).
4. Indexes are rebuilt from the recovered heap. Result: committed data present,
   uncommitted data gone.

---

## 7. Demo viva questions & answers

**Q: Walk me through what happens when I run a SELECT.**
A: Lexer → tokens, Parser → AST, Optimizer → picks index/scan + join order and
builds an operator tree, Executors pull rows (Volcano `Next()`), fetching pages
through the buffer pool and locking rows; results stream back; the transaction
commits. (See section 3.)

**Q: What is a page and why fixed size?**
A: A 4 KB unit of disk I/O. Fixed size makes addressing (page number → file
offset) trivial and lets the buffer pool manage uniform slots.

**Q: What is the buffer pool and what does it replace?**
A: An in-memory cache of pages. When full it evicts the **least-recently-used**
unpinned page. It also enforces write-ahead logging before writing dirty pages.

**Q: How does the B+Tree stay balanced?**
A: On insert, if a leaf overflows it **splits** into two and pushes a separator
key up to the parent; that can cascade and create a new root. All leaves stay at
the same depth.

**Q: Why a B+Tree and not a hash index?**
A: A B+Tree supports both point lookups *and* ordered range scans; a hash index
only does point lookups.

**Q: How does the optimizer choose index vs table scan?**
A: It estimates rows from statistics (equality ≈ rows/distinct-values) and
compares an index cost (≈ a couple of page reads) to a scan cost (≈ all rows). It
picks the cheaper and shows it in `EXPLAIN`.

**Q: How is join order decided?**
A: For two tables it computes `outer_rows × inner_access_cost` for both orderings
and picks the cheaper, favouring the table whose join column is indexed (enables
index-nested-loop).

**Q: What isolation level do you provide and how?**
A: **Serializable**, using **strict two-phase locking** with shared/exclusive row
locks held until commit/abort.

**Q: How do you handle deadlocks?**
A: A **wait-for graph** is checked when a transaction blocks; if there's a cycle
we abort the **youngest** transaction (it has done the least work), and it
releases its locks so others proceed.

**Q: What is WAL and why "write-ahead"?**
A: The log of changes written **before** the corresponding data page is allowed to
reach disk. This guarantees we always have a record to redo/undo from after a
crash.

**Q: Difference between redo and undo? Why do you need both?**
A: Because of **no-force** (committed pages may not be flushed) we need **redo** to
restore committed work; because of **steal** (uncommitted dirty pages may be
flushed) we need **undo** to remove uncommitted work.

**Q: What does `.crash` actually do?**
A: It drops the in-memory buffer pool and catalog **without flushing**, simulating
a power cut. Only the already-forced WAL is on disk, so recovery has to do the
work.

**Q: Why don't you log index changes?**
A: An index is **derived** from the heap, so after recovery (or rollback) we just
**rebuild** indexes from the recovered rows. This keeps recovery simple and is
provably correct.

**Q: What is a tombstone (in the heap and in the LSM)?**
A: A marker for a deleted item. In the heap a deleted slot is marked empty; in the
LSM a delete writes a tombstone that hides older versions until compaction removes
it.

**Q: Why an LSM-tree for the extension? What's the trade-off?**
A: It's the write-/space-optimized opposite of the B+Tree. LSM writes are
sequential and storage is compact, but reads may touch several SSTables (bloom
filters reduce this). Our benchmark shows LSM ~3.8× smaller, B+Tree ~3.5× faster
on reads.

**Q: What does compaction do?**
A: Merges SSTables, keeps only the newest version of each key, and drops
tombstones — bounding read amplification and reclaiming space (we measured ~5×).

**Q: What's a RID and why is it stable?**
A: A Record ID `(page, slot)`. Inserts are append-only (slots are never reused),
so a row's RID never changes — which lets indexes and locks refer to it safely and
lets recovery replay inserts to the exact slot.

**Q: How does a secondary index handle many rows with the same value?**
A: The index B+Tree allows **duplicate keys** for non-unique (secondary) indexes;
a lookup does a range scan over equal keys and returns all matching RIDs. The
primary-key index stays unique.

**Q: What are the main limitations?**
A: No `UPDATE`/`ORDER BY`, `WHERE` is `AND`-only, joins are two-table,
string index keys are truncated to 32 bytes, and B+Tree deletes leave nodes
underfull (no merge). All are documented and none affect correctness of what's
implemented.

**Q: Where would this break at scale and how would you fix it?**
A: Index rebuild on startup is O(rows) — fix by logging index changes (full
ARIES). Single global lock-table mutex — fix by sharding. Single file/process —
fix with a server + sharding/replication.
