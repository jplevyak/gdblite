//! A persistent hash table mapping 64 or 128 bit keys to lists of `u64` values.
//!
//! Ported from the C++ `DynamicTable`, fixing its bugs along the way.
//!
//! # Layout
//!
//! Each key is a non-zero `key` plus an optional `key2` (zero for a 64 bit key).
//! A key maps to an extra bit and a list of non-zero values, built up by
//! [`insert`](Writer::insert) (append), [`replace`](Writer::replace) and
//! [`delete`](Writer::delete).
//!
//! The table is a series of levels, each a file of 128 byte buckets; level `i`
//! has `2^(i + 7)` base buckets followed by overflow buckets.  A key's bucket is
//! the top bits of its hash, and its operations are appended to that bucket as
//! compact varint records, chaining to overflow buckets when it fills.  When
//! more than 1% of a level's buckets have overflowed, the table expands: a few
//! buckets at a time, each base bucket's records are merged and copied into the
//! two buckets of the next level that share its hash prefix, and the bucket is
//! marked as moved.
//!
//! # Concurrency
//!
//! There is one [`Writer`] and any number of [`Reader`]s, which may run on other
//! threads while the writer is inserting.  Readers take no locks.  The writer
//! only appends to buckets, storing 8 byte words in order, and readers copy a
//! bucket word by word and ignore any record that is incomplete.
//!
//! Each level's file is mapped at a fixed address with room reserved for it to
//! grow, so growing a file never moves its mapping.  When expansion finishes,
//! the old level is unmapped after [`Options::retire_delay`]; a reader still in
//! the middle of a lookup on it after that long faults rather than reading
//! garbage.
//!
//! # Crash behavior
//!
//! A record that was only partly written when the writer crashed is detected and
//! cleared by the next write to its bucket, and an expansion step that was
//! interrupted is redone.  Nothing is flushed to disk unless
//! [`sync`](Writer::sync) is called, so after a power loss recent operations may
//! be lost.

mod record;
#[cfg(test)]
mod tests;

use std::collections::{BTreeMap, VecDeque};
use std::fmt;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Write as _};
use std::path::{Path, PathBuf};
use std::ptr;
use std::sync::atomic::{AtomicPtr, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use crate::mmap::{self, Mapping, round_up_to_page, suffixed};
use record::{BUCKET_BYTES, BUCKET_HEADER_BYTES, KeyState, Kind, MAX_RECORD_BYTES};

/// The most values one [`insert`](Writer::insert) or
/// [`replace`](Writer::replace) may add.  A key may accumulate any number.
pub const MAX_VALUES: usize = record::MAX_VALUES;

const BUCKET_WORDS: usize = BUCKET_BYTES / 8;
const BUCKETS_PER_LEVEL_LOG2: u32 = 7;
/// The bucket index uses the top `level + 7` hash bits, at most 63.
const MAX_LEVELS: usize = 57;
const HEADER_NO_OVERFLOW: u32 = 0;
const HEADER_EXTENDED: u32 = u32::MAX;
const BUCKETS_PER_EXPAND_STEP: u64 = 16;
const INSERTIONS_PER_EXPAND_STEP: u64 = 16;
const OVERFLOW_HIGHWATER: f64 = 0.01;
/// Overflow space preallocated for a new level, as a fraction of its base.
const OVERFLOW_PREALLOCATE_DIVISOR: u64 = 50;
// Generated using efficient64 from https://github.com/wizykowski/miller-rabin
const HASH_PRIME: u64 = 13709122941870337439;
const HASH_PRIME2: u64 = 0x9e37_79b9_7f4a_7c15;
const SNAPSHOT_CHUNK: u64 = 1 << 20;

const METADATA_SIZE: u64 = 4096;
const MAGIC: u64 = u64::from_le_bytes(*b"gdbl-dt\0");
const VERSION: u64 = 1;
/// Metadata words before the per-level entries: magic, version, 2 reserved.
const METADATA_HEADER_WORDS: usize = 4;

/// Errors from a dynamic table.
#[derive(Debug)]
pub enum Error {
    Io(io::Error),
    /// The table's files are not a valid dynamic table.
    InvalidFormat(&'static str),
    InvalidArgument(&'static str),
    /// A level has used all of its reserved address space.
    TableFull,
}

pub type Result<T> = std::result::Result<T, Error>;

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Io(e) => write!(f, "I/O error: {e}"),
            Error::InvalidFormat(what) => write!(f, "invalid dynamic table: {what}"),
            Error::InvalidArgument(what) => write!(f, "invalid argument: {what}"),
            Error::TableFull => f.write_str("dynamic table level is full"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Error::Io(e) => Some(e),
            _ => None,
        }
    }
}

impl From<io::Error> for Error {
    fn from(e: io::Error) -> Self {
        Error::Io(e)
    }
}

/// Options for opening a table.
#[derive(Debug, Clone)]
pub struct Options {
    /// The expected size in bytes, used to start with enough buckets to skip
    /// the early expansions.
    pub create_size: u64,
    /// The least address space reserved for each level.  A level reserves at
    /// least twice its base size, leaving as much room again for overflow.
    pub min_level_reserve: u64,
    /// How long after an expansion finishes the old level stays mapped for
    /// readers still using it.
    pub retire_delay: Duration,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            create_size: 0,
            min_level_reserve: 256 << 20,
            retire_delay: Duration::from_secs(300),
        }
    }
}

