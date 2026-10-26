//! A graph database: nodes, interned values and links, stored in a [`Local`]
//! database and indexed by [`tuple_index`].
//!
//! Replaces the C++ `TupleStore`, keeping its graph database core and leaving
//! out its schema-driven write pipeline (primary keys, compounds,
//! `replaced_by`, cardinality, subgraph unions), base/extension layering, and
//! import/export formats.
//!
//! # Entries
//!
//! A *node* is an entity with no content.  A *value* is a blob of data;
//! values are interned, so each distinct data is one value.  A *link* is a
//! tuple of subject, predicate, object, author, creation time (`when`) and
//! any number of subgraphs (see [`LinkFields`]), each field the id of an
//! earlier entry (or 0) except `when`.
//!
//! # Link state
//!
//! A link's subject, predicate and object (its SPO) identify it, and the
//! latest link with an SPO says whether it holds: live, or dead.
//! [`delete_link`](Graph::delete_link) appends a dead link with the SPO, and
//! linking it again appends a live one.  Appending a link whose SPO's latest
//! link already has the same state returns that link instead.
//!
//! Queries that find links by index return them all, live and dead;
//! [`live`](Graph::live) keeps those that are the latest, live link of their
//! SPO.  It needs every link of each SPO in the set, which every set found by
//! subject, predicate or object (or a pair of them) has.
//!
//! # Time
//!
//! Ids increase as entries are appended, so an id is also a time: queries
//! taking `as_of` see the graph as it was when that id was appended.
//!
//! # Commits
//!
//! Appends are visible at once and become durable on
//! [`commit`](Graph::commit); [`batch`](Graph::batch) appends all or nothing.
//! After a crash or [`rollback`](Graph::rollback) the indexes may still name
//! entries that were lost, which the log has replaced with filler nodes, so
//! the graph checks what the ids it finds name.

use std::collections::{BTreeMap, HashMap};
use std::fmt;
use std::io::{Read, Write};
use std::path::Path;

pub use crate::local::Options;
use crate::local::{self, Local};
use crate::local_record_log::{self, Records};
use crate::local_table::IdSet;
use crate::record::{Link, LinkFields, LinkState, Linkage, Record, now};
use crate::record_log::Entry;
use crate::tuple_index::{self, Intersection, NUM_BUILTIN_INDEXES};

/// Errors from a graph.
#[derive(Debug)]
pub enum Error {
    Local(local::Error),
    Index(tuple_index::Error),
    /// Encoding or reading an entry failed, or a link refers forward.
    Record(crate::Error),
    InvalidArgument(&'static str),
}

pub type Result<T> = std::result::Result<T, Error>;

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Local(e) => write!(f, "{e}"),
            Error::Index(e) => write!(f, "{e}"),
            Error::Record(e) => write!(f, "record: {e}"),
            Error::InvalidArgument(what) => write!(f, "invalid argument: {what}"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Error::Local(e) => Some(e),
            Error::Index(e) => Some(e),
            Error::Record(e) => Some(e),
            Error::InvalidArgument(_) => None,
        }
    }
}

impl From<local::Error> for Error {
    fn from(e: local::Error) -> Self {
        Error::Local(e)
    }
}

impl From<local_record_log::Error> for Error {
    fn from(e: local_record_log::Error) -> Self {
        Error::Local(local::Error::Log(e))
    }
}

impl From<tuple_index::Error> for Error {
    fn from(e: tuple_index::Error) -> Self {
        Error::Index(e)
    }
}

impl From<crate::Error> for Error {
    fn from(e: crate::Error) -> Self {
        Error::Record(e)
    }
}

/// A graph database; see the [module](self) docs.
pub struct Graph {
    local: Local,
}

impl Graph {
    /// Opens or creates the graph whose files start with `prefix`, or an
    /// in-memory graph if `prefix` is `None`.
    pub fn open(prefix: Option<&Path>, mut options: Options) -> Result<Self> {
        options.tables = options.tables.max(NUM_BUILTIN_INDEXES);
        Ok(Graph {
            local: Local::open(prefix, options)?,
        })
    }

    /// Creates a graph at `prefix`, or in memory, from a
    /// [`snapshot`](Self::snapshot) read from `input`.
    pub fn restore(
        prefix: Option<&Path>,
        mut options: Options,
        input: &mut impl Read,
    ) -> Result<Self> {
        options.tables = options.tables.max(NUM_BUILTIN_INDEXES);
        Ok(Graph {
            local: Local::restore(prefix, options, input)?,
        })
    }

    /// Writes a snapshot of the graph, including entries not yet committed,
    /// to `out`.
    pub fn snapshot(&mut self, out: &mut impl Write) -> Result<()> {
        Ok(self.local.snapshot(out)?)
    }

    /// Makes the appended entries durable.
    pub fn commit(&mut self) -> Result<()> {
        Ok(self.local.commit()?)
    }

    /// Discards the entries appended since the last commit.  Their ids are not
    /// reused.
    pub fn rollback(&mut self) -> Result<()> {
        Ok(self.local.log_mut().abandon_commit()?)
    }

