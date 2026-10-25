use std::collections::BTreeMap;
use std::path::PathBuf;
use std::sync::atomic::AtomicBool;

use super::*;

const BIG_SIZE: usize = 10_000;

/// A small deterministic generator so tests need no dependencies.
struct XorShift(u64);

impl XorShift {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }

    fn one_in(&mut self, n: u64) -> bool {
        self.next().is_multiple_of(n)
    }

    /// A non-zero value of 1 to 60 random bits, so small and large are common.
    fn skewed60(&mut self) -> u64 {
        let bits = 1 + self.next() % 60;
        (self.next() & ((1 << bits) - 1)).max(1)
    }
}

/// A temporary directory removed on drop.
struct TempDir(PathBuf);

impl TempDir {
    fn new(name: &str) -> Self {
        let dir = std::env::temp_dir().join(format!("gdblite-dt-{}-{name}", std::process::id()));
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

type Records = BTreeMap<(u64, u64), (bool, Vec<u64>)>;

/// Random records with distinct keys, about half of them 128 bit.
fn random_records(rng: &mut XorShift, n: usize, avoid: &Records) -> Records {
    let mut records = Records::new();
    while records.len() < n {
        let key = (
            rng.skewed60(),
            if rng.one_in(2) { rng.skewed60() } else { 0 },
        );
        if records.contains_key(&key) || avoid.contains_key(&key) {
            continue;
        }
        let bit = rng.one_in(2);
        let values = (0..1 + rng.next() % 8).map(|_| rng.skewed60()).collect();
        records.insert(key, (bit, values));
    }
    records
}

fn insert_all(w: &mut Writer, records: &Records) {
    for (&(key, key2), (bit, values)) in records {
        w.insert(key, key2, *bit, values).unwrap();
    }
}

fn check_all(r: &Reader, records: &Records) {
    for (&(key, key2), (bit, values)) in records {
        let found = r
            .get(key, key2)
            .unwrap_or_else(|| panic!("{key} {key2} missing"));
        assert_eq!(found.bit, *bit, "{key} {key2}");
        assert_eq!(&found.values, values, "{key} {key2}");
    }
}

fn found(bit: bool, values: &[u64]) -> Option<Lookup> {
    Some(Lookup {
        bit,
        values: values.to_vec(),
    })
}

fn memory_table() -> Writer {
    Writer::open(None, Options::default()).unwrap()
}

fn level(w: &Writer) -> usize {
    w.shared.level.load(Ordering::Relaxed)
}

/// Distinct 64 bit keys that share a bucket at level 0.
fn colliding_keys(n: usize) -> Vec<u64> {
    let target = hash(1, 0) >> 57;
    (1..)
        .filter(|&k| hash(k, 0) >> 57 == target)
        .take(n)
        .collect()
}

// Ports of the C++ tests.

#[test]
fn simple() {
    let mut w = memory_table();
    assert_eq!(w.get(1234, 0), None);
    w.insert(1234, 0, true, &[12, 1313131313, 14]).unwrap();
    assert_eq!(w.get(1234, 0), found(true, &[12, 1313131313, 14]));
}

#[test]
fn big() {
    let mut w = memory_table();
    let records = random_records(&mut XorShift(1), BIG_SIZE, &Records::new());
    insert_all(&mut w, &records);
    check_all(&w.reader(), &records);
    assert!(level(&w) > 0, "the table expanded");
}

#[test]
fn insert_previous() {
    let mut w = memory_table();
    w.insert(1234, 0, true, &[12, 1313131313]).unwrap();
    w.insert(1234, 0, true, &[13, 1313131314]).unwrap();
    assert_eq!(
        w.get(1234, 0),
        found(true, &[12, 1313131313, 13, 1313131314])
    );
}

#[test]
fn insert_with_key2() {
    let mut w = memory_table();
    w.insert(1234, 0, true, &[12, 1313131313]).unwrap();
    w.insert(1234, 5678, false, &[10, 1313131310]).unwrap();
    w.insert(1234, 0, true, &[13, 1313131314]).unwrap();
    assert_eq!(
        w.get(1234, 0),
        found(true, &[12, 1313131313, 13, 1313131314])
    );
    assert_eq!(w.get(1234, 5678), found(false, &[10, 1313131310]));
}

#[test]
fn replace_previous() {
    let mut w = memory_table();
    w.insert(1234, 0, false, &[12, 1313131313]).unwrap();
    w.replace(1234, 0, true, &[13, 1313131314]).unwrap();
    assert_eq!(w.get(1234, 0), found(true, &[13, 1313131314]));
}

#[test]
fn replace_with_key2() {
    let mut w = memory_table();
    w.insert(1234, 0, true, &[12, 1313131313]).unwrap();
    w.insert(1234, 5678, true, &[10, 1313131310]).unwrap();
    w.replace(1234, 0, true, &[13, 1313131314]).unwrap();
    assert_eq!(w.get(1234, 0), found(true, &[13, 1313131314]));
    assert_eq!(w.get(1234, 5678), found(true, &[10, 1313131310]));
}

#[test]
fn delete_previous() {
    let mut w = memory_table();
    w.insert(1234, 0, true, &[12, 1313131313]).unwrap();
    w.delete(1234, 0).unwrap();
    assert_eq!(w.get(1234, 0), None);
}

#[test]
fn delete_with_key2() {
    let mut w = memory_table();
    w.insert(1234, 0, true, &[12, 1313131313]).unwrap();
    w.insert(1234, 5678, true, &[10, 1313131310]).unwrap();
    w.delete(1234, 0).unwrap();
    assert_eq!(w.get(1234, 0), None);
    assert_eq!(w.get(1234, 5678), found(true, &[10, 1313131310]));
}

#[test]
fn corruption() {
    let mut rng = XorShift(2);
    let mut w = memory_table();
    let good = random_records(&mut rng, BIG_SIZE, &Records::new());
    let corrupt = random_records(&mut rng, BIG_SIZE, &good);
    let (first, second): (Records, Records) = good
        .iter()
        .map(|(k, v)| (*k, v.clone()))
        .partition(|_| rng.one_in(2));
    insert_all(&mut w, &first);
    // Each word store has a 1% chance of being the last, as if the writer
    // crashed and was restarted.
    let mut fault_rng = XorShift(3);
    w.fault = Some(Box::new(move || fault_rng.one_in(100)));
    for (&(key, key2), (bit, values)) in &corrupt {
        w.apply(Kind::Insert, key, key2, *bit, values, true)
            .unwrap();
    }
    w.fault = None;
    insert_all(&mut w, &second);
    check_all(&w.reader(), &good);
}

#[test]
fn snapshot() {
    let dir = TempDir::new("snapshot");
    let records = random_records(&mut XorShift(4), BIG_SIZE, &Records::new());
    let mut chunks = Vec::new();
    {
        let mut w = Writer::open(Some(&dir.prefix("pre")), Options::default()).unwrap();
        insert_all(&mut w, &records);
        w.snapshot(|chunk| {
            chunks.push(chunk.to_vec());
            Ok(())
        })
        .unwrap();
    }
    assert!(chunks.iter().all(|c| c.len() as u64 <= SNAPSHOT_CHUNK));
    let post = dir.prefix("post");
    let mut source = chunks.clone().into_iter();
    let w = Writer::restore(Some(&post), Options::default(), || {
        Ok(source.next().unwrap_or_default())
    })
    .unwrap();
    check_all(&w.reader(), &records);
    drop(w);

    let mut source = chunks.clone().into_iter();
    let again = Writer::restore(Some(&post), Options::default(), || {
        Ok(source.next().unwrap_or_default())
    });
    assert!(matches!(again, Err(Error::Io(e)) if e.kind() == io::ErrorKind::AlreadyExists));

    // Into memory.
    let mut source = chunks.into_iter();
    let mut w = Writer::restore(None, Options::default(), || {
        Ok(source.next().unwrap_or_default())
    })
    .unwrap();
    check_all(&w.reader(), &records);
    w.insert(1, 2, false, &[3]).unwrap();
    assert_eq!(w.get(1, 2).unwrap().values.last(), Some(&3));
}

// Tests for the bugs fixed in the port.

/// A delete used to claim 8 values, swallowing the records after it.
#[test]
fn delete_does_not_hide_later_records() {
    let keys = colliding_keys(3);
    let mut w = memory_table();
    w.insert(keys[0], 0, false, &[1]).unwrap();
    w.delete(keys[0], 0).unwrap();
    w.insert(keys[1], 0, false, &[2]).unwrap();
    w.insert(keys[2], 0, false, &[3]).unwrap();
    assert_eq!(w.get(keys[0], 0), None);
    assert_eq!(w.get(keys[1], 0), found(false, &[2]));
    assert_eq!(w.get(keys[2], 0), found(false, &[3]));
}

/// Re-inserting a record used to overwrite it in place and cut off the rest of
/// the bucket.
#[test]
fn repeated_insert_keeps_later_records() {
    let keys = colliding_keys(2);
    let mut w = memory_table();
    w.insert(keys[0], 0, false, &[1]).unwrap();
    w.insert(keys[1], 0, false, &[5]).unwrap();
    w.insert(keys[0], 0, false, &[1]).unwrap();
    w.insert(keys[0], 0, false, &[2]).unwrap();
    assert_eq!(w.get(keys[0], 0), found(false, &[1, 2]));
    assert_eq!(w.get(keys[1], 0), found(false, &[5]));
}

#[test]
fn repeating_the_last_operation_is_a_noop() {
    let mut w = memory_table();
    w.insert(7, 0, false, &[1, 2]).unwrap();
    w.insert(7, 0, false, &[1, 2]).unwrap();
    assert_eq!(w.get(7, 0), found(false, &[1, 2]));
    w.insert(7, 0, true, &[1, 2]).unwrap();
    assert_eq!(w.get(7, 0), found(true, &[1, 2, 1, 2]));
    w.delete(7, 0).unwrap();
    w.delete(7, 0).unwrap();
    assert_eq!(w.get(7, 0), None);
}

/// Values used to overflow the caller's 8 element array on lookup, and fail an
/// assertion when expansion merged them into one record.
#[test]
fn many_values_survive_expansion() {
    let mut w = memory_table();
    let expected: Vec<u64> = (1..=40).collect();
    for chunk in expected.chunks(3) {
        w.insert(99, 0, true, chunk).unwrap();
    }
    let avoid = Records::from([((99, 0), (true, expected.clone()))]);
    let records = random_records(&mut XorShift(5), BIG_SIZE, &avoid);
    insert_all(&mut w, &records);
    assert!(level(&w) >= 2);
    assert_eq!(w.get(99, 0), found(true, &expected));
}

/// Readers run while the writer inserts, expands levels and grows files.
#[test]
fn readers_during_growth() {
    const KEYS: u64 = 30_000;
    const HOT_KEY: u64 = u64::MAX;
    let mut w = memory_table();
    let progress = AtomicU64::new(0);
    let done = AtomicBool::new(false);
    std::thread::scope(|s| {
        for t in 0..4 {
            let r = w.reader();
            let (progress, done) = (&progress, &done);
            s.spawn(move || {
                let mut rng = XorShift(10 + t);
                let mut values = Vec::new();
                while !done.load(Ordering::Relaxed) {
                    let n = progress.load(Ordering::Acquire);
                    if n > 0 {
                        let k = 1 + rng.next() % n;
                        assert_eq!(r.get_into(k, 0, &mut values), Some(false), "key {k}");
                        assert_eq!(values, [k]);
                    }
                    // The hot key's values are 1, 2, 3, ... added 8 at a time.
                    if r.get_into(HOT_KEY, 0, &mut values).is_some() {
                        assert_eq!(values.len() % MAX_VALUES, 0);
                        assert!(values.iter().copied().eq(1..=values.len() as u64));
                    }
                }
            });
        }
        let mut hot = 1;
        for k in 1..=KEYS {
            w.insert(k, 0, false, &[k]).unwrap();
            progress.store(k, Ordering::Release);
            if k % 4 == 0 {
                let values: Vec<u64> = (hot..hot + MAX_VALUES as u64).collect();
                w.insert(HOT_KEY, 0, false, &values).unwrap();
                hot += MAX_VALUES as u64;
            }
        }
        done.store(true, Ordering::Relaxed);
    });
    assert!(level(&w) >= 4);
    let lvl = w.shared.level(level(&w)).unwrap();
    let initial = round_up_to_page(lvl.base_buckets * 128 * 51 / 50);
    assert!(
        lvl.allocated.load(Ordering::Relaxed) > initial,
        "a file grew"
    );
}

#[test]
fn reopen() {
    let dir = TempDir::new("reopen");
    let prefix = dir.prefix("t");
    let mut rng = XorShift(6);
    let first = random_records(&mut rng, BIG_SIZE, &Records::new());
    {
        let mut w = Writer::open(Some(&prefix), Options::default()).unwrap();
        insert_all(&mut w, &first);
    }
    let mut w = Writer::open(Some(&prefix), Options::default()).unwrap();
    check_all(&w.reader(), &first);

    // Close in the middle of an expansion.
    let mut more = Records::new();
    while !w.is_expanding() {
        let batch = random_records(&mut rng, 100, &first);
        insert_all(&mut w, &batch);
        more.extend(batch);
    }
    drop(w);
    let mut w = Writer::open(Some(&prefix), Options::default()).unwrap();
    assert!(w.is_expanding());
    check_all(&w.reader(), &first);
    check_all(&w.reader(), &more);
    let last = random_records(&mut rng, BIG_SIZE, &first);
    insert_all(&mut w, &last);
    check_all(&w.reader(), &last);
}

/// A crash between marking a bucket moved and saving the count used to stall
/// expansion forever, and a crash mid-copy relied on buggy deduplication.
#[test]
fn interrupted_expansion_resumes() {
    let mut w = memory_table();
    let mut rng = XorShift(7);
    let mut records = Records::new();
    while !w.is_expanding() {
        let batch = random_records(&mut rng, 50, &records);
        insert_all(&mut w, &batch);
        records.extend(batch);
    }
    let shared = Arc::clone(&w.shared);
    let l = level(&w);
    let (from, to) = (shared.level(l).unwrap(), shared.level(l + 1).unwrap());
    let b = w.meta.get(l, Field::ExpandedBuckets);
    // Bucket b: copied, then a crash before it was marked.
    w.copy_bucket(from, to, b).unwrap();
    // Bucket b + 1: copied and marked, then a crash before the count was saved.
    w.copy_bucket(from, to, b + 1).unwrap();
    w.set_header(from, b + 1, HEADER_EXTENDED);

    while w.is_expanding() {
        w.tick().unwrap();
    }
    assert_eq!(level(&w), l + 1);
    check_all(&w.reader(), &records);
}

#[test]
fn retired_level_is_unmapped_and_deleted() {
    let dir = TempDir::new("retire");
    let prefix = dir.prefix("t");
    let options = Options {
        retire_delay: Duration::ZERO,
        ..Options::default()
    };
    let mut w = Writer::open(Some(&prefix), options).unwrap();
    let mut rng = XorShift(8);
    let mut records = Records::new();
    while level(&w) == 0 {
        let batch = random_records(&mut rng, 50, &records);
        insert_all(&mut w, &batch);
        records.extend(batch);
    }
    assert!(w.shared.level(0).is_none(), "new lookups skip level 0");
    assert!(
        level_path(&prefix, 0).exists(),
        "kept until the delay passes"
    );
    w.tick().unwrap();
    assert!(!level_path(&prefix, 0).exists());
    check_all(&w.reader(), &records);
}

#[test]
fn create_size_starts_at_a_larger_level() {
    let dir = TempDir::new("create-size");
    let prefix = dir.prefix("t");
    let options = Options {
        create_size: 1 << 20,
        ..Options::default()
    };
    let mut w = Writer::open(Some(&prefix), options).unwrap();
    assert_eq!(level(&w), 6);
    let records = random_records(&mut XorShift(9), 1000, &Records::new());
    insert_all(&mut w, &records);
    drop(w);
    let w = Writer::open(Some(&prefix), Options::default()).unwrap();
    assert_eq!(level(&w), 6);
    check_all(&w.reader(), &records);
}

#[test]
fn second_writer_is_locked_out() {
    let dir = TempDir::new("lock");
    let prefix = dir.prefix("t");
    let w = Writer::open(Some(&prefix), Options::default()).unwrap();
    let second = Writer::open(Some(&prefix), Options::default());
    assert!(matches!(second, Err(Error::Io(e)) if e.kind() == io::ErrorKind::WouldBlock));
    drop(w);
    Writer::open(Some(&prefix), Options::default()).unwrap();
}

#[test]
fn invalid_arguments() {
    let mut w = memory_table();
    for result in [
        w.insert(0, 0, false, &[1]),
        w.insert(1, 0, false, &[]),
        w.insert(1, 0, false, &[1; 9]),
        w.replace(1, 0, false, &[1, 0]),
        w.delete(0, 5),
    ] {
        assert!(matches!(result, Err(Error::InvalidArgument(_))));
    }
}

#[test]
fn remove_and_exists() {
    let dir = TempDir::new("remove");
    let prefix = dir.prefix("t");
    assert!(!exists(&prefix));
    drop(Writer::open(Some(&prefix), Options::default()).unwrap());
    assert!(exists(&prefix));
    remove(&prefix).unwrap();
    assert!(!exists(&prefix));
    assert_eq!(fs::read_dir(&dir.0).unwrap().count(), 0, "no files left");
}
