//! A record log stored in local memory mapped files.
//!
//! Ported from the C++ `LocalRecordLog`, fixing its bugs along the way.
//!
//! # Layout
//!
//! A log at `prefix` is a metadata file, `{prefix}metadata`, and up to
//! [`MAX_FILES`] data files, `{prefix}0`, `{prefix}1`, ...  Each data file is
//! a [`RecordLog`](crate::RecordLog): a [`PersistentHeader`] giving the id of
//! its first entry, then entries.  File `i` holds the ids from its start id up
//! to the start id of file `i + 1`, and an entry never spans two files.
//!
//! Each file is mapped once with address space reserved for it to grow, so
//! growing a file never moves its entries.  When an entry does not fit in the
//! last file's reservation a new file is started, each reserving at least as
//! much as all the files before it.
//!
//! # Commits
//!
//! [`append`](LocalRecordLog::append) and the `push_*` methods add entries
//! that are readable at once but not durable: reopening the log restores it to
//! its last commit.  A commit has two phases so that a caller can commit other
//! state, such as indexes, between them:
//!
//! 1. [`start_commit`](LocalRecordLog::start_commit) flushes the entries (and
//!    the directory, if files were created) and records the intent to commit
//!    them as [`Metadata::pre_next_id`].
//! 2. [`complete_commit`](LocalRecordLog::complete_commit) makes them the
//!    committed log by setting [`Metadata::next_id`].
//!
//! If a crash interrupts a commit, the reopened log has `pre_next_id` beyond
//! `next_id`, and the entries up to `pre_next_id` are intact on disk; the
//! caller decides whether to complete or abandon the commit.
//!
//! # Ids are never reused
//!
//! Other state, such as an index, may refer to an entry as soon as it is
//! appended, and may reach disk before the entry is committed.  So that such a
//! reference never comes to name a different entry, an id once handed out is
//! never handed out again: before appending past [`Metadata::reserved_id`] the
//! log raises it (in steps of 1 MiB) and flushes it, and when entries are
//! abandoned, by [`abandon_commit`](LocalRecordLog::abandon_commit) or by a
//! crash, the ids up to the highest handed out (or reserved, after a crash)
//! are filled with nodes and committed.
//!
//! One `LocalRecordLog` at a time may open a log; it takes an exclusive lock on
//! the metadata file.

use std::fmt;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read as _};
use std::os::unix::fs::FileExt as _;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use crate::mmap::{self, Mapping, round_up_to_page, suffixed};
use crate::record::{Record, Uuid};
use crate::record_log::{
    Entry, HEADER_SIZE, PersistentHeader, encode_tuple, encode_value, max_tuple_len, max_value_len,
};

/// The most data files a log may have.
pub const MAX_FILES: usize = 256;

const HEADER: u64 = HEADER_SIZE as u64;
const MIN_FILE_SIZE: u64 = 1 << 16;
const EXPAND_BY_BASE_LIMIT: u64 = 1 << 15;
const EXPAND_BY_20_PERCENT_LIMIT: u64 = 1 << 22;
const SNAPSHOT_CHUNK: u64 = 1 << 20;
const RESERVE_STEP: u64 = 1 << 20;

const METADATA_SIZE: u64 = 4096;
const MAGIC: u64 = u64::from_le_bytes(*b"gdbl-rl\0");
const VERSION: u64 = 1;
const SNAPSHOT_MAGIC: u64 = u64::from_le_bytes(*b"gdbl-rls");
const SNAPSHOT_HEADER_WORDS: usize = 6;

/// Errors from a local record log.
#[derive(Debug)]
pub enum Error {
    Io(io::Error),
    /// Encoding or decoding an entry failed.
    Record(crate::Error),
    /// The log's files or a snapshot are not a valid local record log.
    InvalidFormat(&'static str),
    InvalidArgument(&'static str),
    /// The log has used all [`MAX_FILES`] files.
    LogFull,
}

pub type Result<T> = std::result::Result<T, Error>;

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Io(e) => write!(f, "I/O error: {e}"),
            Error::Record(e) => write!(f, "record error: {e}"),
            Error::InvalidFormat(what) => write!(f, "invalid record log: {what}"),
            Error::InvalidArgument(what) => write!(f, "invalid argument: {what}"),
            Error::LogFull => f.write_str("record log has too many files"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Error::Io(e) => Some(e),
            Error::Record(e) => Some(e),
            _ => None,
        }
    }
}