/// The result of a lookup.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Lookup {
    pub bit: bool,
    pub values: Vec<u64>,
}

fn hash(key: u64, key2: u64) -> u64 {
    (key ^ key2.wrapping_mul(HASH_PRIME2)).wrapping_mul(HASH_PRIME)
}

fn level_path(prefix: &Path, level: usize) -> PathBuf {
    suffixed(prefix, &level.to_string())
}

/// Whether a table exists at `prefix`.
pub fn exists(prefix: &Path) -> bool {
    suffixed(prefix, "metadata").exists()
}

/// Deletes the table at `prefix`.
pub fn remove(prefix: &Path) -> io::Result<()> {
    let paths = std::iter::once(suffixed(prefix, "metadata"))
        .chain((0..MAX_LEVELS).map(|l| level_path(prefix, l)));
    for path in paths {
        match fs::remove_file(path) {
            Err(e) if e.kind() != io::ErrorKind::NotFound => return Err(e),
            _ => {}
        }
    }
    Ok(())
}

/// One mapped level.
struct Level {
    index: usize,
    base_buckets: u64,
    map: Mapping,
    /// The bytes of the file that exist, published after the file is extended.
    allocated: AtomicU64,
}

impl Level {
    fn bucket(&self, b: u64) -> Option<&[AtomicU64; BUCKET_WORDS]> {
        let offset = b.checked_mul(BUCKET_BYTES as u64)?;
        if offset.checked_add(BUCKET_BYTES as u64)? > self.allocated.load(Ordering::Acquire) {
            return None;
        }
        // SAFETY: bucket offsets are aligned and the bucket is allocated.
        Some(unsafe { self.map.words(offset) })
    }

    /// A copy of bucket `b`, or `None` if it is past the end of the file.
    fn read(&self, b: u64) -> Option<[u8; BUCKET_BYTES]> {
        let words = self.bucket(b)?;
        let mut bytes = [0u8; BUCKET_BYTES];
        for (chunk, word) in bytes.as_chunks_mut::<8>().0.iter_mut().zip(words) {
            *chunk = word.load(Ordering::Acquire).to_ne_bytes();
        }
        Some(bytes)
    }

    fn header(&self, b: u64) -> u32 {
        let word = self.bucket(b).expect("base bucket")[0].load(Ordering::Acquire);
        u32::from_le_bytes(
            word.to_ne_bytes()[..BUCKET_HEADER_BYTES]
                .try_into()
                .unwrap(),
        )
    }

    fn base_bucket(&self, hash: u64) -> u64 {
        hash >> (64 - (self.index as u32 + BUCKETS_PER_LEVEL_LOG2))
    }

    /// The bucket a non-zero bucket header links to.
    fn overflow_bucket(&self, header: u32) -> u64 {
        self.base_buckets + u64::from(header) - 1
    }
}

/// State shared by the writer and readers.
struct Shared {
    /// The current level, where lookups start.
    level: AtomicUsize,
    /// The mapped levels, null once retired.
    levels: [AtomicPtr<Level>; MAX_LEVELS],
    /// Owns every level ever mapped, so `levels` never dangles.  Boxed so a
    /// level stays put when the vector grows.
    #[allow(clippy::vec_box)]
    owned: Mutex<Vec<Box<Level>>>,
}

