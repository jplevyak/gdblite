//! An index from ids or [`Uuid`]s to sets of ids, stored in a
//! [`dynamic_table`](crate::dynamic_table).
//!
//! Replaces the C++ `LocalTable`, whose in-memory `LocalIDSetMap` and
//! `LocalSets` storage are unnecessary here: a dynamic table key holds any
//! number of values, and an in-memory dynamic table serves transient indexes.
//!
//! An id key `k` is the same key as the [`Uuid`] `{ uuid0: k, uuid1: 0 }`, as
//! in the C++.  Keys and ids must be non-zero.
//!
//! A key holds either a set of ids, built up by [`insert`](LocalTable::insert),
//! or a single id, set by [`set`](LocalTable::set); a table should use each
//! key one way.  Sets are not checked for duplicates when inserting, since
//! that would cost a lookup per insert; duplicates are dropped when reading.

use std::collections::BTreeSet;
use std::io;
use std::path::Path;

pub use crate::dynamic_table::{Error, Options, Result, exists, remove};
use crate::dynamic_table::{Lookup, MAX_VALUES, Reader, Writer};
use crate::record::Uuid;

/// A set of ids, in order.
pub type IdSet = BTreeSet<u64>;

/// A table key: an id or a [`Uuid`].
pub trait Key: Copy {
    /// The dynamic table key and second key.
    fn parts(self) -> (u64, u64);
}

impl Key for u64 {
    fn parts(self) -> (u64, u64) {
        (self, 0)
    }
}

impl Key for Uuid {
    fn parts(self) -> (u64, u64) {
        (self.uuid0, self.uuid1)
    }
}

fn to_set(lookup: Option<Lookup>) -> IdSet {
    lookup
        .map(|l| l.values.into_iter().collect())
        .unwrap_or_default()
}

fn to_one(lookup: Option<Lookup>) -> Option<u64> {
    lookup.and_then(|l| l.values.first().copied())
}

/// The single writer of a table.
pub struct LocalTable {
    writer: Writer,
}

impl LocalTable {
    /// Opens or creates the table whose files start with `prefix`, or an
    /// in-memory table if `prefix` is `None`.
    pub fn open(prefix: Option<&Path>, options: Options) -> Result<Self> {
        Ok(LocalTable {
            writer: Writer::open(prefix, options)?,
        })
    }

    /// A reader for looking up keys, on any thread, while this table changes.
    pub fn reader(&self) -> TableReader {
        TableReader {
            reader: self.writer.reader(),
        }
    }

    /// Adds `ids` to the set at `key`.
    pub fn insert<K: Key>(&mut self, key: K, ids: impl IntoIterator<Item = u64>) -> Result<()> {
        let (k, k2) = key.parts();
        let ids: Vec<u64> = ids.into_iter().collect::<IdSet>().into_iter().collect();
        for chunk in ids.chunks(MAX_VALUES) {
            self.writer.insert(k, k2, false, chunk)?;
        }
        Ok(())
    }

    /// Makes `id` the single id at `key`.
    pub fn set<K: Key>(&mut self, key: K, id: u64) -> Result<()> {
        let (k, k2) = key.parts();
        self.writer.replace(k, k2, false, &[id])
    }

    /// Removes `key` and its ids.
    pub fn delete<K: Key>(&mut self, key: K) -> Result<()> {
        let (k, k2) = key.parts();
        self.writer.delete(k, k2)
    }

    /// The set at `key`, empty if there is none.
    pub fn find<K: Key>(&self, key: K) -> IdSet {
        let (k, k2) = key.parts();
        to_set(self.writer.get(k, k2))
    }

    /// The single id at `key` (or the first inserted, if it holds a set).
    pub fn find_one<K: Key>(&self, key: K) -> Option<u64> {
        let (k, k2) = key.parts();
        to_one(self.writer.get(k, k2))
    }

    /// Does periodic maintenance; see [`Writer::tick`].
    pub fn tick(&mut self) -> Result<()> {
        self.writer.tick()
    }

    /// Flushes the table to disk.
    pub fn sync(&self) -> Result<()> {
        self.writer.sync()
    }

    /// Writes a snapshot of the table to `sink`; see [`Writer::snapshot`].
    pub fn snapshot(&mut self, sink: impl FnMut(&[u8]) -> io::Result<()>) -> Result<()> {
        self.writer.snapshot(sink)
    }

    /// Creates a table at `prefix`, or in memory if `prefix` is `None`, from
    /// the chunks of a [`snapshot`](Self::snapshot); see [`Writer::restore`].
    pub fn restore(
        prefix: Option<&Path>,
        options: Options,
        source: impl FnMut() -> io::Result<Vec<u8>>,
    ) -> Result<Self> {
        Ok(LocalTable {
            writer: Writer::restore(prefix, options, source)?,
        })
    }
}

/// Looks up keys in a [`LocalTable`]; see [`LocalTable::reader`].
#[derive(Clone)]
pub struct TableReader {
    reader: Reader,
}

impl TableReader {
    /// The set at `key`, empty if there is none.
    pub fn find<K: Key>(&self, key: K) -> IdSet {
        let (k, k2) = key.parts();
        to_set(self.reader.get(k, k2))
    }