impl From<io::Error> for Error {
    fn from(e: io::Error) -> Self {
        Error::Io(e)
    }
}

impl From<crate::Error> for Error {
    fn from(e: crate::Error) -> Self {
        Error::Record(e)
    }
}

/// Options for opening a log.
#[derive(Debug, Clone)]
pub struct Options {
    /// The id of the first entry of a newly created log.
    pub start_id: u64,
    /// The expected size in bytes, preallocated when the log is created.
    pub create_size: u64,
    /// The least address space reserved for each data file.
    pub min_file_reserve: u64,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            start_id: 1,
            create_size: 0,
            min_file_reserve: 1 << 30,
        }
    }
}

/// The persistent state of a log.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Metadata {
    /// Identifies the log; random when it is created and kept by snapshots.
    pub uuid: Uuid,
    /// The id of the first entry.
    pub start_id: u64,
    /// The id following the last committed entry.
    pub next_id: u64,
    /// The id following the last entry of a commit in progress, or equal to
    /// `next_id`.
    pub pre_next_id: u64,
    /// No id at or beyond this has been handed out.
    pub reserved_id: u64,
}

/// Whether a log exists at `prefix`.
pub fn exists(prefix: &Path) -> bool {
    suffixed(prefix, "metadata").exists()
}

/// Deletes the log at `prefix`.
pub fn remove(prefix: &Path) -> io::Result<()> {
    let paths = std::iter::once(suffixed(prefix, "metadata"))
        .chain((0..MAX_FILES).map(|i| file_path(prefix, i)));
    for path in paths {
        remove_if_exists(&path)?;
    }
    Ok(())
}

fn file_path(prefix: &Path, i: usize) -> PathBuf {
    suffixed(prefix, &i.to_string())
}

fn remove_if_exists(path: &Path) -> io::Result<()> {
    match fs::remove_file(path) {
        Err(e) if e.kind() != io::ErrorKind::NotFound => Err(e),
        _ => Ok(()),
    }
}

fn random_uuid() -> io::Result<Uuid> {
    let mut b = [0u8; 16];
    File::open("/dev/urandom")?.read_exact(&mut b)?;
    Ok(Uuid {
        uuid0: u64::from_le_bytes(b[..8].try_into().unwrap()),
        uuid1: u64::from_le_bytes(b[8..].try_into().unwrap()),
    })
}

/// The file size to grow to for `needed` bytes.
fn expand_size(needed: u64) -> u64 {
    if needed < EXPAND_BY_BASE_LIMIT {
        needed + MIN_FILE_SIZE
    } else if needed > EXPAND_BY_20_PERCENT_LIMIT {
        needed + needed / 5
    } else {
        needed * 2
    }
}

/// Metadata words.
#[derive(Clone, Copy)]
enum Word {
    Magic,
    Version,
    Uuid0,
    Uuid1,
    StartId,
    NextId,
    PreNextId,
    ReservedId,
}

struct MetadataFile {
    map: Mapping,
    /// Holds the lock that keeps a second writer out.
    _file: File,
}

impl MetadataFile {
    fn word(&self, w: Word) -> &AtomicU64 {
        // SAFETY: aligned and within the metadata file.
        unsafe { self.map.word(w as u64 * 8) }
    }

    fn load(&self, w: Word) -> u64 {
        u64::from_le(self.word(w).load(Ordering::Relaxed))
    }

    fn store(&self, w: Word, v: u64) {
        self.word(w).store(v.to_le(), Ordering::Relaxed);
    }

    fn read(&self) -> Metadata {
        Metadata {
            uuid: Uuid {
                uuid0: self.load(Word::Uuid0),
                uuid1: self.load(Word::Uuid1),
            },
            start_id: self.load(Word::StartId),
            next_id: self.load(Word::NextId),
            pre_next_id: self.load(Word::PreNextId),
            reserved_id: self.load(Word::ReservedId),
        }
    }

