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
8. [DEEP DIVES (viva focus)](#8-deep-dives-viva-focus)
   - [8.1 The optimizer, from scratch](#81-the-optimizer-from-scratch)
   - [8.2 The Volcano iterator model & execution](#82-the-volcano-iterator-model--execution)
   - [8.3 Recovery management & ARIES (in depth)](#83-recovery-management--aries-in-depth)
   - [8.4 LSM-tree (full working)](#84-lsm-tree-full-working)
   - [8.5 MVCC — what it is and why we didn't use it](#85-mvcc--what-it-is-and-why-we-didnt-use-it)

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

---

# 8. DEEP DIVES (viva focus)

The four topics below are explained from first principles, then mapped to our
exact code. The last one (MVCC) is the "why not" question examiners love.

---

## 8.1 The optimizer, from scratch

### The problem it solves
The same SQL query can be executed in many different ways that all return the
same answer but take wildly different time. Example: `SELECT name FROM users
WHERE id = 7`.
- **Plan A:** read every row of `users` and keep the one with `id = 7` (a *table
  scan* — touches all N rows).
- **Plan B:** look `id = 7` up in the B+Tree, get its address, read that one row
  (an *index scan* — touches ~2 pages).

If `users` has a million rows, Plan A reads a million rows; Plan B reads two.
The optimizer's job is to **pick the cheaper plan automatically** — that is what
"cost-based" means.

### The two inputs
1. The **parsed query** (the AST `Statement`).
2. **Statistics** about the data, kept by the catalog.

### Statistics (how we know the data without scanning it every time)
[`Catalog::Analyze`](src/catalog/catalog.cpp) scans a table once and records, per
table:
- **`num_rows`** — how many rows.
- per column: **NDV** ("number of distinct values", computed with a `std::set`)
  and **min/max**.

These are cached and only recomputed when the data changes (`stats_dirty` flag,
set by inserts/deletes). So planning is cheap — it reads numbers, not rows.

### Selectivity = "what fraction of rows survive a filter?"
- **Equality** `col = x`: if a column has `NDV` distinct values and they're spread
  evenly, then `x` matches about `1 / NDV` of the rows. So estimated rows
  `≈ num_rows / NDV`. (A unique column like a primary key → NDV = num_rows → ~1
  row.)
- **Range** `col > x`, `col < x`: we use a fixed guess of **30%** (`0.3`) because
  estimating ranges precisely needs a histogram, which we keep out of scope.

This logic is [`Optimizer::EstimateRows`](src/optimizer/optimizer.cpp).

### The cost model
Costs are expressed in **estimated rows / page-reads** (a *relative* number — good
enough to compare two plans):
- **Sequential scan cost ≈ `num_rows`** (you read everything).
- **Index scan cost ≈ `1 + matched_rows`** (one probe down the tree, then read
  the matches).

### Decision 1 — table scan vs index scan ([`BuildScan`](src/optimizer/optimizer.cpp))
1. Look at the `WHERE` predicates; find one whose column **has an index** and uses
   a usable operator (`=`, `<`, `>`, …) — `PickIndexablePred` prefers an equality.
2. Compute `index_cost = 1 + EstimateRows(that predicate)` and `seq_cost =
   num_rows`.
3. If `index_cost < seq_cost`, build an **`IndexScan`**:
   - For `=`: the scan range is `low = high = key`.
   - For `<`/`<=`: `low = -∞, high = key`. For `>`/`>=`: `low = key, high = +∞`.
   - All original predicates are also attached as **residual filters**, re-checked
     on each row, so the answer is correct even if the key bounds are loose.
4. Otherwise build a **`SeqScan`** with the predicates pushed into it.

`EXPLAIN` prints exactly this, e.g.
`IndexScan(users using users_pk on id = 7) est_rows=2 [index_cost=2 < seq_cost=12]`.

### Decision 2 — join order & join algorithm
For `A JOIN B ON A.x = B.y` the optimizer:
1. **Resolves** which join column belongs to which table.
2. **Splits the WHERE predicates** into: A-only (pushed into A's scan), B-only
   (pushed into B's scan), and cross-table (applied as a `Filter` *after* the
   join).
3. **Chooses the outer table.** It computes, for each ordering,
   `cost = outer_rows × inner_access_cost`, where
   `inner_access_cost = 1 + rows/NDV` if the inner table has an **index on the
   join column** (cheap — *index-nested-loop join*), else `= inner_rows` (full
   scan per outer row — plain nested-loop). It keeps the cheaper ordering.
4. Builds a `NestedLoopJoinExecutor` whose **right side is created by a factory**:
   if the inner table is indexed on the join key, the factory makes an
   `IndexScan` seeked to the current outer row's key; otherwise a `SeqScan`.

So "join order selection" here = *which table is outer* + *whether the inner uses
its index*. `EXPLAIN` shows both candidate costs.

### Worked numbers
`users` (12 rows, `id` is PK), `orders` (10 rows, `uid` not indexed),
`... users JOIN orders ON id = uid`:
- users outer: `12 × inner_access(orders on uid)` = `12 × 10` = **120** (orders
  has no index on uid → full scan each time).
- orders outer: `10 × inner_access(users on id)` = `10 × (1 + 12/12)` = `10 × 2`
  = **20** (users' PK index → index-nested-loop).
→ It picks **orders as outer, users as inner with an index lookup** (cost 20).
That's exactly what `EXPLAIN` prints in the demo.

### Honest scope
Costs are relative, not calibrated to milliseconds; join ordering covers two
tables (the framework generalizes to DP for N); `WHERE` is `AND`-only. These are
documented and don't affect correctness.

---

## 8.2 The Volcano iterator model & execution

### The idea (demand-driven / "pull" execution)
Every operator looks identical from the outside:
```
Init()                 // get ready
Next(&row, &rid)       // produce ONE next row; return false when finished
GetOutputSchema()      // what columns this operator outputs
```
You build a query by **stacking operators into a tree**, and you run it by calling
`Next()` on the **top** operator. That call pulls one row from its child, which
pulls from *its* child, and so on down to the scan. Rows are produced **one at a
time, on demand** — this is why it's called *pull* or *demand-driven* execution.
The name "Volcano" comes from the classic research system that popularised it.

### Why this design
- **Uniformity** — every operator has the same 3 methods, so they compose freely.
- **Pipelining** — a row flows scan → filter → join → project without ever
  building a big temporary table in the middle. Memory stays small.
- **Composability** — the optimizer just snaps operators together like Lego.

### The base class
[`Executor`](src/execution/executor.h) declares the interface; every operator
subclasses it. `GetAffected()` is added for DML (INSERT/DELETE) to report how many
rows changed.

### Every operator, in detail ([`executor.cpp`](src/execution/executor.cpp))
- **`SeqScanExecutor`** — `Init()` opens a `TableHeap` iterator at the first row.
  `Next()` walks rows, **skips** any that fail the pushed-down predicates, takes a
  **shared lock** on the row it returns. This is the fallback access path.
- **`IndexScanExecutor`** — `Init()` calls `BPlusTree::RangeScan(low, high)` to
  collect the matching **RIDs**. `Next()` fetches each RID's tuple from the heap,
  re-checks residual predicates, locks it, returns it. This is the fast path the
  optimizer picks for selective predicates.
- **`FilterExecutor`** — pulls rows from its child and only passes the ones that
  satisfy a predicate list (used for leftover/cross-table predicates after a
  join).
- **`NestedLoopJoinExecutor`** — the join. `Init()` gets the first **left** row and
  builds a fresh **right** executor for it via the factory (seeded with the left
  row's join key). `Next()`: for the current left row, pull right rows that match
  the join key and emit the **combined** row (left columns ++ right columns); when
  the right side is exhausted, advance to the next left row and rebuild the right
  side. The factory is what makes it *index*-nested-loop when the inner table is
  indexed.
- **`ProjectionExecutor`** — keeps only the requested output columns (and expands
  `*`). It maps each output column to a child-column index once in its
  constructor, then just copies values per row.
- **`AggregateExecutor`** — `COUNT/SUM/MIN/MAX/AVG` with optional `GROUP BY`. This
  is a **blocking** operator: `Init()` **consumes the entire child**, building a
  hash map from group-key → running totals; `Next()` then emits one row per group.
  It must block because you can't know a `SUM` until you've seen every row.
- **`InsertExecutor` / `DeleteExecutor`** — DML. `Init()` does all the work
  (write/serialize rows or delete RIDs, update every index, write WAL, take
  exclusive locks); `Next()` returns nothing; `GetAffected()` returns the count.

### Pipelining vs blocking (a key viva point)
- **Pipelined (streaming)**: SeqScan, IndexScan, Filter, NestedLoopJoin,
  Projection — a row flows straight through; constant memory.
- **Blocking (materialising)**: Aggregate — must read all input before producing
  output. (A `Sort`/`ORDER BY` would also be blocking; we don't implement it.)

### RID propagation
`Next()` returns both the tuple *and* its `RID`. Scans return the real RID (so
`DeleteExecutor` knows which row to remove and which lock to take); joins/aggregates
return an invalid RID because their output rows don't correspond to a single
stored row.

### A concrete trace — `SELECT name FROM users WHERE id = 7`
```
Projection.Next()
   └─ IndexScan.Next()
        • (Init already did RangeScan(id=7,id=7) -> [RID(p,s)])
        • fetch page p, slot s -> bytes -> deserialize -> row{7,'grace',33}
        • LockShared(RID)
        • residual check id==7 ✓
        • return row
   • Projection keeps only `name` -> {'grace'}
returns 'grace'; next call -> IndexScan has no more RIDs -> false -> done
```

---

## 8.3 Recovery management & ARIES (in depth)

*(One of your two headline topics — know this cold.)*

### The problem
A crash (power loss, kill -9) can strike at any instant. Afterward the database
must guarantee:
- **Durability** — every **committed** transaction's effects are present.
- **Atomicity** — every **uncommitted** transaction's effects are gone (no
  half-finished work).

The difficulty: at the crash moment, memory (the buffer pool) is lost, and the
disk is in an *arbitrary* state — some committed changes may not have reached
disk yet, and some uncommitted changes may already have.

### Why disk is in an arbitrary state: buffer policies
Two independent choices decide what can be on disk:

| Policy | Meaning | Consequence |
| ------ | ------- | ----------- |
| **Force** | flush a txn's pages at commit | committed data guaranteed on disk → no redo needed (but slow) |
| **No-Force** | *don't* flush at commit | committed data may be only in memory → **need REDO** |
| **No-Steal** | never write an uncommitted txn's dirty page | uncommitted data never on disk → no undo needed (but needs huge memory) |
| **Steal** | may evict (write) a dirty *uncommitted* page | uncommitted data may be on disk → **need UNDO** |

MiniDB uses **Steal + No-Force** — the high-performance choice real databases
use — which is *exactly why we need both redo and undo*.

### The tool: the Write-Ahead Log (WAL)
Before a change is allowed to reach the data file, a **log record describing it**
is written first. Key pieces ([`log_record.h`](src/recovery/log_record.h)):
- **LSN** (Log Sequence Number): a monotonically increasing id for every record.
- **prev_lsn**: links all records of one transaction into a backward chain (for
  undo).
- **type**: `BEGIN`, `INSERT` (carries the *after-image* = new row), `MARK_DELETE`
  (carries the *before-image* = old row), `NEWPAGE` (a structural change),
  `COMMIT`, `ABORT`.
- **table_oid + rid**: where the change happened.
- **Page LSN**: every data page stores, in its first 8 bytes, the LSN of the last
  log record applied to it.

Two iron rules ([`log_manager.cpp`](src/recovery/log_manager.cpp) +
[`buffer_pool.cpp`](src/storage/buffer_pool.cpp)):
1. **Write-Ahead Rule** — before the buffer pool writes a dirty page to disk, it
   flushes the log **up to that page's LSN**. (So the description is always on
   disk before the change.)
2. **Force-log-at-commit** — `COMMIT` appends a COMMIT record and **flushes the
   log**. After that the transaction is durable, even if its data pages are still
   only in memory.

### ARIES recovery — the three phases ([`recovery_manager.cpp`](src/recovery/recovery_manager.cpp))
On startup we read the whole WAL and run:

**1. Analysis — who committed?**
Scan the log. A transaction is a **winner** if it has a `COMMIT` record, otherwise
a **loser**. (Full ARIES also reconstructs the dirty-page table and active-txn
table from the last checkpoint to limit how far back to scan; we simplify by
scanning the entire log — correct, just not incremental.)

**2. Redo — "repeat history".**
Replay **every** data/structural record in LSN order — *winners and losers
alike*. For each record: fetch its page; **if `page.LSN < record.LSN`, apply the
change and set `page.LSN = record.LSN`**; otherwise skip (it's already there).
- That `<` check makes redo **idempotent** — running recovery twice (or crashing
  during recovery) is safe.
- Why redo losers too? Because "repeating history" rebuilds the *exact* pre-crash
  state, which makes the following undo step simple and well-defined. This is the
  defining principle of ARIES.

**3. Undo — roll back the losers.**
Walk the losers' records **backwards** and apply the inverse of each:
`INSERT → remove the row`, `MARK_DELETE → restore the before-image`. Now no trace
of any uncommitted transaction remains.

Finally we **rebuild the indexes from the recovered heap** and flush.

### Three implementation details that make it actually work
- **`NEWPAGE` records.** The heap is a *chain* of pages. Under steal, a page's
  Init or its "next page" link could be lost in the crash, which would silently
  truncate a later scan. So whenever the heap allocates+links a page we log a
  `NEWPAGE`; redo re-initialises the page and restores the link. The very first
  page of each table is flushed at `CREATE TABLE` time so the chain always has a
  durable anchor.
- **Append-only slots → deterministic redo.** Because a row's slot is never
  reused, redo can replay an INSERT to the *exact* slot without the log storing a
  byte offset — replaying inserts in LSN order naturally lands each one in its
  original slot.
- **Indexes are not logged.** They're derived from the heap, so we just rebuild
  them after recovery. This removes the single hardest part of full ARIES
  (logging B+Tree splits/merges with compensation records).

### What `.crash` does
[`Database::SimulateCrash`](src/engine/database.cpp) drops the in-memory buffer
pool and catalog **without flushing**, and sets a flag so the destructor's normal
flush is skipped — a faithful power-loss simulation. Only the already-forced WAL
survives, so recovery has to do real work.

### How we differ from textbook ARIES (be ready for this)
We implement the **core algorithm** (WAL, write-ahead rule, force-at-commit,
analysis/redo/undo, page-LSN idempotence). We **simplify**: no fuzzy
**checkpoints** (we scan the full log instead of starting from a checkpoint), no
**CLRs** (compensation log records that make undo itself restartable), and no
dirty-page table. These optimisations bound recovery *time*; they don't change
*correctness*. We have a `CHECKPOINT` record type reserved for this extension.

### Trace of the crash demo
```
WAL after the run:  BEGIN(t1) INSERT(t1,row1) COMMIT(t1)
                    BEGIN(t2) INSERT(t2,row2)         <-- no COMMIT (crash here)
Analysis: winners={t1}, losers={t2}
Redo:     re-apply INSERT row1 AND INSERT row2 (repeat history)
Undo:     t2 is a loser -> remove row2
Result:   row1 present (committed), row2 gone (uncommitted). ✔
```

---

## 8.4 LSM-tree (full working)

*(Your other headline topic — this is the Track-C extension.)*

### Why LSM exists (the motivation)
A B+Tree updates data **in place**: every insert finds the right leaf and writes
there, causing **random disk I/O**, which is slow on spinning disks and wears out
SSDs. An **LSM-tree (Log-Structured Merge tree)** instead **batches writes in
memory and flushes them sequentially**, turning random writes into fast
sequential ones. It's the engine behind RocksDB, LevelDB, Cassandra, HBase,
ScyllaDB. The trade-off it accepts: reads may have to look in several places.

### The components & the write path ([`lsm_engine.cpp`](src/lsm/lsm_engine.cpp))
1. **MemTable** — an in-memory **sorted map** (`std::map<key, {value, tombstone}>`).
   Every `Put`/`Delete` goes here first. Writes are just an in-memory insert →
   very fast, and the map keeps keys sorted for free.
2. **Flush** — when the MemTable reaches a size limit, it is written out, in
   sorted order, as one **immutable SSTable** file, and the MemTable is cleared.
   Because the map is already sorted, the file is written **sequentially** in one
   pass.
3. **SSTable** ([`sstable.cpp`](src/lsm/sstable.cpp)) — a *Sorted String Table*:
   a header (`magic`, `count`) followed by records sorted by key:
   `[keylen][key][type: value|tombstone][vallen][value]`. When opened, it builds
   an in-memory **key → file-offset index** and a **bloom filter** by scanning
   once.

So on disk you end up with a **stack of immutable SSTables**, newest on top, each
internally sorted.

### The read path ([`LSMEngine::Get`](src/lsm/lsm_engine.cpp))
To read a key, check sources **newest → oldest** and stop at the first hit:
1. the **MemTable** (most recent writes),
2. then each **SSTable** from newest to oldest.
For each SSTable, first ask its **bloom filter**: "could this key be here?" If the
filter says *no*, skip the file entirely (this is what keeps reads fast). The
first source that has the key wins; if that entry is a **tombstone**, the key is
reported as *deleted / not found*.

"Newest wins" is how **updates and deletes** work without editing old files: a new
value (or a tombstone) in a newer source simply *shadows* the older one.

### Bloom filter ([`bloom_filter.h`](src/lsm/bloom_filter.h))
A small bit array + `k` hash functions. `Add(key)` sets `k` bits; `MaybeContains`
checks those `k` bits. Properties:
- **No false negatives** — if it says "not present," the key truly isn't there
  (so it's safe to skip the SSTable).
- **Some false positives** — it may occasionally say "maybe" when the key is
  absent; then we do a wasted lookup, which is just slower, never wrong.
We use **double hashing** (`hash_i = h1 + i·h2`) to synthesise `k` hashes from two
base hashes, sized at ~10 bits/key.

### Tombstones (deletes on immutable files)
You can't erase from an immutable SSTable. So `Delete(key)` writes a **tombstone**
— a marker that says "this key is deleted." It shadows older values on read and is
physically removed later, during compaction.

### Compaction ([`LSMEngine::Compact`](src/lsm/lsm_engine.cpp))
Over time SSTables pile up, so reads get slower and obsolete versions waste space.
**Compaction** fixes both: when the number of SSTables exceeds a trigger, we
**merge** them — keeping only the **newest version of each key** and **dropping
tombstones** (since nothing older survives the merge) — and write one fresh
SSTable, deleting the old files. We use **size-tiered / full-merge** compaction
(simple); production systems often use **leveled** compaction.

### The three "amplifications" (great viva vocabulary)
- **Write amplification** — data gets rewritten during compaction (LSM's cost).
- **Read amplification** — a read may touch several SSTables (bloom filters cut
  this).
- **Space amplification** — obsolete versions sit on disk until compaction
  reclaims them (we measured **~5×** reclaimed after compaction).

### Our measured results vs B+Tree (from `make bench`)
| Aspect | B+Tree | LSM | Why |
| ------ | ------ | --- | --- |
| Write throughput | ~534 K/s | ~510 K/s | similar at this scale (both mostly in memory) |
| Read latency | **~0.96 µs** | ~3.34 µs | B+Tree = one cached descent; LSM may probe several SSTables |
| On-disk size | 8.1 MB | **2.1 MB** | LSM packs sorted records densely (~3.8× smaller) |
| Space after 5× overwrite | — | **5× reclaimed** | compaction drops obsolete versions |

**One-line summary:** B+Tree is **read-optimised**, LSM is **write-/space-
optimised** — same workload, opposite design points.

### Honest limitations of our LSM (so you can defend it)
- The MemTable is **not** separately WAL-backed, so unflushed writes are lost on a
  crash; it's used as a **standalone KV engine for the benchmark**, not wired into
  the SQL/transaction layer.
- The engine doesn't reload existing SSTables on construction (fresh per run),
  and compaction is full-merge, not leveled. All are documented scope choices.

---

## 8.5 MVCC — what it is and why we didn't use it

### What MVCC is
**MVCC = Multi-Version Concurrency Control.** Instead of making readers and
writers block each other with locks, the database keeps **multiple versions of
each row**. Every transaction reads from a **consistent snapshot** — the set of
row versions that existed when it started. Used by PostgreSQL, Oracle, and MySQL's
InnoDB.

### How it would work
- Each row version is tagged with the transaction that **created** it (`xmin`) and
  the one that **deleted/superseded** it (`xmax`).
- A transaction with snapshot time `T` **sees** a version if it was created by a
  transaction that committed before `T` and not yet deleted as of `T`.
- A **write** creates a *new* version rather than overwriting; the old version
  stays for transactions that still need it.
- A background **garbage collector** (PostgreSQL calls it `VACUUM`) removes
  versions no live transaction can see.

### Pros and cons
- **Pros:** readers never block writers and vice-versa → high read concurrency;
  natural **snapshot isolation**.
- **Cons:** stores many versions (space + GC overhead); more complex visibility
  rules; write–write conflicts still need detection.

### Why MiniDB uses 2PL and *not* MVCC
1. **It was a different extension track.** The brief offered MVCC as **Track B**
   ("replace 2PL with MVCC"); we chose **Track C (LSM-tree)**. Implementing MVCC
   would be doing a *second* extension we didn't sign up for.
2. **The core requirement was 2PL.** The mandatory transaction component asks for
   **serializable isolation via two-phase locking**, which we implemented
   directly and cleanly.
3. **Simplicity + clean fit with recovery.** 2PL is easy to reason about and
   pairs naturally with our lock-based, ARIES-style recovery. MVCC would require
   **versioned tuples** (changing the on-disk row format), snapshot/timestamp
   management, and a garbage collector — a lot of extra machinery orthogonal to
   our chosen focus.
4. **The trade-off we accept:** 2PL can make readers and writers block each other
   (lower concurrency) but is simpler and gives strong isolation; MVCC trades
   storage and complexity for concurrency. For a teaching database centred on
   correctness and the LSM extension, 2PL is the right call.

**If asked "how would you add MVCC?"**: store a version chain per row (each with
`xmin`/`xmax`), give each transaction a start-timestamp snapshot, change reads to
walk the chain and pick the visible version, make writes append a new version, and
add GC to reclaim dead versions — replacing the shared/exclusive read locks while
keeping write conflict checks.