impl Shared {
    fn level(&self, i: usize) -> Option<&Level> {
        let p = self.levels.get(i)?.load(Ordering::Acquire);
        // SAFETY: non-null pointers point into `owned`, which outlives `self`.
        unsafe { p.as_ref() }
    }

    fn lookup(&self, key: u64, key2: u64, values: &mut Vec<u64>) -> Option<bool> {
        let h = hash(key, key2);
        let mut level = self.level.load(Ordering::Acquire);
        'restart: loop {
            values.clear();
            let Some(lvl) = self.level(level) else {
                // The level was retired: start over at the current level.
                let current = self.level.load(Ordering::Acquire);
                if current == level {
                    return None;
                }
                level = current;
                continue;
            };
            let mut bit = false;
            let mut b = lvl.base_bucket(h);
            let mut head = true;
            while let Some(bytes) = lvl.read(b) {
                let header = record::bucket_header(&bytes);
                if head && header == HEADER_EXTENDED {
                    level += 1;
                    continue 'restart;
                }
                head = false;
                record::parse_bucket(&bytes, |r| {
                    if r.key == key && r.key2 == key2 {
                        record::merge(values, &mut bit, r);
                    }
                });
                if header == HEADER_NO_OVERFLOW || header == HEADER_EXTENDED {
                    break;
                }
                let next = lvl.overflow_bucket(header);
                if next <= b {
                    break; // Corrupt: chains only link forward.
                }
                b = next;
            }
            return (!values.is_empty()).then_some(bit);
        }
    }
}

#[derive(Clone, Copy)]
enum Field {
    BaseBuckets,
    FileSize,
    OverflowBuckets,
    ExpandedBuckets,
}

/// The metadata file: a header, then four words per level.
struct Metadata {
    map: Mapping,
}

impl Metadata {
    fn word(&self, i: usize) -> &AtomicU64 {
        assert!((i as u64 + 1) * 8 <= METADATA_SIZE);
        // SAFETY: aligned and within the metadata file.
        unsafe { self.map.word(i as u64 * 8) }
    }

    fn load(&self, i: usize) -> u64 {
        u64::from_le(self.word(i).load(Ordering::Relaxed))
    }

    fn store(&self, i: usize, v: u64) {
        self.word(i).store(v.to_le(), Ordering::Relaxed);
    }

    fn get(&self, level: usize, field: Field) -> u64 {
        self.load(METADATA_HEADER_WORDS + 4 * level + field as usize)
    }

    fn set(&self, level: usize, field: Field, v: u64) {
        self.store(METADATA_HEADER_WORDS + 4 * level + field as usize, v);
    }
}

/// The current level: the first that exists and is not fully expanded.
fn current_level(get: impl Fn(usize, Field) -> u64) -> Option<usize> {
    (0..MAX_LEVELS).find(|&l| {
        let base = get(l, Field::BaseBuckets);
        base != 0 && get(l, Field::ExpandedBuckets) < base
    })
}

/// The first level of a new table expected to hold `create_size` bytes.
fn start_level(create_size: u64) -> usize {
    let buckets = create_size.div_ceil(BUCKET_BYTES as u64).max(1);
    let log2 = 64 - (buckets - 1).leading_zeros();
    (log2.saturating_sub(BUCKETS_PER_LEVEL_LOG2) as usize).min(MAX_LEVELS - 1)
}

fn base_buckets(level: usize) -> u64 {
    1 << (level as u32 + BUCKETS_PER_LEVEL_LOG2)
}

/// A read handle: cheap to clone and safe to use from any thread.
#[derive(Clone)]
pub struct Reader {
    shared: Arc<Shared>,
}

impl Reader {
    pub fn get(&self, key: u64, key2: u64) -> Option<Lookup> {
        let mut values = Vec::new();
        let bit = self.get_into(key, key2, &mut values)?;
        Some(Lookup { bit, values })
    }

    /// Looks up a key, filling `values` and returning the extra bit if found.
    pub fn get_into(&self, key: u64, key2: u64, values: &mut Vec<u64>) -> Option<bool> {
        self.shared.lookup(key, key2, values)
    }
}