    fn write(&self, m: &Metadata) {
        self.store(Word::Uuid0, m.uuid.uuid0);
        self.store(Word::Uuid1, m.uuid.uuid1);
        self.store(Word::StartId, m.start_id);
        self.store(Word::NextId, m.next_id);
        self.store(Word::PreNextId, m.pre_next_id);
        self.store(Word::ReservedId, m.reserved_id);
    }
}

/// One mapped data file.
struct LogFile {
    start_id: u64,
    /// The bytes of the file that exist.
    len: u64,
    map: Mapping,
    file: File,
}

/// A record log stored in memory mapped files; see the [module](self) docs.
pub struct LocalRecordLog {
    prefix: Option<PathBuf>,
    options: Options,
    meta_file: MetadataFile,
    meta: Metadata,
    files: Vec<LogFile>,
    /// The id following the last appended entry.
    next: u64,
    /// The id following the highest id that may have been handed out.
    used: u64,
    /// Whether files have been created since the directory was last flushed.
    dir_dirty: bool,
}

impl LocalRecordLog {
    /// Opens or creates the log whose files start with `prefix`, or an
    /// in-memory log if `prefix` is `None`.
    pub fn open(prefix: Option<&Path>, options: Options) -> Result<Self> {
        let file = match prefix {
            Some(p) => OpenOptions::new()
                .read(true)
                .write(true)
                .create(true)
                .truncate(false)
                .open(suffixed(p, "metadata"))?,
            None => mmap::ephemeral_file("gdblite-rl-metadata")?,
        };
        mmap::lock_exclusive(&file)?;
        match file.metadata()?.len() {
            0 => file.set_len(METADATA_SIZE)?,
            METADATA_SIZE => {}
            _ => return Err(Error::InvalidFormat("metadata size")),
        }
        let meta_file = MetadataFile {
            map: Mapping::new(&file, METADATA_SIZE)?,
            _file: file,
        };
        // A log that crashed before writing its magic was never used.
        let fresh = meta_file.load(Word::Magic) == 0;
        if !fresh
            && (meta_file.load(Word::Magic) != MAGIC || meta_file.load(Word::Version) != VERSION)
        {
            return Err(Error::InvalidFormat("bad magic or version"));
        }
        let meta = if fresh {
            Metadata {
                uuid: random_uuid()?,
                start_id: options.start_id,
                next_id: options.start_id,
                pre_next_id: options.start_id,
                reserved_id: options.start_id,
            }
        } else {
            meta_file.read()
        };
        if meta.next_id < meta.start_id
            || meta.pre_next_id < meta.next_id
            || meta.reserved_id < meta.pre_next_id
        {
            return Err(Error::InvalidFormat("inconsistent metadata"));
        }
        let mut log = LocalRecordLog {
            prefix: prefix.map(Path::to_path_buf),
            options,
            meta_file,
            meta,
            files: Vec::new(),
            next: meta.next_id,
            used: meta.reserved_id,
            dir_dirty: fresh,
        };
        if fresh {
            for i in 0..MAX_FILES {
                if let Some(p) = &log.prefix {
                    remove_if_exists(&file_path(p, i))?;
                }
            }
            let size = HEADER.saturating_add(log.options.create_size);
            log.add_file(size)?;
            log.meta_file.write(&log.meta);
            log.meta_file.store(Word::Version, VERSION);
            log.meta_file.store(Word::Magic, MAGIC);
        } else {
            log.load_files()?;
            if !log.has_pending_commit() {
                log.fill_unused()?;
            }
        }
        Ok(log)
    }