    /// Runs `f`, then commits if it succeeds or rolls back if it fails.
    /// Rolling back discards everything appended since the last commit, not
    /// just what `f` appended.
    pub fn batch<R>(&mut self, f: impl FnOnce(&mut Graph) -> Result<R>) -> Result<R> {
        match f(self) {
            Ok(r) => {
                self.commit()?;
                Ok(r)
            }
            Err(e) => {
                self.rollback()?;
                Err(e)
            }
        }
    }

    /// Does periodic maintenance on the indexes.
    pub fn tick(&mut self) -> Result<()> {
        Ok(self.local.tick()?)
    }

    pub fn is_transient(&self) -> bool {
        self.local.is_transient()
    }

    /// The underlying database.
    pub fn local(&self) -> &Local {
        &self.local
    }

    /// The id of the first entry.
    pub fn start_id(&self) -> u64 {
        self.local.log().start_id()
    }

    /// The id the next entry appended will have.
    pub fn next_id(&self) -> u64 {
        self.local.log().next_id()
    }

    /// Whether `id` is within the graph.  It may not start an entry.
    pub fn contains(&self, id: u64) -> bool {
        (self.start_id()..self.next_id()).contains(&id)
    }

    /// The entry at `id`.
    pub fn get(&self, id: u64) -> Result<Record<'_>> {
        Ok(self.local.log().get(id)?)
    }

    /// The entries from `id` (which must start an entry) on.
    pub fn records(&self, id: u64) -> Records<'_> {
        self.local.log().records(id)
    }

    /// The data of the value at `id`.
    pub fn data(&self, id: u64) -> Option<&[u8]> {
        match self.local.log().entry(id) {
            Ok(Entry::Value(v)) if !v.is_empty() => Some(v.as_bytes()),
            _ => None,
        }
    }

    /// Calls `f` with the link at `id`, if it is one.
    fn with_link<T>(&self, id: u64, f: impl FnOnce(&Link<'_>) -> T) -> Option<T> {
        let record = self.local.log().get(id).ok()?;
        record.as_link().map(|l| f(&l))
    }

    fn append(&mut self, record: &Record<'_>) -> Result<u64> {
        self.local.log_mut().append(record)?;
        tuple_index::index_records(&mut self.local, std::slice::from_ref(record))?;
        Ok(record.id())
    }

    /// Appends a node.
    pub fn node(&mut self) -> Result<u64> {
        let id = self.next_id();
        self.append(&Record::node(id))
    }

    /// The value holding `data`, appending it if there is none.  Empty data
    /// is a node, which is not interned.
    pub fn value(&mut self, data: &[u8]) -> Result<u64> {
        if data.is_empty() {
            return self.node();
        }
        if let Some(id) = self.find_value(data) {
            return Ok(id);
        }
        let id = self.next_id();
        self.append(&Record::value(id, data))
    }

    /// The value holding `data`.
    pub fn find_value(&self, data: &[u8]) -> Option<u64> {
        tuple_index::find_value(&self.local, data).filter(|&id| self.data(id) == Some(data))
    }

    /// Links `subject` to `object` by `predicate`, now, returning the live
    /// link.
    pub fn link(&mut self, subject: u64, predicate: u64, object: u64) -> Result<u64> {
        self.add_link(&LinkFields {
            subject,
            predicate,
            object,
            when: now(),
            ..LinkFields::default()
        })
    }

    /// Appends a link with `fields`, unless the latest link with its SPO
    /// already has its state, which is returned instead.  Every field but
    /// `when` must be 0 or the id of an earlier entry.
    pub fn add_link(&mut self, fields: &LinkFields) -> Result<u64> {
        if let Some((id, state)) = self.latest_link(fields.subject, fields.predicate, fields.object)
            && state == fields.state
        {
            return Ok(id);
        }
        let record = Record::link(self.next_id(), fields)?;
        self.append(&record)
    }

    /// Deletes the link with this SPO, now, returning the dead link appended,
    /// or `None` if there is no live link to delete.
    pub fn delete_link(
        &mut self,
        subject: u64,
        predicate: u64,
        object: u64,
    ) -> Result<Option<u64>> {
        match self.latest_link(subject, predicate, object) {
            Some((_, LinkState::Live)) => self
                .add_link(&LinkFields {
                    subject,
                    predicate,
                    object,
                    when: now(),
                    state: LinkState::Dead,
                    ..LinkFields::default()
                })
                .map(Some),
            _ => Ok(None),
        }
    }

    /// Every link, live or dead, with this SPO.
    pub fn find_links(&self, subject: u64, predicate: u64, object: u64) -> IdSet {
        tuple_index::find_links_by_spo(&self.local, subject, predicate, object)
            .into_iter()
            .filter(|&id| {
                self.with_link(id, |l| (l.subject(), l.predicate(), l.object()))
                    == Some((subject, predicate, object))
            })
            .collect()
    }

    fn latest_link(&self, subject: u64, predicate: u64, object: u64) -> Option<(u64, LinkState)> {
        let id = *self.find_links(subject, predicate, object).last()?;
        Some((id, self.with_link(id, |l| l.state())?))
    }

    /// The live link with this SPO.
    pub fn find_link(&self, subject: u64, predicate: u64, object: u64) -> Option<u64> {
        match self.latest_link(subject, predicate, object) {
            Some((id, LinkState::Live)) => Some(id),
            _ => None,
        }
    }

    /// Every link, live or dead, with any of `ids` at `linkage` (any of its
    /// subgraphs, for `Subgraph`).
    pub fn links_at(&self, linkage: Linkage, ids: impl IntoIterator<Item = u64>) -> IdSet {
        if linkage == Linkage::When {
            return IdSet::new();
        }
        let mut links = IdSet::new();
        for id in ids {
            links.extend(tuple_index::find_links(&self.local, linkage, id));
        }
        links.retain(|&id| self.with_link(id, |_| ()).is_some());
        links
    }

    /// The links of `links` that are the latest, live link of their SPO
    /// among those appended by `as_of` (or at all, if `None`).  `links` must
    /// hold every such link of each SPO it holds.
    pub fn live(&self, links: &IdSet, as_of: Option<u64>) -> IdSet {
        let mut latest = HashMap::new();
        for &id in links.range(..=as_of.unwrap_or(u64::MAX)) {
            if let Some((spo, dead)) =
                self.with_link(id, |l| ((l.subject(), l.predicate(), l.object()), l.dead()))
            {
                latest.insert(spo, (id, dead));
            }
        }
        latest
            .into_values()
            .filter(|&(_, dead)| !dead)
            .map(|(id, _)| id)
            .collect()
    }

    /// The non-zero ids at `linkage` (every subgraph, for `Subgraph`) of
    /// `links`.
    pub fn project(&self, links: &IdSet, linkage: Linkage) -> IdSet {
        let mut out = IdSet::new();
        for &id in links {
            self.with_link(id, |l| match linkage {
                Linkage::Subgraph => out.extend(l.subgraphs()),
                _ => {
                    out.insert(l.get(linkage));
                }
            });
        }
        out.remove(&0);
        out
    }

    /// The id at `linkage` (the first subgraph, for `Subgraph`) of each of
    /// `links`, if non-zero.
    pub fn project_map(&self, links: &IdSet, linkage: Linkage) -> BTreeMap<u64, u64> {
        links
            .iter()
            .filter_map(|&id| Some((id, self.with_link(id, |l| l.get(linkage))?)))
            .filter(|&(_, v)| v != 0)
            .collect()
    }

    /// The live links (as of `as_of`) with `predicate` and any of `ids` at
    /// `linkage`, which must be `Subject` or `Object`, and the entities at the
    /// other of the two.
    pub fn query_property(
        &self,
        ids: impl IntoIterator<Item = u64>,
        linkage: Linkage,
        predicate: u64,
        as_of: Option<u64>,
    ) -> Result<(IdSet, IdSet)> {
        let (intersection, other) = match linkage {
            Linkage::Subject => (Intersection::SubjectPredicate, Linkage::Object),
            Linkage::Object => (Intersection::PredicateObject, Linkage::Subject),
            _ => return Err(Error::InvalidArgument("linkage must be subject or object")),
        };
        let mut links = IdSet::new();
        for id in ids {
            let (x, y) = match intersection {
                Intersection::SubjectPredicate => (id, predicate),
                Intersection::PredicateObject => (predicate, id),
            };
            links.extend(tuple_index::find_links_by_intersection(
                &self.local,
                intersection,
                x,
                y,
            ));
        }
        let links = self.live(&links, as_of);
        let entities = self.project(&links, other);
        Ok((links, entities))
    }

    /// The objects of the live links with `predicate` from any of `subjects`.
    pub fn follow_forward(&self, predicate: u64, subjects: impl IntoIterator<Item = u64>) -> IdSet {
        self.query_property(subjects, Linkage::Subject, predicate, None)
            .expect("subject")
            .1
    }

    /// The subjects of the live links with `predicate` to any of `objects`.
    pub fn follow_backward(&self, predicate: u64, objects: impl IntoIterator<Item = u64>) -> IdSet {
        self.query_property(objects, Linkage::Object, predicate, None)
            .expect("object")
            .1
    }

    /// The ids at `output` of the live links (as of `as_of`) with `id` at
    /// `input`.
    pub fn entities_with(
        &self,
        input: Linkage,
        id: u64,
        output: Linkage,
        as_of: Option<u64>,
    ) -> IdSet {
        let links = self.live(&self.links_at(input, [id]), as_of);
        self.project(&links, output)
    }

    /// The id at `output` of the latest live link (as of `as_of`) with `id`
    /// at `input`.
    pub fn entity_with(
        &self,
        input: Linkage,
        id: u64,
        output: Linkage,
        as_of: Option<u64>,
    ) -> Option<u64> {
        let links = self.live(&self.links_at(input, [id]), as_of);
        let latest = *links.last()?;
        self.with_link(latest, |l| l.get(output))
            .filter(|&v| v != 0)
    }
}

#[cfg(test)]
mod tests;
