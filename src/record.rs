//! Records: record log entries paired with their ids and read as the nodes,
//! values and links of a graph.
//!
//! A link is a tuple whose fields follow a fixed layout (see [`Linkage`]):
//! subject, predicate, object, author, the creation time (`when`), then any
//! number of subgraphs.  Its extra bit marks it dead.
//!
//! A [`Record`] either borrows its entry from a log or shares an owned copy;
//! [`into_owned`](Record::into_owned) lets a record outlive the log it was
//! read from.  A tuple's ids are encoded relative to its own id, so giving a
//! link a new id re-encodes it.

use std::collections::{BTreeMap, HashMap};
use std::fmt;
use std::ops::RangeBounds;
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};

use sha2::{Digest, Sha256};

use crate::record_log::{
    Entry, RecordLog, Tuple, WHEN_FIELD, encode_tuple, encode_value, max_tuple_len, max_value_len,
};
use crate::{Error, Result};

/// A link's creation time in milliseconds since the Unix epoch.
pub type LinkTime = u64;

pub const LINK_TIME_PER_SECOND: u64 = 1000;

/// The fields of a link.  Fields from [`Subgraph`](Linkage::Subgraph) on are
/// all subgraphs.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Linkage {
    Subject,
    Predicate,
    Object,
    Author,
    /// The creation time, not a reference to another entry.
    When,
    Subgraph,
}

const _: () = assert!(Linkage::When as usize == WHEN_FIELD);

impl Linkage {
    /// The linkages that refer to other entries: all but `When`.
    pub const ALL: [Linkage; 5] = [
        Linkage::Subject,
        Linkage::Predicate,
        Linkage::Object,
        Linkage::Author,
        Linkage::Subgraph,
    ];

    /// The tuple field index of the linkage (the first, for subgraphs).
    pub const fn field(self) -> usize {
        self as usize
    }

    /// The linkage of tuple field `i`.
    pub fn of_field(i: usize) -> Linkage {
        match i {
            0 => Linkage::Subject,
            1 => Linkage::Predicate,
            2 => Linkage::Object,
            3 => Linkage::Author,
            4 => Linkage::When,
            _ => Linkage::Subgraph,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Linkage::Subject => "subject",
            Linkage::Predicate => "predicate",
            Linkage::Object => "object",
            Linkage::Author => "author",
            Linkage::When => "when",
            Linkage::Subgraph => "subgraph",
        }
    }

    /// Subject and object are each other's inverse; nothing else has one.
    pub fn inverse(self) -> Option<Linkage> {
        match self {
            Linkage::Subject => Some(Linkage::Object),
            Linkage::Object => Some(Linkage::Subject),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Hash)]
pub enum LinkState {
    #[default]
    Live,
    Dead,
}

/// A 128 bit identifier, such as the hash of a value.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Uuid {
    pub uuid0: u64,
    pub uuid1: u64,
}

impl Uuid {
    /// The SHA-256 of `data` folded to 128 bits.
    pub fn of(data: &[u8]) -> Uuid {
        let digest = Sha256::digest(data);
        let word = |i: usize| u64::from_le_bytes(digest[i * 8..i * 8 + 8].try_into().unwrap());
        Uuid {
            uuid0: word(0) ^ word(1),
            uuid1: word(2) ^ word(3),
        }
    }
}

impl From<u64> for Uuid {
    /// The uuid of an id, as a table key: `{ uuid0: id, uuid1: 0 }`.
    fn from(id: u64) -> Self {
        Uuid {
            uuid0: id,
            uuid1: 0,
        }
    }
}

/// The current time as a [`LinkTime`].
pub fn now() -> LinkTime {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_millis() as LinkTime)
}

/// The encoding of a node: an empty value.
const NODE: &[u8] = &[0];

#[derive(Clone)]
enum Bytes<'a> {
    Node,
    Borrowed(&'a [u8]),
    Shared(Arc<[u8]>),
}

/// An entry of a record log and its id.
///
/// The bytes are always exactly one complete, valid entry.
#[derive(Clone)]
pub struct Record<'a> {
    id: u64,
    bytes: Bytes<'a>,
}