    /// Maps the existing data files, removing any started after the last
    /// commit.
    fn load_files(&mut self) -> Result<()> {
        let prefix = self.prefix.clone().expect("persistent log");
        let mut found = Vec::new();
        for i in 0..MAX_FILES {
            let path = file_path(&prefix, i);
            let file = match OpenOptions::new().read(true).write(true).open(&path) {
                Ok(f) => f,
                Err(e) if e.kind() == io::ErrorKind::NotFound => break,
                Err(e) => return Err(e.into()),
            };
            let mut header = [0u8; HEADER_SIZE];
            let len = file.metadata()?.len();
            let header = (len >= HEADER)
                .then(|| file.read_exact_at(&mut header, 0).map(|_| header))
                .transpose()?
                .map(|h| PersistentHeader::read(&h).expect("header sized"))
                .filter(PersistentHeader::is_valid);
            // A file whose header was never written, or that starts beyond
            // the committed entries, holds nothing committed.
            match header {
                Some(h) if h.start_id <= self.meta.pre_next_id => found.push((file, len, h)),
                _ => {
                    for j in i..MAX_FILES {
                        remove_if_exists(&file_path(&prefix, j))?;
                    }
                    break;
                }
            }
        }
        let Some((_, _, first)) = found.first() else {
            return Err(Error::InvalidFormat("no data files"));
        };
        if first.start_id != self.meta.start_id {
            return Err(Error::InvalidFormat("first file does not start the log"));
        }
        let mut reserved = 0;
        let n = found.len();
        for (i, (file, len, header)) in found.into_iter().enumerate() {
            let end = match self.files.last() {
                Some(prev) if header.start_id <= prev.start_id => {
                    return Err(Error::InvalidFormat("file start ids out of order"));
                }
                _ => header.start_id,
            };
            if let Some(prev) = self.files.last()
                && prev.start_id + (prev.len - HEADER) < end
            {
                return Err(Error::InvalidFormat("data file too short"));
            }
            let reserve = if i + 1 == n {
                round_up_to_page(len.max(self.options.min_file_reserve).max(reserved))
            } else {
                round_up_to_page(len)
            };
            reserved += reserve;
            self.files.push(LogFile {
                start_id: header.start_id,
                len,
                map: Mapping::new(&file, reserve)?,
                file,
            });
        }
        let last = self.files.last().expect("found files");
        if last.start_id + (last.len - HEADER) < self.meta.pre_next_id {
            return Err(Error::InvalidFormat("data file too short"));
        }
        Ok(())
    }

    /// Starts a data file at the next id with room for at least `size`
    /// bytes, header included.
    fn add_file(&mut self, size: u64) -> Result<()> {
        // An empty last file is replaced rather than followed.
        if self.files.last().is_some_and(|f| f.start_id == self.next) {
            self.files.pop();
        }
        let i = self.files.len();
        if i == MAX_FILES {
            return Err(Error::LogFull);
        }
        let file = match &self.prefix {
            Some(p) => OpenOptions::new()
                .read(true)
                .write(true)
                .create(true)
                .truncate(true)
                .open(file_path(p, i))?,
            None => mmap::ephemeral_file(&format!("gdblite-rl{i}"))?,
        };
        let len = round_up_to_page(size.max(MIN_FILE_SIZE));
        let reserved: u64 = self.files.iter().map(|f| f.map.len()).sum();
        let reserve = round_up_to_page(len.max(self.options.min_file_reserve).max(reserved));
        mmap::allocate(&file, len)?;
        let mut map = Mapping::new(&file, reserve)?;
        let header = PersistentHeader::new(self.next).to_bytes();
        // SAFETY: the file has at least `len >= HEADER` bytes.
        unsafe { map.bytes_mut(0, HEADER) }.copy_from_slice(&header);
        self.files.push(LogFile {
            start_id: self.next,
            len,
            map,
            file,
        });
        self.dir_dirty = true;
        Ok(())
    }

    /// The `max_len` bytes at the next id, growing the last file or starting
    /// a new one to make room.
    fn room(&mut self, max_len: u64) -> Result<&mut [u8]> {
        let last = self.files.last().expect("a log has a file");
        let offset = HEADER + (self.next - last.start_id);
        let end = offset.saturating_add(max_len);
        if end > last.map.len() {
            self.add_file(HEADER.saturating_add(max_len))?;
        } else if end > last.len {
            let len = round_up_to_page(expand_size(end)).min(last.map.len());
            mmap::allocate(&last.file, len)?;
            self.files.last_mut().expect("a log has a file").len = len;
        }
        let last = self.files.last_mut().expect("a log has a file");
        let offset = HEADER + (self.next - last.start_id);
        // SAFETY: the file has been extended to cover the range.
        Ok(unsafe { last.map.bytes_mut(offset, max_len) })
    }