/// The single writer of a table.
pub struct Writer {
    shared: Arc<Shared>,
    meta: Metadata,
    /// Holds the lock that keeps a second writer out.
    _meta_file: File,
    files: Vec<Option<File>>,
    prefix: Option<PathBuf>,
    options: Options,
    inserts: u64,
    /// Levels whose expansion finished, and when.
    retiring: VecDeque<(usize, Instant)>,
    /// For tests: called before each word of a record is stored, and returning
    /// true abandons the rest of the record, as if the writer crashed.
    fault: Option<Box<dyn FnMut() -> bool + Send>>,
}

impl Writer {
    /// Opens or creates the table whose files start with `prefix`, or an
    /// in-memory table if `prefix` is `None`.
    pub fn open(prefix: Option<&Path>, options: Options) -> Result<Writer> {
        let meta_file = match prefix {
            Some(p) => OpenOptions::new()
                .read(true)
                .write(true)
                .create(true)
                .truncate(false)
                .open(suffixed(p, "metadata"))?,
            None => mmap::ephemeral_file("gdblite-metadata")?,
        };
        Writer::open_files(prefix, options, meta_file, Vec::new())
    }

    /// Opens a table from its metadata file and any level files already open,
    /// as `(level, file)`.
    fn open_files(
        prefix: Option<&Path>,
        options: Options,
        meta_file: File,
        level_files: Vec<(usize, File)>,
    ) -> Result<Writer> {
        mmap::lock_exclusive(&meta_file)?;
        match meta_file.metadata()?.len() {
            0 => meta_file.set_len(METADATA_SIZE)?,
            METADATA_SIZE => {}
            _ => return Err(Error::InvalidFormat("metadata size")),
        }
        let meta = Metadata {
            map: Mapping::new(&meta_file, METADATA_SIZE)?,
        };
        // A table that crashed before writing its magic was never used.
        let fresh = meta.load(0) == 0;
        if !fresh && (meta.load(0) != MAGIC || meta.load(1) != VERSION) {
            return Err(Error::InvalidFormat("bad magic or version"));
        }

        let shared = Arc::new(Shared {
            level: AtomicUsize::new(0),
            levels: std::array::from_fn(|_| AtomicPtr::new(ptr::null_mut())),
            owned: Mutex::new(Vec::new()),
        });
        let mut files: Vec<Option<File>> = (0..MAX_LEVELS).map(|_| None).collect();
        for (l, file) in level_files {
            files[l] = Some(file);
        }
        let mut writer = Writer {
            shared,
            meta,
            _meta_file: meta_file,
            files,
            prefix: prefix.map(Path::to_path_buf),
            options,
            inserts: 0,
            retiring: VecDeque::new(),
            fault: None,
        };
        let current = match current_level(|l, f| writer.meta.get(l, f)) {
            Some(l) => l,
            None => start_level(writer.options.create_size),
        };
        // Levels below the current one were fully expanded.
        for l in 0..current {
            if writer.meta.get(l, Field::BaseBuckets) != 0 {
                writer.remove_level_file(l)?;
            }
        }
        writer.map_level(current)?;
        if current + 1 < MAX_LEVELS && writer.meta.get(current + 1, Field::BaseBuckets) != 0 {
            writer.map_level(current + 1)?;
        }
        writer.shared.level.store(current, Ordering::Release);
        if fresh {
            writer.meta.store(1, VERSION);
            writer.meta.store(0, MAGIC);
        }
        Ok(writer)
    }

    pub fn reader(&self) -> Reader {
        Reader {
            shared: Arc::clone(&self.shared),
        }
    }

    pub fn get(&self, key: u64, key2: u64) -> Option<Lookup> {
        self.reader().get(key, key2)
    }

    /// Appends 1 to [`MAX_VALUES`] values to a key and sets its extra bit.
    /// Repeating the key's most recent operation exactly is a no-op.
    pub fn insert(&mut self, key: u64, key2: u64, bit: bool, values: &[u64]) -> Result<()> {
        self.apply(Kind::Insert, key, key2, bit, values, false)
    }

    /// Replaces a key's values with 1 to [`MAX_VALUES`] values and sets its
    /// extra bit.
    pub fn replace(&mut self, key: u64, key2: u64, bit: bool, values: &[u64]) -> Result<()> {
        self.apply(Kind::Replace, key, key2, bit, values, false)
    }

