# gdblite

A lightweight graph database written in Rust.

gdblite stores a graph as an append-only **record log**: a compact byte log of
nodes, values and tuples where every entry is addressed by its position in the
log. Edges are tuples of ids pointing back at earlier entries, encoded relative
to the tuple's own position so that nearby references take only a byte or two.

A persistent **dynamic table** (an on-disk hash table) maps 64 or 128 bit keys
to lists of `u64` values, for use as an index.

> **Status:** early development. The storage layer and a graph API (interned
> values, links with delete and history, index-backed queries) are in place;
> a query language is still to come.

## Data model

Every entry in the log has an **id**: its byte position in the log body plus the
log's `start_id`. Ids are therefore stable, ordered by insertion time, and need
no separate index to resolve.

| Entry     | Encoding                                          | Use                         |
| --------- | ------------------------------------------------- | --------------------------- |
| **Node**  | A zero-length value: a single `0x00` byte         | A vertex with no payload    |
| **Value** | Length followed by that many bytes                | Labels, properties, blobs   |
| **Tuple** | Count followed by that many ids, plus an extra bit | Edges / hyperedges linking entries |

By convention the fifth field of a tuple (index 4, `record_log::WHEN_FIELD`)
holds a timestamp.

## Graph

`graph::Graph` is the database: nodes, interned values, and links of
subject, predicate and object (plus author, creation time and subgraphs),
stored in memory mapped files and indexed by each of those.

```rust
use gdblite::graph::{Graph, Options};
use gdblite::Linkage;

// Pass Some(path_prefix) for a graph on disk, or None for one in memory.
let mut g = Graph::open(None, Options::default()).unwrap();
let knows = g.value(b"knows").unwrap();        // values are interned
let (alice, bob) = (g.node().unwrap(), g.node().unwrap());
let link = g.link(alice, knows, bob).unwrap();
assert_eq!(g.link(alice, knows, bob).unwrap(), link);  // already live
g.commit().unwrap();

assert_eq!(g.follow_forward(knows, [alice]), [bob].into());
assert_eq!(g.find_link(alice, knows, bob), Some(link));

// Deleting appends a dead link; the history stays queryable by id (time).
g.delete_link(alice, knows, bob).unwrap();
assert!(g.follow_forward(knows, [alice]).is_empty());
let all = g.links_at(Linkage::Subject, [alice]);
assert_eq!(g.live(&all, Some(link)), [link].into());
```

A link is identified by its subject, predicate and object, and the latest link
with them says whether it is live or dead. Ids increase as entries are
appended, so queries taking `as_of` see the graph as of that id. Appends are
visible at once and durable on `commit`; `batch` appends all or nothing.

## Record log usage

```rust
use gdblite::RecordLog;

// A growable log whose first entry has id 1.
let mut log = RecordLog::new(1);

let alice = log.push_value(b"alice");
let bob = log.push_value(b"bob");
let knows = log.push_value(b"knows");
let edge = log.push_tuple(false, &[alice, knows, bob]).unwrap();

// Read entries back by id.
assert_eq!(log.blob(alice).unwrap(), b"alice");
let (ids, _extra) = log.tuple(edge).unwrap().decode(edge);
assert_eq!(ids, vec![alice, knows, bob]);

// Walk the whole log in insertion order.
let mut id = log.start_id();
while id < log.end_id() {
    let entry = log.entry(id).unwrap();
    id += entry.encoded_len() as u64;
}

// Persist the bytes and reopen them later, e.g. from a file or mmap.
let bytes = log.into_inner();
let reopened = RecordLog::adopt(&bytes[..]).unwrap();
assert!(reopened.is_valid());
assert_eq!(reopened.blob(bob).unwrap(), b"bob");
```

`RecordLog<B>` works over any buffer:

- `B: AsRef<[u8]>` (such as `&[u8]` or a memory map) can be read.
- `B: AsMut<[u8]>` (such as `&mut [u8]`) can also be written in place with
  `make_node`, `make_value` and `make_tuple` at ids you choose.
- `RecordLog<Vec<u8>>` grows as you append with `push_node`, `push_value` and
  `push_tuple`.

All decoding is bounds checked. A truncated or corrupt log returns an
`Error` and never reads out of bounds. `Entry::complete_len` and
`Tuple::estimate_len` help when you consume a log that is still arriving.

## Dynamic table

`dynamic_table` is a persistent hash table from a key (`key`, plus an optional
`key2` for 128 bit keys) to an extra bit and a list of non-zero `u64` values.
It has one `Writer` and any number of `Reader`s, which take no locks and can
look up keys on other threads while the writer inserts.

