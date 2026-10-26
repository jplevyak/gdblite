//! The built-in indexes of a graph: links by each linkage, values by content,
//! links by (subject, predicate, object), and links by pairs of linkages.
//!
//! Ported from the C++ `Indexes`, without its schema-specific compound index.
//! Each [`BuiltinIndex`] is the [`Local`] table of the same number.
//!
//! Fixed from the C++: every subgraph of a link is indexed, not just the
//! first; a duplicate value is an error rather than a crash; and finding by
//! intersection gives the latest link both in memory and on disk.
//!
//! After a crash an index may name ids that are now filler nodes (see
//! [`local`](crate::local)); callers should check what the ids they find
//! name.

use std::collections::{BTreeMap, HashMap};
use std::fmt;

use crate::dynamic_table;
use crate::local::Local;
use crate::local_record_log;
use crate::local_table::IdSet;
use crate::record::{Linkage, Record, Uuid};

/// The built-in indexes, numbered as tables of a [`Local`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum BuiltinIndex {
    /// Links by subject.
    Subject,
    /// Links by predicate.
    Predicate,
    /// Links by object.
    Object,
    /// Links by author.
    Author,
    /// Links by each of their subgraphs.
    Subgraph,
    /// Each value by the [`Uuid`] of its data.
    Value,
    /// Links by [`spo_key`].
    Spo,
    /// Links by [`intersection_key`] of subject and predicate.
    SubjectPredicate,
    /// Links by [`intersection_key`] of predicate and object.
    PredicateObject,
}

/// The number of built-in indexes.
pub const NUM_BUILTIN_INDEXES: usize = 9;

impl BuiltinIndex {
    /// The table number.
    pub const fn table(self) -> usize {
        self as usize
    }

    /// The index of links by `linkage`.  `When` is not a reference and has
    /// none; it maps to `Subgraph`, as in the C++.
    pub fn for_linkage(linkage: Linkage) -> BuiltinIndex {
        match linkage {
            Linkage::Subject => BuiltinIndex::Subject,
            Linkage::Predicate => BuiltinIndex::Predicate,
            Linkage::Object => BuiltinIndex::Object,
            Linkage::Author => BuiltinIndex::Author,
            Linkage::When | Linkage::Subgraph => BuiltinIndex::Subgraph,
        }
    }
}

const _: () = assert!(BuiltinIndex::PredicateObject as usize + 1 == NUM_BUILTIN_INDEXES);

/// The indexes of links by a pair of linkages.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Intersection {
    SubjectPredicate,
    PredicateObject,
}

impl Intersection {
    /// The intersection index of `x` and `y`, if there is one.
    pub fn of(x: Linkage, y: Linkage) -> Option<Intersection> {
        match (x, y) {
            (Linkage::Subject, Linkage::Predicate) => Some(Intersection::SubjectPredicate),
            (Linkage::Predicate, Linkage::Object) => Some(Intersection::PredicateObject),
            _ => None,
        }
    }

    pub fn index(self) -> BuiltinIndex {
        match self {
            Intersection::SubjectPredicate => BuiltinIndex::SubjectPredicate,
            Intersection::PredicateObject => BuiltinIndex::PredicateObject,
        }
    }
}

/// Errors from indexing.
#[derive(Debug)]
pub enum Error {
    Index(dynamic_table::Error),
    Log(local_record_log::Error),
    /// A value's data is already the value at `existing` (or has the same
    /// uuid); values are interned, so each distinct data may appear once.
    DuplicateValue {
        id: u64,
        existing: u64,
    },
}

pub type Result<T> = std::result::Result<T, Error>;

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Index(e) => write!(f, "index: {e}"),
            Error::Log(e) => write!(f, "record log: {e}"),
            Error::DuplicateValue { id, existing } => {
                write!(f, "value {id} duplicates value {existing}")
            }
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Error::Index(e) => Some(e),
            Error::Log(e) => Some(e),
            Error::DuplicateValue { .. } => None,
        }
    }
}

impl From<dynamic_table::Error> for Error {
    fn from(e: dynamic_table::Error) -> Self {
        Error::Index(e)
    }
}

impl From<local_record_log::Error> for Error {
    fn from(e: local_record_log::Error) -> Self {
        Error::Log(e)
    }
}

fn put_varint(out: &mut Vec<u8>, mut v: u64) {
    while v >= 0x80 {
        out.push(v as u8 | 0x80);
        v >>= 7;
    }
    out.push(v as u8);
}

/// The key for an encoding: the encoding itself if it fits, zero padded, or
/// else its hash.  A key whose first half is zero, which a table cannot
/// hold, is hashed too.
fn small_uuid(bytes: &[u8]) -> Uuid {
    if bytes.len() <= 16 {
        let mut b = [0u8; 16];
        b[..bytes.len()].copy_from_slice(bytes);
        let uuid = Uuid {
            uuid0: u64::from_le_bytes(b[..8].try_into().unwrap()),
            uuid1: u64::from_le_bytes(b[8..].try_into().unwrap()),
        };
        if uuid.uuid0 != 0 {
            return uuid;
        }
    }
    Uuid::of(bytes)
}