impl Record<'static> {
    pub fn node(id: u64) -> Self {
        Record {
            id,
            bytes: Bytes::Node,
        }
    }

    /// A value holding `data`, or a node if `data` is empty.
    pub fn value(id: u64, data: &[u8]) -> Self {
        if data.is_empty() {
            return Record::node(id);
        }
        let mut buf = vec![0; max_value_len(data.len())];
        let n = encode_value(&mut buf, data).expect("buffer sized for value");
        buf.truncate(n);
        Record {
            id,
            bytes: Bytes::Shared(buf.into()),
        }
    }

    /// A tuple of `ids` with the extra bit `dead`, encoded relative to `id`.
    /// Fails with [`Error::Overflow`] if an id exceeds `i64::MAX`.
    pub fn tuple(id: u64, dead: bool, ids: &[u64]) -> Result<Self> {
        let mut buf = vec![0; max_tuple_len(ids.len())];
        let n = encode_tuple(&mut buf, id, dead, ids)?;
        buf.truncate(n);
        Ok(Record {
            id,
            bytes: Bytes::Shared(buf.into()),
        })
    }

    /// A link.  Fails with [`Error::ForwardReference`] unless every field
    /// but `when` is less than `id`.
    pub fn link(id: u64, fields: &LinkFields) -> Result<Self> {
        let ids = fields.ids();
        if !refers_back(id, &ids) {
            return Err(Error::ForwardReference);
        }
        Record::tuple(id, fields.state == LinkState::Dead, &ids)
    }
}

fn refers_back(id: u64, ids: &[u64]) -> bool {
    ids.iter()
        .enumerate()
        .all(|(i, &field)| i == WHEN_FIELD || field < id)
}

impl<'a> Record<'a> {
    /// The record at `id` whose entry is at the front of `buf`, which may
    /// hold more after it.
    pub fn from_bytes(id: u64, buf: &'a [u8]) -> Result<Self> {
        let n = Entry::parse(buf)?.encoded_len();
        Ok(Record {
            id,
            bytes: Bytes::Borrowed(&buf[..n]),
        })
    }

    /// The record at `id` in `log`, borrowed from it.
    pub fn from_log<B: AsRef<[u8]>>(log: &'a RecordLog<B>, id: u64) -> Result<Self> {
        Record::from_bytes(id, log.entry_bytes(id)?)
    }

    pub fn id(&self) -> u64 {
        self.id
    }

    /// The encoded entry.
    pub fn bytes(&self) -> &[u8] {
        match &self.bytes {
            Bytes::Node => NODE,
            Bytes::Borrowed(b) => b,
            Bytes::Shared(b) => b,
        }
    }

    /// The number of bytes the record occupies in a log.
    pub fn encoded_len(&self) -> u64 {
        self.bytes().len() as u64
    }

    /// The id of the entry following this one in a log.
    pub fn next_id(&self) -> u64 {
        self.id + self.encoded_len()
    }