    pub fn delete(&mut self, key: u64, key2: u64) -> Result<()> {
        self.apply(Kind::Delete, key, key2, false, &[], false)
    }

    /// Does periodic maintenance: an expansion step if one is in progress, and
    /// unmapping and deleting levels whose retire delay has passed.
    pub fn tick(&mut self) -> Result<()> {
        if self.is_expanding() {
            self.expand_step()?;
        }
        let now = Instant::now();
        while let Some(&(level, at)) = self.retiring.front() {
            if now.duration_since(at) < self.options.retire_delay {
                break;
            }
            self.retiring.pop_front();
            self.retire(level)?;
        }
        Ok(())
    }

    /// Flushes the table, and the directory holding its files, to disk.
    pub fn sync(&self) -> Result<()> {
        for lvl in self.live_levels() {
            lvl.map.sync(lvl.allocated.load(Ordering::Relaxed))?;
        }
        self.meta.map.sync(METADATA_SIZE)?;
        if let Some(p) = &self.prefix {
            mmap::sync_dir(p)?;
        }
        Ok(())
    }

    /// Writes a snapshot of the table to `sink` as a series of chunks: the
    /// metadata, then each live level's file in chunks of at most 1 MiB.
    pub fn snapshot(&mut self, mut sink: impl FnMut(&[u8]) -> io::Result<()>) -> Result<()> {
        let mut buf = Vec::new();
        copy_words(&self.meta.map, 0, METADATA_SIZE, &mut buf);
        sink(&buf)?;
        for lvl in self.live_levels() {
            let size = lvl.allocated.load(Ordering::Relaxed);
            let mut offset = 0;
            while offset < size {
                let len = (size - offset).min(SNAPSHOT_CHUNK);
                copy_words(&lvl.map, offset, len, &mut buf);
                sink(&buf)?;
                offset += len;
            }
        }
        Ok(())
    }

    /// Creates a table at `prefix`, or in memory if `prefix` is `None`, from
    /// the chunks of a [`snapshot`](Self::snapshot), returned in order by
    /// `source`.
    pub fn restore(
        prefix: Option<&Path>,
        options: Options,
        mut source: impl FnMut() -> io::Result<Vec<u8>>,
    ) -> Result<Writer> {
        if prefix.is_some_and(exists) {
            return Err(io::Error::from(io::ErrorKind::AlreadyExists).into());
        }
        let result = (|| {
            let meta = source()?;
            if meta.len() as u64 != METADATA_SIZE {
                return Err(Error::InvalidFormat("snapshot metadata size"));
            }
            let word = |i: usize| u64::from_le_bytes(meta[i * 8..i * 8 + 8].try_into().unwrap());
            if word(0) != MAGIC || word(1) != VERSION {
                return Err(Error::InvalidFormat("bad magic or version"));
            }
            let get = |l: usize, f: Field| word(METADATA_HEADER_WORDS + 4 * l + f as usize);
            let current = current_level(get).ok_or(Error::InvalidFormat("no current level"))?;
            let mut live = vec![current];
            if current + 1 < MAX_LEVELS && get(current + 1, Field::BaseBuckets) != 0 {
                live.push(current + 1);
            }
            let mut level_files = Vec::new();
            for l in live {
                let mut file = match prefix {
                    Some(p) => OpenOptions::new()
                        .read(true)
                        .write(true)
                        .create_new(true)
                        .open(level_path(p, l))?,
                    None => mmap::ephemeral_file(&format!("gdblite-level{l}"))?,
                };
                let mut remaining = get(l, Field::FileSize);
                while remaining > 0 {
                    let chunk = source()?;
                    if chunk.len() as u64 != remaining.min(SNAPSHOT_CHUNK) {
                        return Err(Error::InvalidFormat("snapshot chunk size"));
                    }
                    file.write_all(&chunk)?;
                    remaining -= chunk.len() as u64;
                }
                level_files.push((l, file));
            }
            match prefix {
                Some(p) => {
                    drop(level_files);
                    // Written last, so a failed restore leaves no table behind.
                    fs::write(suffixed(p, "metadata"), &meta)?;
                    Writer::open(Some(p), options)
                }
                None => {
                    let mut meta_file = mmap::ephemeral_file("gdblite-metadata")?;
                    meta_file.write_all(&meta)?;
                    Writer::open_files(None, options, meta_file, level_files)
                }
            }
        })();
        if result.is_err()
            && let Some(p) = prefix
        {
            let _ = remove(p);
        }
        result
    }