    /// Appends an entry of at most `max_len` bytes written by `encode`,
    /// returning its id.
    fn push(
        &mut self,
        max_len: usize,
        encode: impl FnOnce(&mut [u8], u64) -> crate::Result<usize>,
    ) -> Result<u64> {
        self.check_no_pending_commit()?;
        let end = self.next.saturating_add(max_len as u64);
        if end > self.meta.reserved_id {
            self.meta.reserved_id = end.next_multiple_of(RESERVE_STEP);
            self.write_metadata()?;
        }
        let id = self.next;
        let n = encode(self.room(max_len as u64)?, id)?;
        self.next += n as u64;
        self.used = self.used.max(self.next);
        Ok(id)
    }

    /// Appends `record`, which must have the next id.
    pub fn append(&mut self, record: &Record<'_>) -> Result<()> {
        if record.id() != self.next {
            return Err(Error::InvalidArgument("record id is not the next id"));
        }
        let bytes = record.bytes();
        self.push(bytes.len(), |out, _| {
            out[..bytes.len()].copy_from_slice(bytes);
            Ok(bytes.len())
        })?;
        Ok(())
    }

    /// Appends a node, returning its id.
    pub fn push_node(&mut self) -> Result<u64> {
        self.push_value(&[])
    }

    /// Appends a value holding `data`, returning its id.
    pub fn push_value(&mut self, data: &[u8]) -> Result<u64> {
        self.push(max_value_len(data.len()), |out, _| encode_value(out, data))
    }

    /// Appends a tuple of `ids`, returning its id.
    pub fn push_tuple(&mut self, extra: bool, ids: &[u64]) -> Result<u64> {
        self.push(max_tuple_len(ids.len()), |out, id| {
            encode_tuple(out, id, extra, ids)
        })
    }

    pub fn is_transient(&self) -> bool {
        self.prefix.is_none()
    }

    /// The id of the first entry.
    pub fn start_id(&self) -> u64 {
        self.meta.start_id
    }

    /// The id following the last appended entry, committed or not.
    pub fn next_id(&self) -> u64 {
        self.next
    }

    /// The persistent state as of the last commit or commit phase.
    pub fn metadata(&self) -> Metadata {
        self.meta
    }

    /// The number of data files.
    pub fn file_count(&self) -> usize {
        self.files.len()
    }

    /// The id following the entries of file `i`.
    fn file_end(&self, i: usize) -> u64 {
        self.files.get(i + 1).map_or(self.next, |f| f.start_id)
    }

    /// The bytes of the file holding `id` from `id` to the end of the file's
    /// entries.
    fn bytes_from(&self, id: u64) -> Option<&[u8]> {
        if id < self.start_id() || id >= self.next {
            return None;
        }
        let i = self.files.partition_point(|f| f.start_id <= id) - 1;
        let f = &self.files[i];
        // SAFETY: the file's entries up to its end are backed, and are only
        // written through `&mut self`.
        Some(unsafe {
            f.map
                .bytes(HEADER + (id - f.start_id), self.file_end(i) - id)
        })
    }