    pub fn entry(&self) -> Entry<'_> {
        Entry::parse(self.bytes()).expect("validated entry")
    }

    pub fn is_node(&self) -> bool {
        matches!(self.entry(), Entry::Value(v) if v.is_empty())
    }

    /// Whether the record is a non-empty value.
    pub fn is_value(&self) -> bool {
        matches!(self.entry(), Entry::Value(v) if !v.is_empty())
    }

    pub fn is_link(&self) -> bool {
        self.entry().is_tuple()
    }

    /// The data of a value, empty for a node, or `None` for a link.
    pub fn data(&self) -> Option<&[u8]> {
        self.entry().as_value().map(|v| v.as_bytes())
    }

    pub fn as_link(&self) -> Option<Link<'_>> {
        self.entry()
            .as_tuple()
            .map(|&tuple| Link { id: self.id, tuple })
    }

    /// The [`Uuid`] of a value's (or node's) data.
    pub fn value_uuid(&self) -> Option<Uuid> {
        self.data().map(Uuid::of)
    }

    /// The record with any borrowed bytes copied, so it no longer refers to
    /// the log it was read from.  Shared bytes are not copied.
    pub fn into_owned(self) -> Record<'static> {
        let bytes = match self.bytes {
            Bytes::Node => Bytes::Node,
            Bytes::Borrowed(b) => Bytes::Shared(b.into()),
            Bytes::Shared(b) => Bytes::Shared(b),
        };
        Record { id: self.id, bytes }
    }

    fn decoded_link(&self) -> Result<(Vec<u64>, bool)> {
        let link = self.as_link().ok_or(Error::KindMismatch)?;
        Ok((link.ids(), link.dead()))
    }

    /// The record moved to `new_id`.  A link is re-encoded relative to its
    /// new id; a node or value shares its bytes.
    pub fn with_id(&self, new_id: u64) -> Result<Record<'a>> {
        if let Some(link) = self.as_link() {
            return Record::tuple(new_id, link.dead(), &link.ids());
        }
        Ok(Record {
            id: new_id,
            bytes: self.bytes.clone(),
        })
    }

    /// A link with field `linkage` (the first subgraph, for `Subgraph`) set
    /// to `id`, adding zero fields if the link is too short to hold it.
    pub fn with_linkage(&self, linkage: Linkage, id: u64) -> Result<Record<'static>> {
        let (mut ids, dead) = self.decoded_link()?;
        let i = linkage.field();
        if ids.len() <= i {
            ids.resize(i + 1, 0);
        }
        ids[i] = id;
        Record::tuple(self.id, dead, &ids)
    }

    /// A link with its creation time set to `when`.  Nodes and values have
    /// no creation time and are returned unchanged.
    pub fn with_when(&self, when: LinkTime) -> Result<Record<'a>> {
        if self.is_link() {
            self.with_linkage(Linkage::When, when)
        } else {
            Ok(self.clone())
        }
    }

    pub fn with_state(&self, state: LinkState) -> Result<Record<'static>> {
        let (ids, _) = self.decoded_link()?;
        Record::tuple(self.id, state == LinkState::Dead, &ids)
    }
}

/// Records are equal when their ids and encodings are; see [`same_content`]
/// to compare what they hold.
impl PartialEq for Record<'_> {
    fn eq(&self, other: &Self) -> bool {
        self.id == other.id && self.bytes() == other.bytes()
    }
}

impl Eq for Record<'_> {}

impl fmt::Debug for Record<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.entry() {
            Entry::Value(v) if v.is_empty() => {
                f.debug_struct("Node").field("id", &self.id).finish()
            }
            Entry::Value(v) => f
                .debug_struct("Value")
                .field("id", &self.id)
                .field("data", &String::from_utf8_lossy(v.as_bytes()))
                .finish(),
            Entry::Tuple(t) => {
                let (ids, dead) = t.decode(self.id);
                f.debug_struct("Link")
                    .field("id", &self.id)
                    .field("dead", &dead)
                    .field("ids", &ids)
                    .finish()
            }
        }
    }
}

/// Whether two records hold the same node, value or link ids, regardless of
/// their own ids.  A link's dead state is ignored.
pub fn same_content(a: &Record<'_>, b: &Record<'_>) -> bool {
    match (a.entry(), b.entry()) {
        (Entry::Value(x), Entry::Value(y)) => x.as_bytes() == y.as_bytes(),
        (Entry::Tuple(x), Entry::Tuple(y)) => {
            x.len() == y.len() && x.decode(a.id).0 == y.decode(b.id).0
        }
        _ => false,
    }
}

/// A view of a link record.
#[derive(Debug, Clone, Copy)]
pub struct Link<'r> {
    id: u64,
    tuple: Tuple<'r>,
}