    fn live_levels(&self) -> impl Iterator<Item = &Level> {
        let current = self.shared.level.load(Ordering::Relaxed);
        [current, current + 1]
            .into_iter()
            .filter_map(|l| self.shared.level(l))
    }

    fn is_expanding(&self) -> bool {
        let current = self.shared.level.load(Ordering::Relaxed);
        self.shared.level(current + 1).is_some()
    }

    fn apply(
        &mut self,
        kind: Kind,
        key: u64,
        key2: u64,
        bit: bool,
        values: &[u64],
        fault: bool,
    ) -> Result<()> {
        if key == 0 {
            return Err(Error::InvalidArgument("key must be non-zero"));
        }
        if kind != Kind::Delete {
            if !(1..=MAX_VALUES).contains(&values.len()) {
                return Err(Error::InvalidArgument(
                    "insert and replace take 1 to 8 values",
                ));
            }
            if values.contains(&0) {
                return Err(Error::InvalidArgument("values must be non-zero"));
            }
        }
        let shared = Arc::clone(&self.shared);
        let h = hash(key, key2);
        let mut level = shared.level.load(Ordering::Relaxed);
        let lvl = loop {
            let lvl = shared.level(level).expect("live level is mapped");
            if lvl.header(lvl.base_bucket(h)) != HEADER_EXTENDED {
                break lvl;
            }
            level += 1;
        };
        self.append(
            lvl,
            lvl.base_bucket(h),
            kind,
            key,
            key2,
            bit,
            values,
            true,
            fault,
        )?;
        self.check_expansion()
    }

    /// Appends a record to the end of the chain starting at bucket `head`.
    /// With `dedupe`, an operation that would change nothing is skipped.
    #[allow(clippy::too_many_arguments)]
    fn append(
        &mut self,
        lvl: &Level,
        head: u64,
        kind: Kind,
        key: u64,
        key2: u64,
        bit: bool,
        values: &[u64],
        dedupe: bool,
        fault: bool,
    ) -> Result<()> {
        let mut state = KeyState::default();
        let mut b = head;
        loop {
            let bytes = lvl
                .read(b)
                .ok_or(Error::InvalidFormat("overflow bucket past end of file"))?;
            let header = record::bucket_header(&bytes);
            let parsed = record::parse_bucket(&bytes, |r| {
                if r.key == key && r.key2 == key2 {
                    state.apply(r);
                }
            });
            if header != HEADER_NO_OVERFLOW {
                let next = lvl.overflow_bucket(header);
                if next <= b {
                    return Err(Error::InvalidFormat("overflow chain links backward"));
                }
                b = next;
                continue;
            }
            if dedupe && state.is_noop(kind, bit, values) {
                return Ok(());
            }
            // Clear a partly written record, and anything else past the end.
            if parsed.corrupt || bytes[parsed.end..].iter().any(|&x| x != 0) {
                self.write(lvl, b, parsed.end, &[0; BUCKET_BYTES][parsed.end..], false);
            }
            let mut rec = [0u8; MAX_RECORD_BYTES];
            let previous = parsed.last_key == Some((key, key2));
            let n = record::encode(&mut rec, kind, previous, bit, key, key2, values);
            if parsed.end + n <= BUCKET_BYTES {
                self.write(lvl, b, parsed.end, &rec[..n], fault);
                return Ok(());
            }
            let next = self.allocate_overflow(lvl, b)?;
            let n = record::encode(&mut rec, kind, false, bit, key, key2, values);
            self.write(lvl, next, BUCKET_HEADER_BYTES, &rec[..n], fault);
            return Ok(());
        }
    }