```rust
use gdblite::dynamic_table::{Options, Writer};

// Pass Some(path_prefix) for a table on disk, or None for one in memory.
let mut table = Writer::open(None, Options::default()).unwrap();
table.insert(42, 0, false, &[7, 8]).unwrap();  // append values
table.insert(42, 0, true, &[9]).unwrap();
table.replace(43, 99, false, &[1]).unwrap();   // replace a 128 bit key's values
table.delete(43, 99).unwrap();

let reader = table.reader();                   // Clone + Send + Sync
let found = reader.get(42, 0).unwrap();
assert_eq!((found.bit, found.values), (true, vec![7, 8, 9]));
assert!(reader.get(43, 99).is_none());
```

Each level of the table is a file of 128 byte buckets. Each level has twice as
many buckets as the one before. A key's operations are appended to its bucket
as compact varint records, and each bucket links to an overflow bucket when it
fills. When too many buckets overflow, the table moves to the next level a few
buckets at a time. Each level is mapped at a fixed address with room reserved to
grow, so growing never moves memory that readers are using. An old level is
unmapped `Options::retire_delay` after the move finishes (5 minutes by default).

It is ported from a C++ original in `reference/`, with its bugs fixed: deletes
no longer corrupt later records, re-inserts no longer truncate buckets, a key
can hold any number of values, readers survive file growth, and an interrupted
expansion resumes. It runs on 64-bit Unix only, and one process at a time; a
lock file stops a second writer.

## Record log format

All multi-byte integers are little endian.

### Header (16 bytes)

| Offset | Size | Field         | Meaning                                         |
| -----: | ---: | ------------- | ----------------------------------------------- |
| 0      | 4    | `magic`       | `0xe3e0ba2f`                                    |
| 4      | 1    | `version`     | Record log version, currently `0`               |
| 5      | 1    | `compression` | `0` = none, `1` = V1                            |
| 6      | 1    | `written`     | `0` = unwritten, `1`+ = restorable              |
| 7      | 1    | `reserved`    | Must be zero                                    |
| 8      | 8    | `start_id`    | Id of the first entry; `1` for a root database  |

The body follows immediately, at offset 16.

### Tagged variable integer

Every entry begins with an unsigned integer whose top bit is a tag: `0` for a
value, `1` for a tuple. The first byte is `[tag:1][more:1][value:6]` and each
following byte is `[more:1][value:7]`, least significant group first. Seven
bytes hold any 48 bit length.

### Group variable integer

A tuple's ids are stored in groups of five. Any remaining one to four ids form a
last, shorter group. Each group is a length prefix followed by each integer in
its minimal number of little-endian bytes (1 to 8):

- Groups of 1 or 2 ids have a 1 byte prefix. Groups of 3 to 5 ids have a 2 byte prefix.
- Bit 7 of the first prefix byte is the tuple's **extra bit**. Only the first
  group carries it.
- The other prefix bits hold each integer's `byte length - 1` in 3 bits, in order.

### Id encoding

Each id is stored relative to a base, normally the tuple's own id, and zigzag
encoded. An earlier id may be stored as a negative offset from the base (odd
values). Otherwise it is stored as its absolute value (even values). The
encoder picks whichever is smaller, so ids must be at most `i64::MAX`.

## Project layout

```text
src/
  lib.rs              crate root and public re-exports
  record_log.rs       RecordLog, PersistentHeader, Entry, Value, Tuple
  record.rs           Record (an entry and its id), links, RecordSet
  coder.rs            tagged / group variable integers and id encoding
  error.rs            Error and Result
  mmap.rs             memory mapping and file helpers
  dynamic_table/      persistent hash table: Writer, Reader (mod.rs), record
                      format, tests
  local_record_log.rs record log in memory mapped files, with commits and
                      snapshots (tests in local_record_log/)
  local_table.rs      index from ids or uuids to id sets, on a dynamic table
  local.rs            local database: a log plus index tables (tests in local/)
  tuple_index.rs      built-in graph indexes: links by linkage, values, SPO,
                      intersections (tests in tuple_index/)
  graph.rs            Graph: the graph database API (tests in graph/)
benches/
  record_log.rs       encode/decode benchmarks
reference/            original C++ sources ported to Rust
```

## Building and testing

```sh
cargo build
cargo test          # unit tests, plus the example in this README
cargo clippy --all-targets
cargo bench         # record log encode/decode benchmarks (criterion)
cargo doc --open
```

gdblite's runtime dependencies are [libc](https://crates.io/crates/libc), for
memory mapping, and [sha2](https://crates.io/crates/sha2), for value uuids.
[criterion](https://crates.io/crates/criterion) is used for benchmarks only.