/// The key of a link's subject, predicate and object.
pub fn spo_key(subject: u64, predicate: u64, object: u64) -> Uuid {
    let mut b = Vec::with_capacity(30);
    for id in [subject, predicate, object] {
        put_varint(&mut b, id);
    }
    small_uuid(&b)
}

/// The key of a pair of linkages.
pub fn intersection_key(x: u64, y: u64) -> Uuid {
    Uuid { uuid0: x, uuid1: y }
}

/// The index entries for a batch of records.
#[derive(Default)]
struct Updates {
    values: Vec<(Uuid, u64)>,
    /// Ids to add to sets, by index and key.
    sets: BTreeMap<(usize, Uuid), Vec<u64>>,
}

impl Updates {
    fn add(&mut self, index: BuiltinIndex, key: Uuid, id: u64) {
        self.sets.entry((index.table(), key)).or_default().push(id);
    }

    fn record(&mut self, r: &Record<'_>) {
        if r.is_value() {
            let data = r.data().expect("a value");
            self.values.push((Uuid::of(data), r.id()));
        }
        let Some(link) = r.as_link() else {
            return;
        };
        let id = r.id();
        let (s, p, o) = (link.subject(), link.predicate(), link.object());
        self.add(BuiltinIndex::Spo, spo_key(s, p, o), id);
        for (i, field) in link.ids().into_iter().enumerate() {
            let linkage = Linkage::of_field(i);
            if field != 0 && linkage != Linkage::When {
                self.add(BuiltinIndex::for_linkage(linkage), field.into(), id);
            }
        }
        // A table key's first half must be non-zero.
        if s != 0 {
            self.add(BuiltinIndex::SubjectPredicate, intersection_key(s, p), id);
        }
        if p != 0 {
            self.add(BuiltinIndex::PredicateObject, intersection_key(p, o), id);
        }
    }

    fn apply(self, local: &mut Local) -> Result<()> {
        // Check every value before writing anything.  An existing entry that
        // no longer names a value is left from a crash and is replaced.
        let values = local.table(BuiltinIndex::Value.table());
        let mut seen = HashMap::new();
        for &(uuid, id) in &self.values {
            let existing = *seen.entry(uuid).or_insert(id);
            let existing = values
                .find_one(uuid)
                .filter(|&e| local.log().get(e).is_ok_and(|r| r.is_value()))
                .unwrap_or(existing);
            if existing != id {
                return Err(Error::DuplicateValue { id, existing });
            }
        }
        for (uuid, id) in self.values {
            local.table_mut(BuiltinIndex::Value.table()).set(uuid, id)?;
        }
        for ((table, key), ids) in self.sets {
            local.table_mut(table).insert(key, ids)?;
        }
        Ok(())
    }
}

/// Indexes `records`.  Fails without indexing anything if a value
/// duplicates another.
pub fn index_records(local: &mut Local, records: &[Record<'_>]) -> Result<()> {
    let mut updates = Updates::default();
    for r in records {
        updates.record(r);
    }
    updates.apply(local)
}

/// Indexes the log's entries from `start` (which must start an entry) to the
/// end; see [`index_records`].
pub fn index_log(local: &mut Local, start: u64) -> Result<()> {
    let mut updates = Updates::default();
    for r in local.log().records(start) {
        updates.record(&r?);
    }
    updates.apply(local)
}

/// The links with `id` at `linkage`.
pub fn find_links(local: &Local, linkage: Linkage, id: u64) -> IdSet {
    local
        .table(BuiltinIndex::for_linkage(linkage).table())
        .find(id)
}

/// The value holding `data`.
pub fn find_value(local: &Local, data: &[u8]) -> Option<u64> {
    find_value_by_uuid(local, Uuid::of(data))
}

/// The value whose data has `uuid`.
pub fn find_value_by_uuid(local: &Local, uuid: Uuid) -> Option<u64> {
    local.table(BuiltinIndex::Value.table()).find_one(uuid)
}

/// The links with `subject`, `predicate` and `object`.
pub fn find_links_by_spo(local: &Local, subject: u64, predicate: u64, object: u64) -> IdSet {
    local
        .table(BuiltinIndex::Spo.table())
        .find(spo_key(subject, predicate, object))
}

/// The latest link with `subject`, `predicate` and `object`.
pub fn find_link_by_spo(local: &Local, subject: u64, predicate: u64, object: u64) -> Option<u64> {
    find_links_by_spo(local, subject, predicate, object)
        .last()
        .copied()
}

/// The links with `x` and `y` at the linkages of `intersection`.
pub fn find_links_by_intersection(
    local: &Local,
    intersection: Intersection,
    x: u64,
    y: u64,
) -> IdSet {
    local
        .table(intersection.index().table())
        .find(intersection_key(x, y))
}

/// The latest link with `x` and `y` at the linkages of `intersection`.
pub fn find_link_by_intersection(
    local: &Local,
    intersection: Intersection,
    x: u64,
    y: u64,
) -> Option<u64> {
    find_links_by_intersection(local, intersection, x, y)
        .last()
        .copied()
}

#[cfg(test)]
mod tests;
