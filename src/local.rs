//! A local graph database: a [`LocalRecordLog`] of entries plus a number of
//! [`LocalTable`] indexes, committed together.
//!
//! Ported from the C++ `LocalInterface` and its `CreateLocal`/`OpenLocal`
//! functions.
//!
//! # Files
//!
//! A database at `prefix` is the log at `{prefix}record_log.` and the index
//! tables at `{prefix}index0.`, `{prefix}index1.`, ...  With no prefix the
//! database is in memory.
//!
//! # Commits
//!
//! [`commit`](Local::commit) runs the log's two-phase commit with the indexes
//! flushed in between, so a completed commit's index entries are on disk.  If
//! a crash interrupts a commit, opening the database abandons it, since its
//! index entries may not be.
//!
//! An index may refer to entries that were never committed: its pages can
//! reach disk at any time.  The log never reuses an id, so such a reference
//! names a filler node rather than a different entry (see
//! [`local_record_log`](crate::local_record_log#ids-are-never-reused)).

use std::fmt;
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};

use crate::dynamic_table;
use crate::local_record_log::{self, LocalRecordLog};
use crate::local_table::{self, LocalTable};
use crate::mmap::suffixed;

/// Rough sizes for preallocating a database from its expected tuple count.
const LOG_BYTES_PER_TUPLE: u64 = 30;
const INDEX_BYTES_PER_TUPLE: u64 = 10;

const SNAPSHOT_MAGIC: u64 = u64::from_le_bytes(*b"gdbl-snp");
const SNAPSHOT_VERSION: u64 = 1;
/// The largest snapshot frame accepted, well above any component's chunks.
const MAX_FRAME: u64 = 1 << 24;

/// Errors from a local database.
#[derive(Debug)]
pub enum Error {
    Io(io::Error),
    Log(local_record_log::Error),
    Index(dynamic_table::Error),
    /// A snapshot is not a valid local database snapshot.
    InvalidFormat(&'static str),
}

pub type Result<T> = std::result::Result<T, Error>;

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Io(e) => write!(f, "I/O error: {e}"),
            Error::Log(e) => write!(f, "record log: {e}"),
            Error::Index(e) => write!(f, "index: {e}"),
            Error::InvalidFormat(what) => write!(f, "invalid snapshot: {what}"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Error::Io(e) => Some(e),
            Error::Log(e) => Some(e),
            Error::Index(e) => Some(e),
            Error::InvalidFormat(_) => None,
        }
    }
}

impl From<io::Error> for Error {
    fn from(e: io::Error) -> Self {
        Error::Io(e)
    }
}

impl From<local_record_log::Error> for Error {
    fn from(e: local_record_log::Error) -> Self {
        Error::Log(e)
    }
}

impl From<dynamic_table::Error> for Error {
    fn from(e: dynamic_table::Error) -> Self {
        Error::Index(e)
    }
}

/// Options for opening a database.
#[derive(Debug, Clone, Default)]
pub struct Options {
    pub log: local_record_log::Options,
    /// The options for each index.
    pub index: dynamic_table::Options,
    /// The number of index tables a database has at least: an existing
    /// database with fewer is given more.
    pub tables: usize,
}

impl Options {
    /// Options preallocating room for about `tuples` tuples.
    pub fn for_tuples(tuples: u64) -> Self {
        let mut options = Options::default();
        options.log.create_size = tuples.saturating_mul(LOG_BYTES_PER_TUPLE);
        options.index.create_size = tuples.saturating_mul(INDEX_BYTES_PER_TUPLE);
        options
    }
}

fn log_prefix(prefix: &Path) -> PathBuf {
    suffixed(prefix, "record_log.")
}

fn index_prefix(prefix: &Path, i: usize) -> PathBuf {
    suffixed(prefix, &format!("index{i}."))
}

/// Whether a database exists at `prefix`.
pub fn exists(prefix: &Path) -> bool {
    local_record_log::exists(&log_prefix(prefix))
}

/// Deletes the database at `prefix`.
pub fn remove(prefix: &Path) -> io::Result<()> {
    local_record_log::remove(&log_prefix(prefix))?;
    for i in 0.. {
        let p = index_prefix(prefix, i);
        if !local_table::exists(&p) {
            break;
        }
        local_table::remove(&p)?;
    }
    Ok(())
}

/// A local graph database; see the [module](self) docs.
pub struct Local {
    prefix: Option<PathBuf>,
    options: Options,
    log: LocalRecordLog,
    indexes: Vec<LocalTable>,
}

impl Local {
    /// Opens or creates the database whose files start with `prefix`, or an
    /// in-memory database if `prefix` is `None`.
    pub fn open(prefix: Option<&Path>, options: Options) -> Result<Self> {
        let mut log = LocalRecordLog::open(prefix.map(log_prefix).as_deref(), options.log.clone())?;
        if log.has_pending_commit() {
            log.abandon_commit()?;
        }
        let mut local = Local {
            prefix: prefix.map(Path::to_path_buf),
            options,
            log,
            indexes: Vec::new(),
        };
        while local.indexes.len() < local.options.tables
            || local
                .prefix
                .as_ref()
                .is_some_and(|p| local_table::exists(&index_prefix(p, local.indexes.len())))
        {
            local.new_table()?;
        }
        Ok(local)
    }