    /// The entry at `id`, borrowed from the log.
    pub fn entry(&self, id: u64) -> Result<Entry<'_>> {
        let bytes = self.bytes_from(id).ok_or(crate::Error::IdOutOfRange)?;
        Ok(Entry::parse(bytes)?)
    }

    /// The entry at `id`.
    pub fn get(&self, id: u64) -> Result<Record<'_>> {
        let bytes = self.bytes_from(id).ok_or(crate::Error::IdOutOfRange)?;
        Ok(Record::from_bytes(id, bytes)?)
    }

    /// The encoded entries from `start` up to `end`, as one slice per file
    /// along with the id it starts at.  `end` may fall inside an entry.
    pub fn range(&self, start: u64, end: u64) -> Result<impl Iterator<Item = (u64, &[u8])>> {
        if start < self.start_id() || end > self.next || start > end {
            return Err(crate::Error::IdOutOfRange.into());
        }
        let mut id = start;
        Ok(std::iter::from_fn(move || {
            if id >= end {
                return None;
            }
            let bytes = self.bytes_from(id).expect("in range");
            let bytes = &bytes[..bytes.len().min((end - id) as usize)];
            let chunk = (id, bytes);
            id += bytes.len() as u64;
            Some(chunk)
        }))
    }

    /// The entries from `id` (which must start an entry) to the end.
    pub fn records(&self, id: u64) -> Records<'_> {
        Records { log: self, id }
    }

    /// Flushes the entries from file `first` on to disk.
    fn sync_files(&self, first: usize) -> Result<()> {
        if self.is_transient() {
            return Ok(());
        }
        for f in &self.files[first..] {
            f.map.sync(f.len)?;
        }
        Ok(())
    }

    fn write_metadata(&mut self) -> Result<()> {
        self.meta_file.write(&self.meta);
        if !self.is_transient() {
            self.meta_file.map.sync(METADATA_SIZE)?;
        }
        Ok(())
    }

    /// Whether the log was opened with an interrupted commit, which must be
    /// completed or abandoned before anything else is appended or committed.
    pub fn has_pending_commit(&self) -> bool {
        self.meta.pre_next_id > self.next
    }

    fn check_no_pending_commit(&self) -> Result<()> {
        if self.has_pending_commit() {
            return Err(Error::InvalidArgument("a commit is pending"));
        }
        Ok(())
    }

    /// The first phase of a commit: flushes the appended entries to disk and
    /// records the intent to commit them.
    pub fn start_commit(&mut self) -> Result<()> {
        self.check_no_pending_commit()?;
        let first = self
            .files
            .partition_point(|f| f.start_id <= self.meta.next_id)
            - 1;
        self.sync_files(first)?;
        if self.dir_dirty
            && let Some(p) = &self.prefix
        {
            mmap::sync_dir(p)?;
        }
        self.dir_dirty = false;
        self.meta.pre_next_id = self.next;
        self.write_metadata()
    }

    /// The second phase of a commit: makes the entries flushed by
    /// [`start_commit`](Self::start_commit) the committed log.
    /// Also completes an interrupted commit found on opening.
    pub fn complete_commit(&mut self) -> Result<()> {
        let pending = self.has_pending_commit();
        self.meta.next_id = self.meta.pre_next_id;
        self.next = self.next.max(self.meta.next_id);
        if !pending {
            // Only the ids up to `used` have been handed out, so a clean
            // reopen has nothing to fill.
            self.meta.reserved_id = self.used;
        }
        self.write_metadata()?;
        if pending {
            self.fill_unused()?;
        }
        Ok(())
    }

    /// Commits the appended entries in one step.
    pub fn commit(&mut self) -> Result<()> {
        self.start_commit()?;
        self.complete_commit()
    }

    /// Abandons an interrupted commit found on opening and any entries
    /// appended since the last commit.  Their ids are not reused: they become
    /// nodes, which are committed.
    pub fn abandon_commit(&mut self) -> Result<()> {
        self.meta.pre_next_id = self.meta.next_id;
        self.next = self.meta.next_id;
        self.write_metadata()?;
        while self.files.len() > 1 && self.files.last().expect("files").start_id > self.next {
            self.files.pop();
            if let Some(p) = &self.prefix {
                remove_if_exists(&file_path(p, self.files.len()))?;
            }
        }
        self.fill_unused()
    }

    /// Fills the ids from the next id up to the highest that may have been
    /// handed out with nodes, and commits them.
    fn fill_unused(&mut self) -> Result<()> {
        if self.used <= self.next {
            return Ok(());
        }
        self.room(self.used - self.next)?.fill(0);
        self.next = self.used;
        self.commit()
    }

    /// Writes a snapshot of the log, including entries not yet committed, to
    /// `sink` as a series of chunks: a header, then the log as a single
    /// [`RecordLog`](crate::RecordLog) in chunks of at most 1 MiB.
    pub fn snapshot(&self, mut sink: impl FnMut(&[u8]) -> io::Result<()>) -> Result<()> {
        let words = [
            SNAPSHOT_MAGIC,
            VERSION,
            self.meta.uuid.uuid0,
            self.meta.uuid.uuid1,
            self.start_id(),
            self.next,
        ];
        let header: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
        sink(&header)?;
        sink(&PersistentHeader::new(self.start_id()).to_bytes())?;
        for (_, bytes) in self.range(self.start_id(), self.next)? {
            for chunk in bytes.chunks(SNAPSHOT_CHUNK as usize) {
                sink(chunk)?;
            }
        }
        Ok(())
    }

    /// Creates a log at `prefix` (or in memory) from the chunks of a
    /// [`snapshot`](Self::snapshot), returned in order by `source`.  The
    /// restored entries are committed.  `options.start_id` is ignored.
    pub fn restore(
        prefix: Option<&Path>,
        options: Options,
        mut source: impl FnMut() -> io::Result<Vec<u8>>,
    ) -> Result<Self> {
        if prefix.is_some_and(exists) {
            return Err(io::Error::from(io::ErrorKind::AlreadyExists).into());
        }
        let result = (|| {
            let header = source()?;
            if header.len() != SNAPSHOT_HEADER_WORDS * 8 {
                return Err(Error::InvalidFormat("snapshot header size"));
            }
            let word = |i: usize| u64::from_le_bytes(header[i * 8..i * 8 + 8].try_into().unwrap());
            if word(0) != SNAPSHOT_MAGIC || word(1) != VERSION {
                return Err(Error::InvalidFormat("bad snapshot magic or version"));
            }
            let (start_id, next_id) = (word(4), word(5));
            let body = next_id
                .checked_sub(start_id)
                .ok_or(Error::InvalidFormat("snapshot ids"))?;
            let mut log = LocalRecordLog::open(
                prefix,
                Options {
                    start_id,
                    create_size: options.create_size.max(body),
                    ..options
                },
            )?;
            log.meta.uuid = Uuid {
                uuid0: word(2),
                uuid1: word(3),
            };
            let total = HEADER + body;
            let mut data = Vec::with_capacity(HEADER_SIZE);
            let mut offset = 0;
            while offset < total {
                let chunk = source()?;
                if chunk.is_empty() {
                    return Err(Error::InvalidFormat("snapshot truncated"));
                }
                if offset + chunk.len() as u64 > total {
                    return Err(Error::InvalidFormat("snapshot too long"));
                }
                // The log's header is checked rather than copied.
                let (head, rest) =
                    chunk.split_at(HEADER.saturating_sub(offset).min(chunk.len() as u64) as usize);
                data.extend_from_slice(head);
                let f = &mut log.files[0];
                let at = offset + head.len() as u64;
                // SAFETY: file 0 was created with room for the whole log.
                unsafe { f.map.bytes_mut(at, rest.len() as u64) }.copy_from_slice(rest);
                offset += chunk.len() as u64;
            }
            let h = PersistentHeader::read(&data)?;
            if !h.is_valid() || h.start_id != start_id {
                return Err(Error::InvalidFormat("snapshot log header"));
            }
            log.next = next_id;
            log.used = next_id;
            log.meta.reserved_id = next_id;
            log.commit()?;
            Ok(log)
        })();
        if result.is_err()
            && let Some(p) = prefix
        {
            let _ = remove(p);
        }
        result
    }
}

/// An iterator over the entries of a [`LocalRecordLog`]; see
/// [`records`](LocalRecordLog::records).
pub struct Records<'a> {
    log: &'a LocalRecordLog,
    id: u64,
}

impl<'a> Iterator for Records<'a> {
    type Item = Result<Record<'a>>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.id >= self.log.next {
            return None;
        }
        let r = self.log.get(self.id);
        self.id = match &r {
            Ok(r) => r.next_id(),
            Err(_) => self.log.next,
        };
        Some(r)
    }
}

#[cfg(test)]
mod tests;
