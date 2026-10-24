# gdblite

A lightweight graph database written in Rust.

gdblite stores a graph as an append-only **record log**: a compact byte log of
nodes, values and tuples where every entry is addressed by its position in the
log. Edges are tuples of ids pointing back at earlier entries, encoded relative
to the tuple's own position so that nearby references take only a byte or two.

> **Status:** early development. The record log storage format is in place;
> the graph query layer is still to come.

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

## Usage

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

## On-disk format

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
  lib.rs         crate root and public re-exports
  record_log.rs  RecordLog, PersistentHeader, Entry, Value, Tuple
  coder.rs       tagged / group variable integers and id encoding
  error.rs       Error and Result
reference/       original C++ sources the record log was ported from
```

## Building and testing

```sh
cargo build
cargo test          # unit tests, plus the example in this README
cargo clippy --all-targets
cargo doc --open
```

gdblite has no dependencies outside the Rust standard library.