    /// Stores `data` at `offset` in bucket `b`, a word at a time in order, so a
    /// concurrent reader sees a prefix of it.
    fn write(&mut self, lvl: &Level, b: u64, offset: usize, data: &[u8], fault: bool) {
        let words = lvl.bucket(b).expect("bucket is allocated");
        let end = offset + data.len();
        let first = offset / 8;
        for (w, word) in words.iter().enumerate().take(end.div_ceil(8)).skip(first) {
            let mut bytes = word.load(Ordering::Relaxed).to_ne_bytes();
            for (i, byte) in bytes.iter_mut().enumerate() {
                let pos = w * 8 + i;
                if (offset..end).contains(&pos) {
                    *byte = data[pos - offset];
                }
            }
            if fault && self.fault.as_mut().is_some_and(|f| f()) {
                return;
            }
            word.store(u64::from_ne_bytes(bytes), Ordering::Release);
        }
    }

    fn set_header(&mut self, lvl: &Level, b: u64, header: u32) {
        self.write(lvl, b, 0, &header.to_le_bytes(), false);
    }

    /// Links a new overflow bucket to the end of a chain and returns it.
    fn allocate_overflow(&mut self, lvl: &Level, last: u64) -> Result<u64> {
        let count = self.meta.get(lvl.index, Field::OverflowBuckets) + 1;
        if count >= u64::from(HEADER_EXTENDED) {
            return Err(Error::TableFull);
        }
        let bucket = lvl.overflow_bucket(count as u32);
        let needed = (bucket + 1) * BUCKET_BYTES as u64;
        if needed > lvl.allocated.load(Ordering::Relaxed) {
            self.grow(lvl, needed)?;
        }
        // Counted before it is linked, so a crash can never hand it out twice.
        self.meta.set(lvl.index, Field::OverflowBuckets, count);
        self.set_header(lvl, last, count as u32);
        Ok(bucket)
    }

    /// Extends a level's file to hold at least `needed` bytes.  The mapping
    /// already covers the new space, so readers keep using the same addresses.
    fn grow(&mut self, lvl: &Level, needed: u64) -> Result<()> {
        let reserved = lvl.map.len();
        if needed > reserved {
            return Err(Error::TableFull);
        }
        let current = lvl.allocated.load(Ordering::Relaxed);
        let size = round_up_to_page(needed.max(current + current / 8)).min(reserved);
        mmap::allocate(
            self.files[lvl.index].as_ref().expect("level file is open"),
            size,
        )?;
        self.meta.set(lvl.index, Field::FileSize, size);
        lvl.allocated.store(size, Ordering::Release);
        Ok(())
    }

    /// Opens, sizes and maps a level's file, reserving room for it to grow.
    fn map_level(&mut self, l: usize) -> Result<()> {
        let base = base_buckets(l);
        if self.meta.get(l, Field::BaseBuckets) == 0 {
            self.meta.set(l, Field::BaseBuckets, base);
        } else if self.meta.get(l, Field::BaseBuckets) != base {
            return Err(Error::InvalidFormat("level base bucket count"));
        }
        let file = match (self.files[l].take(), &self.prefix) {
            (Some(file), _) => file,
            (None, Some(p)) => OpenOptions::new()
                .read(true)
                .write(true)
                .create(true)
                .truncate(false)
                .open(level_path(p, l))?,
            (None, None) => mmap::ephemeral_file(&format!("gdblite-level{l}"))?,
        };
        let base_bytes = base.saturating_mul(BUCKET_BYTES as u64);
        let initial = round_up_to_page(base_bytes + base_bytes / OVERFLOW_PREALLOCATE_DIVISOR);
        let mut size = file.metadata()?.len();
        if size < initial {
            mmap::allocate(&file, initial)?;
            size = initial;
        }
        let reserve = round_up_to_page(
            base_bytes
                .saturating_mul(2)
                .max(self.options.min_level_reserve)
                .max(size),
        );
        let level = Box::new(Level {
            index: l,
            base_buckets: base,
            map: Mapping::new(&file, reserve)?,
            allocated: AtomicU64::new(size),
        });
        self.meta.set(l, Field::FileSize, size);
        let ptr = &*level as *const Level as *mut Level;
        self.shared.owned.lock().unwrap().push(level);
        self.shared.levels[l].store(ptr, Ordering::Release);
        self.files[l] = Some(file);
        Ok(())
    }