impl Link<'_> {
    pub fn id(&self) -> u64 {
        self.id
    }

    /// The number of fields.
    pub fn len(&self) -> usize {
        self.tuple.len()
    }

    pub fn is_empty(&self) -> bool {
        self.tuple.is_empty()
    }

    /// Field `i`, or 0 (no entry) if the link is too short to have it.
    pub fn field(&self, i: usize) -> u64 {
        self.tuple.id(i, self.id).unwrap_or(0)
    }

    /// The field for `linkage` (the first subgraph, for `Subgraph`).
    pub fn get(&self, linkage: Linkage) -> u64 {
        self.field(linkage.field())
    }

    pub fn subject(&self) -> u64 {
        self.get(Linkage::Subject)
    }

    pub fn predicate(&self) -> u64 {
        self.get(Linkage::Predicate)
    }

    pub fn object(&self) -> u64 {
        self.get(Linkage::Object)
    }

    pub fn author(&self) -> u64 {
        self.get(Linkage::Author)
    }

    pub fn when(&self) -> LinkTime {
        self.get(Linkage::When)
    }

    pub fn dead(&self) -> bool {
        self.tuple.extra_bit()
    }

    pub fn state(&self) -> LinkState {
        if self.dead() {
            LinkState::Dead
        } else {
            LinkState::Live
        }
    }

    /// All the fields.
    pub fn ids(&self) -> Vec<u64> {
        self.tuple.decode(self.id).0
    }

    pub fn subgraphs(&self) -> Vec<u64> {
        let mut ids = self.ids();
        ids.drain(..ids.len().min(Linkage::Subgraph.field()));
        ids
    }

    /// Whether every field but `when` refers to an earlier entry.
    pub fn is_valid(&self) -> bool {
        refers_back(self.id, &self.ids())
    }

    pub fn fields(&self) -> LinkFields {
        LinkFields {
            subject: self.subject(),
            predicate: self.predicate(),
            object: self.object(),
            author: self.author(),
            when: self.when(),
            state: self.state(),
            subgraphs: self.subgraphs(),
        }
    }
}

/// The fields of a link, for building one with [`Record::link`].
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct LinkFields {
    pub subject: u64,
    pub predicate: u64,
    pub object: u64,
    pub author: u64,
    pub when: LinkTime,
    pub state: LinkState,
    pub subgraphs: Vec<u64>,
}

impl LinkFields {
    /// The tuple fields, in [`Linkage`] order.
    pub fn ids(&self) -> Vec<u64> {
        let mut ids = vec![
            self.subject,
            self.predicate,
            self.object,
            self.author,
            self.when,
        ];
        ids.extend_from_slice(&self.subgraphs);
        ids
    }
}

/// Records ordered by id.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct RecordSet<'a> {
    records: BTreeMap<u64, Record<'a>>,
}

impl<'a> RecordSet<'a> {
    pub fn new() -> Self {
        RecordSet::default()
    }

    /// Adds `record`, returning any record it replaces with the same id.
    pub fn insert(&mut self, record: Record<'a>) -> Option<Record<'a>> {
        self.records.insert(record.id, record)
    }

    pub fn get(&self, id: u64) -> Option<&Record<'a>> {
        self.records.get(&id)
    }

    /// The data of the value (or node) at `id`.
    pub fn data(&self, id: u64) -> Option<&[u8]> {
        self.get(id)?.data()
    }

    /// The id of the first record.
    pub fn start_id(&self) -> Option<u64> {
        self.records.keys().next().copied()
    }

    /// The id following the last record.
    pub fn next_id(&self) -> Option<u64> {
        self.records.values().next_back().map(Record::next_id)
    }

    /// The records with ids in `range`; `range(id..)` starts at the first
    /// record at or after `id`.
    pub fn range(
        &self,
        range: impl RangeBounds<u64>,
    ) -> impl DoubleEndedIterator<Item = &Record<'a>> {
        self.records.range(range).map(|(_, r)| r)
    }

    pub fn iter(&self) -> impl DoubleEndedIterator<Item = &Record<'a>> {
        self.records.values()
    }

    pub fn len(&self) -> usize {
        self.records.len()
    }

    pub fn is_empty(&self) -> bool {
        self.records.is_empty()
    }
}

impl<'a> FromIterator<Record<'a>> for RecordSet<'a> {
    fn from_iter<I: IntoIterator<Item = Record<'a>>>(iter: I) -> Self {
        RecordSet {
            records: iter.into_iter().map(|r| (r.id, r)).collect(),
        }
    }
}