    /// Adds an index table, returning its number.
    pub fn new_table(&mut self) -> Result<usize> {
        let i = self.indexes.len();
        let prefix = self.prefix.as_ref().map(|p| index_prefix(p, i));
        self.indexes.push(LocalTable::open(
            prefix.as_deref(),
            self.options.index.clone(),
        )?);
        Ok(i)
    }

    pub fn is_transient(&self) -> bool {
        self.prefix.is_none()
    }

    /// The path prefix of the database's files, if it is not in memory.
    pub fn prefix(&self) -> Option<&Path> {
        self.prefix.as_deref()
    }

    pub fn log(&self) -> &LocalRecordLog {
        &self.log
    }

    /// The log, for appending.  Commit with [`commit`](Self::commit), not the
    /// log's own commit, so that the indexes are flushed too.
    pub fn log_mut(&mut self) -> &mut LocalRecordLog {
        &mut self.log
    }

    pub fn table_count(&self) -> usize {
        self.indexes.len()
    }

    /// Index table `i`.  Panics if there is no such table.
    pub fn table(&self, i: usize) -> &LocalTable {
        &self.indexes[i]
    }

    /// Index table `i`, for inserting.  Panics if there is no such table.
    pub fn table_mut(&mut self, i: usize) -> &mut LocalTable {
        &mut self.indexes[i]
    }

    /// Commits the log's appended entries, with the indexes flushed so that
    /// they cover them.
    pub fn commit(&mut self) -> Result<()> {
        self.log.start_commit()?;
        for index in &self.indexes {
            index.sync()?;
        }
        self.log.complete_commit()?;
        Ok(())
    }

    /// Does periodic maintenance on the indexes.
    pub fn tick(&mut self) -> Result<()> {
        for index in &mut self.indexes {
            index.tick()?;
        }
        Ok(())
    }

    /// Writes a snapshot of the database, including entries not yet
    /// committed, to `out`.
    pub fn snapshot(&mut self, out: &mut impl Write) -> Result<()> {
        for word in [SNAPSHOT_MAGIC, SNAPSHOT_VERSION, self.indexes.len() as u64] {
            out.write_all(&word.to_le_bytes())?;
        }
        let mut sink = |chunk: &[u8]| {
            out.write_all(&(chunk.len() as u64).to_le_bytes())?;
            out.write_all(chunk)
        };
        self.log.snapshot(&mut sink)?;
        for index in &mut self.indexes {
            index.snapshot(&mut sink)?;
        }
        Ok(())
    }

    /// Creates a database at `prefix`, or in memory if `prefix` is `None`,
    /// from a [`snapshot`](Self::snapshot) read from `input`.  Fails if a
    /// database exists at `prefix`.
    pub fn restore(prefix: Option<&Path>, options: Options, input: &mut impl Read) -> Result<Self> {
        if prefix.is_some_and(exists) {
            return Err(io::Error::from(io::ErrorKind::AlreadyExists).into());
        }
        let result = (|| {
            let mut word = || -> io::Result<u64> {
                let mut b = [0u8; 8];
                input.read_exact(&mut b)?;
                Ok(u64::from_le_bytes(b))
            };
            if word()? != SNAPSHOT_MAGIC || word()? != SNAPSHOT_VERSION {
                return Err(Error::InvalidFormat("bad magic or version"));
            }
            let count = word()?;
            let mut source = || -> io::Result<Vec<u8>> {
                let mut b = [0u8; 8];
                input.read_exact(&mut b)?;
                let len = u64::from_le_bytes(b);
                if len > MAX_FRAME {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        "snapshot frame too long",
                    ));
                }
                let mut chunk = vec![0; len as usize];
                input.read_exact(&mut chunk)?;
                Ok(chunk)
            };
            let log = LocalRecordLog::restore(
                prefix.map(log_prefix).as_deref(),
                options.log.clone(),
                &mut source,
            )?;
            let mut indexes = Vec::new();
            for i in 0..count as usize {
                let p = prefix.map(|p| index_prefix(p, i));
                indexes.push(LocalTable::restore(
                    p.as_deref(),
                    options.index.clone(),
                    &mut source,
                )?);
            }
            if input.read(&mut [0])? != 0 {
                return Err(Error::InvalidFormat("trailing data"));
            }
            let mut local = Local {
                prefix: prefix.map(Path::to_path_buf),
                options,
                log,
                indexes,
            };
            while local.indexes.len() < local.options.tables {
                local.new_table()?;
            }
            Ok(local)
        })();
        if result.is_err()
            && let Some(p) = prefix
        {
            let _ = remove(p);
        }
        result
    }
}

#[cfg(test)]
mod tests;