    fn check_expansion(&mut self) -> Result<()> {
        self.inserts += 1;
        if self.is_expanding() {
            if self.inserts.is_multiple_of(INSERTIONS_PER_EXPAND_STEP) {
                self.expand_step()?;
            }
            return Ok(());
        }
        let shared = Arc::clone(&self.shared);
        let current = shared.level.load(Ordering::Relaxed);
        let lvl = shared.level(current).expect("current level is mapped");
        let overflow = self.meta.get(current, Field::OverflowBuckets);
        if current + 1 < MAX_LEVELS
            && overflow as f64 > lvl.base_buckets as f64 * OVERFLOW_HIGHWATER
        {
            self.map_level(current + 1)?;
            self.expand_step()?;
        }
        Ok(())
    }

    /// Moves the next few buckets of the current level to the next level.
    fn expand_step(&mut self) -> Result<()> {
        let shared = Arc::clone(&self.shared);
        let l = shared.level.load(Ordering::Relaxed);
        let from = shared.level(l).expect("current level is mapped");
        let to = shared.level(l + 1).expect("next level is mapped");
        for _ in 0..BUCKETS_PER_EXPAND_STEP {
            let b = self.meta.get(l, Field::ExpandedBuckets);
            if b >= from.base_buckets {
                break;
            }
            // If it is already marked, a crash came before the count was saved.
            if from.header(b) != HEADER_EXTENDED {
                self.copy_bucket(from, to, b)?;
                self.set_header(from, b, HEADER_EXTENDED);
            }
            self.meta.set(l, Field::ExpandedBuckets, b + 1);
        }
        if self.meta.get(l, Field::ExpandedBuckets) >= from.base_buckets {
            self.shared.level.store(l + 1, Ordering::Release);
            self.shared.levels[l].store(ptr::null_mut(), Ordering::Release);
            self.retiring.push_back((l, Instant::now()));
        }
        Ok(())
    }

    /// Merges the records in the chain at `from` bucket `b` and writes them to
    /// the two `to` buckets that share its hash prefix.
    fn copy_bucket(&mut self, from: &Level, to: &Level, b: u64) -> Result<()> {
        // Only bucket `b` feeds these, so anything in them is left over from an
        // interrupted copy.  Its overflow buckets are abandoned.
        for target in [2 * b, 2 * b + 1] {
            self.write(to, target, 0, &[0; BUCKET_BYTES], false);
        }
        let mut entries: BTreeMap<(u64, u64), (bool, Vec<u64>)> = BTreeMap::new();
        let mut c = b;
        loop {
            let bytes = from
                .read(c)
                .ok_or(Error::InvalidFormat("overflow bucket past end of file"))?;
            record::parse_bucket(&bytes, |r| {
                let (bit, values) = entries.entry((r.key, r.key2)).or_default();
                record::merge(values, bit, r);
            });
            let header = record::bucket_header(&bytes);
            if header == HEADER_NO_OVERFLOW {
                break;
            }
            let next = from.overflow_bucket(header);
            if next <= c {
                return Err(Error::InvalidFormat("overflow chain links backward"));
            }
            c = next;
        }
        for ((key, key2), (bit, values)) in entries {
            let head = to.base_bucket(hash(key, key2));
            for chunk in values.chunks(MAX_VALUES) {
                self.append(to, head, Kind::Insert, key, key2, bit, chunk, false, false)?;
            }
        }
        Ok(())
    }

    /// Unmaps a fully expanded level and deletes its file.
    fn retire(&mut self, l: usize) -> Result<()> {
        let owned = self.shared.owned.lock().unwrap();
        if let Some(lvl) = owned.iter().find(|lvl| lvl.index == l) {
            lvl.map.make_inaccessible()?;
        }
        drop(owned);
        self.files[l] = None;
        self.remove_level_file(l)
    }

    fn remove_level_file(&self, l: usize) -> Result<()> {
        if let Some(p) = &self.prefix {
            match fs::remove_file(level_path(p, l)) {
                Err(e) if e.kind() != io::ErrorKind::NotFound => return Err(e.into()),
                _ => {}
            }
        }
        Ok(())
    }
}

/// Copies `len` bytes at `offset` from a mapping, a word at a time.
fn copy_words(map: &Mapping, offset: u64, len: u64, out: &mut Vec<u8>) {
    out.clear();
    for o in (offset..offset + len).step_by(8) {
        // SAFETY: the range is aligned and within the file.
        out.extend_from_slice(&unsafe { map.word(o) }.load(Ordering::Relaxed).to_ne_bytes());
    }
}