/// Moves a sequence of records to consecutive ids starting at a given id,
/// rewriting the links among them to match.
#[derive(Debug, Clone, Default)]
pub struct Renumberer {
    next_id: u64,
    map: HashMap<u64, u64>,
}

impl Renumberer {
    pub fn new(next_id: u64) -> Self {
        Renumberer {
            next_id,
            map: HashMap::new(),
        }
    }

    /// The id the next record will be given.
    pub fn next_id(&self) -> u64 {
        self.next_id
    }

    /// The new id of `id`, which is unchanged if it has not been moved.
    pub fn map(&self, id: u64) -> u64 {
        self.map.get(&id).copied().unwrap_or(id)
    }

    /// Gives `record` the next id, rewriting a link's fields (but not `when`)
    /// to the new ids of the records they refer to.  Records must be passed
    /// in order, after any they refer to.
    pub fn renumber<'a>(&mut self, record: &Record<'a>) -> Result<Record<'a>> {
        if record.id != self.next_id {
            self.map.insert(record.id, self.next_id);
        }
        let result = match record.as_link() {
            _ if self.map.is_empty() => record.clone(),
            Some(link) => {
                let mut ids = link.ids();
                for (i, id) in ids.iter_mut().enumerate() {
                    if i != WHEN_FIELD {
                        *id = self.map(*id);
                    }
                }
                Record::tuple(self.next_id, link.dead(), &ids)?
            }
            None => Record {
                id: self.next_id,
                bytes: record.bytes.clone(),
            },
        };
        self.next_id += result.encoded_len();
        Ok(result)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A small deterministic generator so tests need no dependencies.
    struct XorShift(u64);

    impl XorShift {
        fn next(&mut self) -> u64 {
            self.0 ^= self.0 << 13;
            self.0 ^= self.0 >> 7;
            self.0 ^= self.0 << 17;
            self.0
        }

        /// Ids skewed towards small values, all encodable.
        fn skewed(&mut self, n: usize) -> Vec<u64> {
            (0..n)
                .map(|_| {
                    let shift = 1 + self.next() % 63;
                    self.next() >> shift
                })
                .collect()
        }
    }

    const LINKAGES_PER_TUPLE: usize = 10;

    fn offset(i: usize, linkage: Linkage) -> usize {
        i * LINKAGES_PER_TUPLE + linkage.field()
    }

    #[test]
    fn simple() {
        let ids = XorShift(0x2545_f491_4f6c_dd1d).skewed(4 * LINKAGES_PER_TUPLE);
        let mut log = RecordLog::new(1);
        let t: Vec<u64> = ids
            .chunks(LINKAGES_PER_TUPLE)
            .map(|c| log.push_tuple(false, c).unwrap())
            .collect();

        let v1 = Record::from_log(&log, t[0]).unwrap();
        let v2 = Record::from_log(&log, t[1]).unwrap();
        let mut v3 = Record::from_log(&log, t[2]).unwrap().into_owned();
        let v4 = v3.clone();
        v3 = Record::from_log(&log, t[3]).unwrap().into_owned();

        assert_eq!(
            v1.as_link().unwrap().subject(),
            ids[offset(0, Linkage::Subject)]
        );
        assert_eq!(
            v2.as_link().unwrap().object(),
            ids[offset(1, Linkage::Object)]
        );
        assert_eq!(
            v3.as_link().unwrap().predicate(),
            ids[offset(3, Linkage::Predicate)]
        );
        assert_eq!(
            v4.as_link().unwrap().subgraphs()[0],
            ids[offset(2, Linkage::Subgraph)]
        );

        // Owning shared bytes does not copy them; owning borrowed bytes does.
        let r3 = v3.clone().into_owned();
        assert_eq!(r3.bytes().as_ptr(), v3.bytes().as_ptr());
        let r2 = v2.clone().into_owned();
        assert_ne!(r2.bytes().as_ptr(), v2.bytes().as_ptr());
        assert_eq!(r2, v2);
        drop(log);
        assert_eq!(r2.as_link().unwrap().ids(), ids[10..20]);
    }

    #[test]
    fn kinds() {
        let node = Record::node(7);
        assert!(node.is_node() && !node.is_value() && !node.is_link());
        assert_eq!(node.bytes(), [0]);
        assert_eq!(node.next_id(), 8);
        assert_eq!(node.data(), Some(&[][..]));
        assert_eq!(Record::value(7, b""), node);
        assert_eq!(Record::from_bytes(7, &[0, 9, 9]).unwrap(), node);

        let value = Record::value(8, b"hello");
        assert!(value.is_value() && !value.is_node() && !value.is_link());
        assert_eq!(value.data(), Some(&b"hello"[..]));
        assert!(value.as_link().is_none());
        assert_eq!(value.value_uuid(), Some(Uuid::of(b"hello")));
        assert_ne!(Uuid::of(b"hello"), Uuid::of(b"hellp"));

        let link = Record::tuple(20, true, &[1, 2, 3]).unwrap();
        assert!(link.is_link() && link.data().is_none());
        let l = link.as_link().unwrap();
        assert_eq!((l.len(), l.dead(), l.state()), (3, true, LinkState::Dead));
        assert_eq!((l.author(), l.when(), l.field(9)), (0, 0, 0));
        assert!(l.subgraphs().is_empty());

        assert_eq!(
            Record::from_bytes(1, &value.bytes()[..3]),
            Err(Error::Truncated)
        );
    }

    #[test]
    fn link_fields() {
        let fields = LinkFields {
            subject: 1,
            predicate: 2,
            object: 3,
            author: 4,
            when: now(),
            state: LinkState::Live,
            subgraphs: vec![5, 6],
        };
        let link = Record::link(100, &fields).unwrap();
        let l = link.as_link().unwrap();
        assert_eq!(l.fields(), fields);
        assert!(l.is_valid());
        assert_eq!(l.get(Linkage::Subgraph), 5);
        assert_eq!(l.len(), 7);

        // `when` may be after the link; nothing else may.
        assert!(
            Record::link(
                3,
                &LinkFields {
                    when: 1 << 40,
                    ..Default::default()
                }
            )
            .is_ok()
        );
        let forward = LinkFields {
            object: 100,
            ..fields.clone()
        };
        assert_eq!(Record::link(100, &forward), Err(Error::ForwardReference));
        assert!(
            !Record::tuple(100, false, &forward.ids())
                .unwrap()
                .as_link()
                .unwrap()
                .is_valid()
        );
    }

    #[test]
    fn changes() {
        let fields = LinkFields {
            subject: 1,
            predicate: 2,
            object: 3,
            author: 9,
            when: 50,
            ..Default::default()
        };
        let link = Record::link(10, &fields).unwrap();

        let moved = link.with_id(1000).unwrap();
        assert_eq!(moved.id(), 1000);
        assert_eq!(moved.as_link().unwrap().fields(), fields);
        assert_ne!(moved.bytes(), link.bytes());

        let changed = link.with_linkage(Linkage::Object, 9).unwrap();
        assert_eq!(changed.as_link().unwrap().object(), 9);
        let with_sg = link.with_linkage(Linkage::Subgraph, 8).unwrap();
        assert_eq!(with_sg.as_link().unwrap().subgraphs(), [8]);

        assert_eq!(link.with_when(77).unwrap().as_link().unwrap().when(), 77);
        let dead = link.with_state(LinkState::Dead).unwrap();
        assert_eq!(dead.as_link().unwrap().state(), LinkState::Dead);
        assert!(same_content(&dead, &link));

        let value = Record::value(5, b"v");
        assert_eq!(value.with_when(77).unwrap(), value);
        assert_eq!(value.with_id(6).unwrap().data(), Some(&b"v"[..]));
        assert_eq!(
            value.with_linkage(Linkage::Subject, 1),
            Err(Error::KindMismatch)
        );
        assert_eq!(value.with_state(LinkState::Dead), Err(Error::KindMismatch));
    }

    #[test]
    fn content() {
        assert!(same_content(&Record::node(1), &Record::node(2)));
        assert!(same_content(
            &Record::value(1, b"a"),
            &Record::value(9, b"a")
        ));
        assert!(!same_content(
            &Record::value(1, b"a"),
            &Record::value(1, b"b")
        ));
        assert!(!same_content(&Record::node(1), &Record::value(1, b"a")));
        let a = Record::tuple(1000, false, &[990, 995]).unwrap();
        let b = Record::tuple(5000, false, &[990, 995]).unwrap();
        assert_ne!(a.bytes(), b.bytes());
        assert!(same_content(&a, &b));
        assert!(!same_content(
            &a,
            &Record::tuple(1000, false, &[990, 995, 0]).unwrap()
        ));
        assert!(!same_content(&a, &Record::node(1000)));
    }

    #[test]
    fn linkages() {
        assert_eq!(Linkage::Subject.inverse(), Some(Linkage::Object));
        assert_eq!(Linkage::Object.inverse(), Some(Linkage::Subject));
        assert_eq!(Linkage::Author.inverse(), None);
        assert_eq!(Linkage::of_field(4), Linkage::When);
        assert_eq!(Linkage::of_field(12).name(), "subgraph");
        for l in Linkage::ALL {
            assert_eq!(Linkage::of_field(l.field()), l);
        }
        assert!(!Linkage::ALL.contains(&Linkage::When));
    }

    #[test]
    fn record_set() {
        let mut log = RecordLog::new(1);
        let a = log.push_value(b"a");
        let n = log.push_node();
        let t = log.push_tuple(false, &[a, n]).unwrap();
        let set: RecordSet = [t, a, n]
            .iter()
            .map(|&id| Record::from_log(&log, id).unwrap())
            .collect();
        assert_eq!(set.len(), 3);
        assert_eq!(set.start_id(), Some(a));
        assert_eq!(set.next_id(), Some(log.end_id()));
        assert_eq!(set.data(a), Some(&b"a"[..]));
        assert_eq!(set.data(t), None);
        assert_eq!(set.range(n..).map(Record::id).collect::<Vec<_>>(), [n, t]);
        assert_eq!(set.range(n + 1..).next().unwrap().id(), t);
        assert!(RecordSet::new().next_id().is_none());
    }

    #[test]
    fn renumber() {
        let mut log = RecordLog::new(100);
        let a = log.push_value(b"alice");
        let k = log.push_value(b"knows");
        let b = log.push_node();
        let e = log.push_tuple(true, &[a, k, b, 0, 12345, 7]).unwrap();
        let records: Vec<_> = [a, k, b, e]
            .iter()
            .map(|&id| Record::from_log(&log, id).unwrap())
            .collect();

        // Renumbering in place changes nothing.
        let mut same = Renumberer::new(100);
        for r in &records {
            assert_eq!(&same.renumber(r).unwrap(), r);
        }
        assert_eq!(same.next_id(), log.end_id());

        let mut r = Renumberer::new(1);
        let out: Vec<_> = records.iter().map(|x| r.renumber(x).unwrap()).collect();
        assert_eq!(out[0].id(), 1);
        assert_eq!(out[1].id(), out[0].next_id());
        assert_eq!(out[1].data(), Some(&b"knows"[..]));
        assert!(out[2].is_node());
        let link = out[3].as_link().unwrap();
        assert_eq!(
            link.ids(),
            [out[0].id(), out[1].id(), out[2].id(), 0, 12345, 7]
        );
        assert!(link.dead());
        assert_eq!(r.next_id(), out[3].next_id());
        assert_eq!(r.map(e), out[3].id());
        assert_eq!(r.map(7), 7);

        // The renumbered records form a valid log.
        let mut copy = RecordLog::new(1);
        for x in &out {
            match x.as_link() {
                Some(l) => copy.push_tuple(l.dead(), &l.ids()).unwrap(),
                None => copy.push_value(x.data().unwrap()),
            };
        }
        for x in &out {
            assert_eq!(&Record::from_log(&copy, x.id()).unwrap(), x);
        }
    }
}