    /// The single id at `key` (or the first inserted, if it holds a set).
    pub fn find_one<K: Key>(&self, key: K) -> Option<u64> {
        let (k, k2) = key.parts();
        to_one(self.reader.get(k, k2))
    }
}

#[cfg(test)]
mod tests {
    use std::collections::VecDeque;
    use std::fs;
    use std::path::PathBuf;

    use super::*;

    /// A temporary directory removed on drop.
    struct TempDir(PathBuf);

    impl TempDir {
        fn new(name: &str) -> Self {
            let dir =
                std::env::temp_dir().join(format!("gdblite-lt-{}-{name}", std::process::id()));
            let _ = fs::remove_dir_all(&dir);
            fs::create_dir_all(&dir).unwrap();
            TempDir(dir)
        }

        fn prefix(&self, name: &str) -> PathBuf {
            self.0.join(format!("{name}."))
        }
    }

    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn set(ids: &[u64]) -> IdSet {
        ids.iter().copied().collect()
    }

    #[test]
    fn sets() {
        let mut t = LocalTable::open(None, Options::default()).unwrap();
        assert!(t.find(1u64).is_empty());
        t.insert(1u64, [5, 3, 9]).unwrap();
        t.insert(1u64, [3, 1]).unwrap();
        t.insert(1u64, []).unwrap();
        assert_eq!(t.find(1u64), set(&[1, 3, 5, 9]));

        // More than fit in one dynamic table operation, with duplicates.
        let many: Vec<u64> = (1..=100).rev().chain(1..=50).collect();
        t.insert(2u64, many).unwrap();
        assert_eq!(t.find(2u64), (1..=100).collect());
        assert_eq!(t.find_one(2u64), Some(1));

        t.delete(1u64).unwrap();
        assert!(t.find(1u64).is_empty());
        assert_eq!(t.find_one(1u64), None);

        assert!(matches!(
            t.insert(0u64, [1]),
            Err(Error::InvalidArgument(_))
        ));
        assert!(matches!(
            t.insert(3u64, [1, 0]),
            Err(Error::InvalidArgument(_))
        ));
    }

    #[test]
    fn single() {
        let mut t = LocalTable::open(None, Options::default()).unwrap();
        t.set(7u64, 70).unwrap();
        assert_eq!(t.find_one(7u64), Some(70));
        t.set(7u64, 71).unwrap();
        assert_eq!(t.find_one(7u64), Some(71));
        assert_eq!(t.find(7u64), set(&[71]));
        assert!(matches!(t.set(7u64, 0), Err(Error::InvalidArgument(_))));
    }

    #[test]
    fn uuid_keys() {
        let mut t = LocalTable::open(None, Options::default()).unwrap();
        let u = Uuid::of(b"alice");
        t.set(u, 10).unwrap();
        t.insert(Uuid { uuid0: 4, uuid1: 1 }, [1, 2]).unwrap();
        t.insert(4u64, [3]).unwrap();
        assert_eq!(t.find_one(u), Some(10));
        assert_eq!(t.find(Uuid { uuid0: 4, uuid1: 1 }), set(&[1, 2]));
        assert_eq!(t.find(4u64), set(&[3]));
        // An id key is the uuid with a zero second half.
        assert_eq!(t.find(Uuid { uuid0: 4, uuid1: 0 }), set(&[3]));
    }

    #[test]
    fn reader() {
        let mut t = LocalTable::open(None, Options::default()).unwrap();
        let r = t.reader();
        t.insert(1u64, [1, 2]).unwrap();
        t.set(2u64, 9).unwrap();
        std::thread::spawn(move || {
            assert_eq!(r.find(1u64), set(&[1, 2]));
            assert_eq!(r.find_one(2u64), Some(9));
            assert!(r.find(3u64).is_empty());
        })
        .join()
        .unwrap();
    }

    #[test]
    fn persistent() {
        let dir = TempDir::new("persistent");
        let prefix = dir.prefix("index");
        let mut t = LocalTable::open(Some(&prefix), Options::default()).unwrap();
        for k in 1..=2000u64 {
            t.insert(k, (1..=k % 20 + 1).map(|i| i * k)).unwrap();
            t.tick().unwrap();
        }
        t.set(Uuid::of(b"v"), 42).unwrap();
        t.sync().unwrap();
        drop(t);

        let check = |t: &LocalTable| {
            for k in 1..=2000u64 {
                assert_eq!(
                    t.find(k),
                    (1..=k % 20 + 1).map(|i| i * k).collect(),
                    "key {k}"
                );
            }
            assert_eq!(t.find_one(Uuid::of(b"v")), Some(42));
        };
        let mut t = LocalTable::open(Some(&prefix), Options::default()).unwrap();
        check(&t);

        let mut chunks = VecDeque::new();
        t.snapshot(|c| {
            chunks.push_back(c.to_vec());
            Ok(())
        })
        .unwrap();
        let copy = dir.prefix("copy");
        let restored = LocalTable::restore(Some(&copy), Options::default(), || {
            Ok(chunks.pop_front().unwrap_or_default())
        })
        .unwrap();
        check(&restored);
        drop(t);
        remove(&prefix).unwrap();
        assert!(!exists(&prefix));
        assert!(exists(&copy));
    }
}
